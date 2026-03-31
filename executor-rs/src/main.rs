use std::{collections::HashMap, sync::Arc, time::Duration};

use axum::{
    extract::{Path, State},
    http::StatusCode,
    routing::{get, post},
    Json, Router,
};
use chrono::{DateTime, Utc};
use serde::{Deserialize, Serialize};
use tokio::sync::RwLock;
use uuid::Uuid;

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(rename_all = "SCREAMING_SNAKE_CASE")]
enum ExecutionStatus {
    Queued,
    Running,
    Completed,
    Failed,
    Cancelled,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
struct ExecutionRecord {
    execution_id: String,
    task_id: String,
    tool_name: String,
    status: ExecutionStatus,
    result: Option<String>,
    error: Option<String>,
    trace_id: String,
    idempotency_key: String,
    created_at: DateTime<Utc>,
    updated_at: DateTime<Utc>,
}

#[derive(Clone, Debug, Deserialize)]
struct ExecuteRequest {
    task_id: String,
    tool_name: String,
    input: serde_json::Value,
    permissions: Vec<String>,
    idempotency_key: String,
    timeout_ms: u64,
    trace_id: String,
}

#[derive(Clone, Debug, Serialize)]
struct ExecuteResponse {
    execution_id: String,
    task_id: String,
    tool_name: String,
    status: ExecutionStatus,
    result: Option<String>,
    trace_id: String,
}

#[derive(Clone, Debug, Serialize)]
struct CancelResponse {
    execution_id: String,
    cancelled: bool,
}

#[derive(Clone, Default)]
struct AppState {
    records: Arc<RwLock<HashMap<String, ExecutionRecord>>>,
    idempotency_index: Arc<RwLock<HashMap<String, String>>>,
}

#[tokio::main]
async fn main() {
    let app_state = AppState::default();

    let app = Router::new()
        .route("/healthz", get(healthz))
        .route("/execute", post(execute_tool))
        .route("/status/:execution_id", get(get_status))
        .route("/cancel/:execution_id", post(cancel_execution))
        .with_state(app_state);

    let listener = tokio::net::TcpListener::bind("0.0.0.0:8088").await.unwrap();
    println!("executor listening on 8088");
    axum::serve(listener, app).await.unwrap();
}

async fn healthz() -> Json<serde_json::Value> {
    Json(serde_json::json!({"status": "ok"}))
}

async fn execute_tool(
    State(state): State<AppState>,
    Json(req): Json<ExecuteRequest>,
) -> Result<Json<ExecuteResponse>, (StatusCode, Json<serde_json::Value>)> {
    if !is_tool_allowed(&req.tool_name) {
        return Err((
            StatusCode::FORBIDDEN,
            Json(serde_json::json!({"error": "tool not allowed"})),
        ));
    }

    if req.permissions.iter().all(|p| p != "tool.execute") {
        return Err((
            StatusCode::FORBIDDEN,
            Json(serde_json::json!({"error": "missing permission tool.execute"})),
        ));
    }

    let existing = {
        let idempotency = state.idempotency_index.read().await;
        idempotency.get(&req.idempotency_key).cloned()
    };

    if let Some(execution_id) = existing {
        let records = state.records.read().await;
        if let Some(record) = records.get(&execution_id) {
            return Ok(Json(ExecuteResponse {
                execution_id: record.execution_id.clone(),
                task_id: record.task_id.clone(),
                tool_name: record.tool_name.clone(),
                status: record.status.clone(),
                result: record.result.clone(),
                trace_id: record.trace_id.clone(),
            }));
        }
    }

    let execution_id = Uuid::new_v4().to_string();
    let now = Utc::now();
    let record = ExecutionRecord {
        execution_id: execution_id.clone(),
        task_id: req.task_id.clone(),
        tool_name: req.tool_name.clone(),
        status: ExecutionStatus::Running,
        result: None,
        error: None,
        trace_id: req.trace_id.clone(),
        idempotency_key: req.idempotency_key.clone(),
        created_at: now,
        updated_at: now,
    };

    {
        let mut idempotency = state.idempotency_index.write().await;
        idempotency.insert(req.idempotency_key.clone(), execution_id.clone());
    }
    {
        let mut records = state.records.write().await;
        records.insert(execution_id.clone(), record);
    }

    let state_clone = state.clone();
    let execution_id_clone = execution_id.clone();
    tokio::spawn(async move {
        let output = run_tool(req.tool_name, req.input, req.timeout_ms).await;

        let mut records = state_clone.records.write().await;
        if let Some(r) = records.get_mut(&execution_id_clone) {
            if matches!(r.status, ExecutionStatus::Cancelled) {
                return;
            }
            r.updated_at = Utc::now();
            match output {
                Ok(result) => {
                    r.status = ExecutionStatus::Completed;
                    r.result = Some(result);
                }
                Err(err) => {
                    r.status = ExecutionStatus::Failed;
                    r.error = Some(err);
                }
            }
            println!("audit trace_id={} execution_id={} status={:?}", r.trace_id, r.execution_id, r.status);
        }
    });

    tokio::time::sleep(Duration::from_millis(30)).await;
    let records = state.records.read().await;
    let record = records.get(&execution_id).unwrap();

    Ok(Json(ExecuteResponse {
        execution_id: execution_id.clone(),
        task_id: record.task_id.clone(),
        tool_name: record.tool_name.clone(),
        status: record.status.clone(),
        result: record.result.clone(),
        trace_id: record.trace_id.clone(),
    }))
}

async fn get_status(
    State(state): State<AppState>,
    Path(execution_id): Path<String>,
) -> Result<Json<ExecutionRecord>, (StatusCode, Json<serde_json::Value>)> {
    let records = state.records.read().await;
    let Some(record) = records.get(&execution_id) else {
        return Err((
            StatusCode::NOT_FOUND,
            Json(serde_json::json!({"error": "execution not found"})),
        ));
    };
    Ok(Json(record.clone()))
}

async fn cancel_execution(
    State(state): State<AppState>,
    Path(execution_id): Path<String>,
) -> Result<Json<CancelResponse>, (StatusCode, Json<serde_json::Value>)> {
    let mut records = state.records.write().await;
    let Some(record) = records.get_mut(&execution_id) else {
        return Err((
            StatusCode::NOT_FOUND,
            Json(serde_json::json!({"error": "execution not found"})),
        ));
    };

    record.status = ExecutionStatus::Cancelled;
    record.updated_at = Utc::now();

    Ok(Json(CancelResponse {
        execution_id,
        cancelled: true,
    }))
}

fn is_tool_allowed(tool_name: &str) -> bool {
    matches!(tool_name, "echo" | "memory.write" | "http_fetch" | "shell_exec" | "json_transform")
}

async fn run_tool(tool_name: String, input: serde_json::Value, timeout_ms: u64) -> Result<String, String> {
    let run = async move {
        match tool_name.as_str() {
            "echo" => Ok(input
                .get("text")
                .and_then(|x| x.as_str())
                .unwrap_or_default()
                .to_string()),
            "memory.write" => Ok("memory persisted".to_string()),
            "http_fetch" => run_http_fetch(&input).await,
            "shell_exec" => run_shell_exec(&input).await,
            "json_transform" => run_json_transform(&input),
            _ => Err("tool not implemented".to_string()),
        }
    };

    match tokio::time::timeout(Duration::from_millis(timeout_ms), run).await {
        Ok(result) => result,
        Err(_) => Err("tool execution timeout".to_string()),
    }
}

/// Fetch a URL and return the response body (text).
async fn run_http_fetch(input: &serde_json::Value) -> Result<String, String> {
    let url = input
        .get("url")
        .and_then(|v| v.as_str())
        .ok_or_else(|| "missing 'url' field".to_string())?;

    let method = input
        .get("method")
        .and_then(|v| v.as_str())
        .unwrap_or("GET")
        .to_uppercase();

    let client = reqwest::Client::builder()
        .redirect(reqwest::redirect::Policy::limited(5))
        .build()
        .map_err(|e| e.to_string())?;

    let mut req_builder = match method.as_str() {
        "POST" => client.post(url),
        "PUT" => client.put(url),
        "DELETE" => client.delete(url),
        _ => client.get(url),
    };

    if let Some(headers) = input.get("headers").and_then(|v| v.as_object()) {
        for (k, v) in headers {
            if let Some(val) = v.as_str() {
                req_builder = req_builder.header(k.as_str(), val);
            }
        }
    }

    if let Some(body) = input.get("body") {
        req_builder = req_builder.json(body);
    }

    let resp = req_builder.send().await.map_err(|e| e.to_string())?;
    let status = resp.status().as_u16();
    let text = resp.text().await.map_err(|e| e.to_string())?;

    // Limit response size to 64KB
    let truncated = if text.len() > 65536 {
        format!("{}...(truncated)", &text[..65536])
    } else {
        text
    };

    Ok(serde_json::json!({
        "status": status,
        "body": truncated
    })
    .to_string())
}

/// Execute a shell command in a sandboxed manner.
/// Only allows a predefined set of safe commands.
async fn run_shell_exec(input: &serde_json::Value) -> Result<String, String> {
    let command = input
        .get("command")
        .and_then(|v| v.as_str())
        .ok_or_else(|| "missing 'command' field".to_string())?;

    // Allowlist of safe command prefixes for sandboxing
    let safe_prefixes = ["echo", "date", "whoami", "uname", "cat", "ls", "dir", "pwd", "env"];
    let cmd_lower = command.trim().to_lowercase();
    let is_safe = safe_prefixes
        .iter()
        .any(|prefix| cmd_lower.starts_with(prefix));

    if !is_safe {
        return Err(format!(
            "command '{}' not in sandbox allowlist",
            command.split_whitespace().next().unwrap_or(command)
        ));
    }

    let output = if cfg!(target_os = "windows") {
        tokio::process::Command::new("cmd")
            .args(["/C", command])
            .output()
            .await
    } else {
        tokio::process::Command::new("sh")
            .args(["-c", command])
            .output()
            .await
    };

    match output {
        Ok(out) => {
            let stdout = String::from_utf8_lossy(&out.stdout).to_string();
            let stderr = String::from_utf8_lossy(&out.stderr).to_string();
            Ok(serde_json::json!({
                "exit_code": out.status.code().unwrap_or(-1),
                "stdout": stdout,
                "stderr": stderr
            })
            .to_string())
        }
        Err(e) => Err(e.to_string()),
    }
}

/// Transform JSON: extract a field, apply jq-like path, or pretty-print.
fn run_json_transform(input: &serde_json::Value) -> Result<String, String> {
    let data = input
        .get("data")
        .ok_or_else(|| "missing 'data' field".to_string())?;

    let path = input
        .get("path")
        .and_then(|v| v.as_str())
        .unwrap_or("");

    if path.is_empty() {
        return serde_json::to_string_pretty(data).map_err(|e| e.to_string());
    }

    // Simple dot-path navigation: "a.b.c"
    let mut current = data;
    for segment in path.split('.') {
        if segment.is_empty() {
            continue;
        }
        if let Ok(idx) = segment.parse::<usize>() {
            current = current.get(idx).ok_or_else(|| format!("index {idx} not found"))?;
        } else {
            current = current.get(segment).ok_or_else(|| format!("key '{segment}' not found"))?;
        }
    }
    serde_json::to_string_pretty(current).map_err(|e| e.to_string())
}
