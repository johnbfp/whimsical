"""Lightweight Python mock of the Rust executor for local testing.
Listens on port 8088 and handles tool execute / status / cancel requests.
"""

from __future__ import annotations

import json
import uuid
from datetime import datetime, timezone

from fastapi import FastAPI, HTTPException
from pydantic import BaseModel
from typing import Any

app = FastAPI(title="Mock Executor")

ALLOWED_TOOLS = {"echo", "file_read", "file_write", "http_fetch", "shell_exec", "json_transform"}

records: dict[str, dict] = {}


class ExecuteRequest(BaseModel):
    task_id: str
    tool_name: str
    input: Any
    permissions: list[str]
    idempotency_key: str
    timeout_ms: int = 4000
    trace_id: str = ""


@app.get("/healthz")
async def healthz():
    return {"status": "ok"}


@app.post("/execute")
async def execute_tool(req: ExecuteRequest):
    if req.tool_name not in ALLOWED_TOOLS:
        raise HTTPException(status_code=403, detail="tool not allowed")

    if "tool.execute" not in req.permissions:
        raise HTTPException(status_code=403, detail="missing permission tool.execute")

    # Idempotency check
    for rec in records.values():
        if rec["idempotency_key"] == req.idempotency_key:
            return {
                "execution_id": rec["execution_id"],
                "task_id": rec["task_id"],
                "tool_name": rec["tool_name"],
                "status": rec["status"],
                "result": rec["result"],
                "trace_id": rec["trace_id"],
            }

    execution_id = str(uuid.uuid4())
    now = datetime.now(timezone.utc).isoformat()

    # Simulate tool execution
    result = _run_tool(req.tool_name, req.input)

    record = {
        "execution_id": execution_id,
        "task_id": req.task_id,
        "tool_name": req.tool_name,
        "status": "COMPLETED",
        "result": result,
        "error": None,
        "trace_id": req.trace_id,
        "idempotency_key": req.idempotency_key,
        "created_at": now,
        "updated_at": now,
    }
    records[execution_id] = record

    return {
        "execution_id": execution_id,
        "task_id": req.task_id,
        "tool_name": req.tool_name,
        "status": "COMPLETED",
        "result": result,
        "trace_id": req.trace_id,
    }


@app.get("/status/{execution_id}")
async def get_status(execution_id: str):
    rec = records.get(execution_id)
    if not rec:
        raise HTTPException(status_code=404, detail="not found")
    return rec


@app.post("/cancel/{execution_id}")
async def cancel_execution(execution_id: str):
    rec = records.get(execution_id)
    if not rec:
        raise HTTPException(status_code=404, detail="not found")
    rec["status"] = "CANCELLED"
    rec["updated_at"] = datetime.now(timezone.utc).isoformat()
    return {"execution_id": execution_id, "cancelled": True}


def _run_tool(tool_name: str, tool_input: Any) -> str:
    """Simulate tool execution and return a string result."""
    if tool_name == "echo":
        text = tool_input if isinstance(tool_input, str) else tool_input.get("text", str(tool_input))
        return f"Echo: {text}"

    if tool_name == "file_read":
        path = tool_input.get("path", "unknown") if isinstance(tool_input, dict) else str(tool_input)
        return f"[mock] Contents of {path}: (simulated file data)"

    if tool_name == "file_write":
        path = tool_input.get("path", "unknown") if isinstance(tool_input, dict) else "unknown"
        return f"[mock] Written to {path}"

    if tool_name == "http_fetch":
        url = tool_input.get("url", "unknown") if isinstance(tool_input, dict) else str(tool_input)
        return json.dumps({"mock": True, "url": url, "status": 200, "body": "<html>mock</html>"})

    if tool_name == "shell_exec":
        cmd = tool_input.get("command", "?") if isinstance(tool_input, dict) else str(tool_input)
        return f"[mock] Shell output for '{cmd}': OK"

    if tool_name == "json_transform":
        return json.dumps({"transformed": True, "input": tool_input})

    return f"[mock] {tool_name} executed"


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8088)
