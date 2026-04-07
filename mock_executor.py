"""Lightweight Python mock of the Rust executor for local testing.
Listens on port 8088 and handles tool execute / status / cancel requests.
"""

from __future__ import annotations

import json
import os
import pathlib
import uuid
from datetime import datetime, timezone

from fastapi import FastAPI, HTTPException
from pydantic import BaseModel
from typing import Any

app = FastAPI(title="Mock Executor")

# 沙盒目录：所有 file_read / file_write 操作都限定在这个目录内
SANDBOX_DIR = pathlib.Path(r"E:\new_work_space\whimsical_ideas\sandbox")
SANDBOX_DIR.mkdir(parents=True, exist_ok=True)

# 记忆KV存储：与 backend MemoryService 共享同一目录和格式
MEMORY_KV_DIR = pathlib.Path(r"E:\new_work_space\whimsical_ideas\sandbox\memory\kv")
MEMORY_KV_DIR.mkdir(parents=True, exist_ok=True)


def _mem_safe(s: str) -> str:
    import re
    return re.sub(r"[^a-zA-Z0-9_\-]", "_", s)[:80]

ALLOWED_TOOLS = {
    "echo", "file_read", "file_write", "http_fetch", "shell_exec", "json_transform",
    "memory.write", "memory.read", "memory.search", "memory.delete",
    "list_dir", "edit_file", "run_tests", "grep_search",
    "create_dir", "delete_file", "rename_file",
}

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
    result = _run_tool(req.tool_name, req.input, req.task_id)

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


def _run_tool(tool_name: str, tool_input: Any, task_id: str = "") -> str:
    """Execute a tool and return a string result."""
    if tool_name == "echo":
        if isinstance(tool_input, str):
            text = tool_input
        elif isinstance(tool_input, dict):
            # 兼容 LLM 可能生成的各种键名
            text = tool_input.get("text") or tool_input.get("message") or tool_input.get("content") or str(tool_input)
        else:
            text = str(tool_input)
        return f"Echo: {text}"

    if tool_name == "file_read":
        rel_path = tool_input.get("path", "unknown") if isinstance(tool_input, dict) else str(tool_input)
        target = (SANDBOX_DIR / rel_path).resolve()
        # 安全检查：只允许读取沙盒目录内的文件
        if not str(target).startswith(str(SANDBOX_DIR.resolve())):
            return f"[error] 路径不在沙盒范围内: {rel_path}"
        try:
            return target.read_text(encoding="utf-8")
        except FileNotFoundError:
            return f"[error] 文件不存在: {rel_path}"
        except Exception as e:
            return f"[error] 读取失败: {e}"

    if tool_name == "file_write":
        rel_path = tool_input.get("path", "output.txt") if isinstance(tool_input, dict) else "output.txt"
        content = tool_input.get("content", "") if isinstance(tool_input, dict) else str(tool_input)
        target = (SANDBOX_DIR / rel_path).resolve()
        # 安全检查：只允许写入沙盒目录内的文件
        if not str(target).startswith(str(SANDBOX_DIR.resolve())):
            return f"[error] 路径不在沙盒范围内: {rel_path}"
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content, encoding="utf-8")
        return f"[ok] 已写入 {rel_path}（沙盒路径: {target}）"

    if tool_name == "http_fetch":
        import urllib.request as _ur
        import urllib.error as _ue
        url = tool_input.get("url", "") if isinstance(tool_input, dict) else str(tool_input)
        method = (tool_input.get("method", "GET") if isinstance(tool_input, dict) else "GET").upper()
        headers = tool_input.get("headers", {}) if isinstance(tool_input, dict) else {}
        body_data = tool_input.get("body", None) if isinstance(tool_input, dict) else None
        if not url:
            return json.dumps({"error": "url 为空"})
        try:
            encoded_body = body_data.encode() if isinstance(body_data, str) else None
            req = _ur.Request(url, data=encoded_body, method=method)
            req.add_header("User-Agent", "WhimsicalAgent/1.0")
            for k, v in headers.items():
                req.add_header(k, v)
            with _ur.urlopen(req, timeout=10) as resp:
                raw = resp.read(8192).decode("utf-8", errors="replace")
                return json.dumps({"status": resp.status, "url": url, "body": raw[:3000]})
        except _ue.HTTPError as e:
            return json.dumps({"error": f"HTTP {e.code}", "url": url})
        except Exception as e:
            return json.dumps({"error": str(e), "url": url})

    if tool_name == "shell_exec":
        import subprocess, sys
        cmd = tool_input.get("command", "") if isinstance(tool_input, dict) else str(tool_input)
        if not cmd.strip():
            return "[error] 命令为空"
        # Windows 下系统命令输出编码为 cp936（GBK）
        enc = "cp936" if sys.platform == "win32" else "utf-8"
        try:
            proc = subprocess.run(
                cmd, shell=True, capture_output=True,
                timeout=20, cwd=str(SANDBOX_DIR),
            )
            stdout = proc.stdout.decode(enc, errors="replace") if proc.stdout else ""
            stderr = proc.stderr.decode(enc, errors="replace") if proc.stderr else ""
            output = stdout or stderr or "[ok] 命令执行完成（无输出）"
            return output[:3000]
        except subprocess.TimeoutExpired:
            return "[error] 命令执行超时（>20s）"
        except Exception as e:
            return f"[error] {e}"

    if tool_name == "json_transform":
        data = tool_input.get("data", tool_input) if isinstance(tool_input, dict) else tool_input
        expression = tool_input.get("expression", "") if isinstance(tool_input, dict) else ""
        if expression:
            try:
                result_val = eval(expression, {"__builtins__": {}}, {"data": data, "json": json})  # noqa: S307
                return json.dumps({"result": result_val}, ensure_ascii=False)
            except Exception as e:
                return json.dumps({"error": str(e), "data": data})
        return json.dumps(data, ensure_ascii=False, indent=2)

    if tool_name == "memory.write":
        key = tool_input.get("key", "unknown") if isinstance(tool_input, dict) else str(tool_input)
        value = tool_input.get("value", "") if isinstance(tool_input, dict) else ""
        tenant = tool_input.get("tenant_id", "default") if isinstance(tool_input, dict) else "default"
        entry = {
            "key": key, "value": value,
            "timestamp": datetime.now(timezone.utc).isoformat(),
            "tenant_id": tenant, "user_id": task_id,
        }
        kv_dir = MEMORY_KV_DIR / _mem_safe(tenant)
        kv_dir.mkdir(parents=True, exist_ok=True)
        (kv_dir / f"{_mem_safe(key)}.json").write_text(
            json.dumps(entry, ensure_ascii=False, indent=2), encoding="utf-8"
        )
        return json.dumps({"ok": True, "key": key, "written": True})

    if tool_name == "memory.read":
        key = tool_input.get("key", "unknown") if isinstance(tool_input, dict) else str(tool_input)
        tenant = tool_input.get("tenant_id", "default") if isinstance(tool_input, dict) else "default"
        kv_file = MEMORY_KV_DIR / _mem_safe(tenant) / f"{_mem_safe(key)}.json"
        if kv_file.exists():
            try:
                entry = json.loads(kv_file.read_text(encoding="utf-8"))
                return json.dumps({"key": key, "value": entry.get("value", "")})
            except Exception as e:
                return json.dumps({"error": str(e)})
        return json.dumps({"key": key, "value": None, "found": False})

    if tool_name == "memory.search":
        import re as _re
        query = tool_input.get("query", "") if isinstance(tool_input, dict) else str(tool_input)
        tenant = tool_input.get("tenant_id", "default") if isinstance(tool_input, dict) else "default"
        top_k = tool_input.get("top_k", 5) if isinstance(tool_input, dict) else 5
        kv_dir = MEMORY_KV_DIR / _mem_safe(tenant)
        results = []
        if kv_dir.exists():
            q_tokens = set(_re.findall(r"[\w\u4e00-\u9fff]+", query.lower()))
            for f in kv_dir.glob("*.json"):
                try:
                    entry = json.loads(f.read_text(encoding="utf-8"))
                    text = f"{entry.get('key','')} {entry.get('value','')}".lower()
                    score = sum(1 for t in q_tokens if t in text)
                    if score > 0:
                        results.append({"key": entry["key"], "value": entry["value"], "score": score})
                except Exception:
                    pass
        results.sort(key=lambda x: x["score"], reverse=True)
        return json.dumps({"results": results[:top_k]})

    if tool_name == "memory.delete":
        key = tool_input.get("key", "unknown") if isinstance(tool_input, dict) else str(tool_input)
        tenant = tool_input.get("tenant_id", "default") if isinstance(tool_input, dict) else "default"
        kv_file = MEMORY_KV_DIR / _mem_safe(tenant) / f"{_mem_safe(key)}.json"
        deleted = kv_file.exists()
        if deleted:
            kv_file.unlink()
        return json.dumps({"ok": True, "key": key, "deleted": deleted})

    if tool_name == "list_dir":
        rel_path = tool_input.get("path", ".") if isinstance(tool_input, dict) else "."
        target = (SANDBOX_DIR / rel_path).resolve()
        if not str(target).startswith(str(SANDBOX_DIR.resolve())):
            return json.dumps({"error": "路径不在沙盒范围内"})
        if not target.exists():
            return json.dumps({"error": f"目录不存在: {rel_path}"})
        entries = []
        try:
            for item in sorted(target.iterdir()):
                rel = str(item.relative_to(SANDBOX_DIR)).replace("\\", "/")
                entries.append({
                    "name": item.name,
                    "path": rel,
                    "is_dir": item.is_dir(),
                    "size": item.stat().st_size if item.is_file() else 0,
                })
        except Exception as e:
            return json.dumps({"error": str(e)})
        return json.dumps({"path": rel_path, "entries": entries}, ensure_ascii=False)

    if tool_name == "edit_file":
        rel_path = tool_input.get("path", "") if isinstance(tool_input, dict) else ""
        old_str = tool_input.get("old_string", "") if isinstance(tool_input, dict) else ""
        new_str = tool_input.get("new_string", "") if isinstance(tool_input, dict) else ""
        if not rel_path:
            return json.dumps({"error": "path 为空"})
        target = (SANDBOX_DIR / rel_path).resolve()
        if not str(target).startswith(str(SANDBOX_DIR.resolve())):
            return json.dumps({"error": "路径不在沙盒范围内"})
        if not target.exists():
            return json.dumps({"error": f"文件不存在: {rel_path}"})
        try:
            content = target.read_text(encoding="utf-8")
            if old_str not in content:
                return json.dumps({"error": "old_string 未找到", "path": rel_path})
            if content.count(old_str) > 1:
                return json.dumps({"error": "old_string 存在多处匹配，请提供更精确的上下文", "path": rel_path})
            content = content.replace(old_str, new_str, 1)
            target.write_text(content, encoding="utf-8")
            return json.dumps({"ok": True, "path": rel_path, "edited": True})
        except Exception as e:
            return json.dumps({"error": str(e)})

    if tool_name == "grep_search":
        query = tool_input.get("query", "") if isinstance(tool_input, dict) else str(tool_input)
        rel_path = tool_input.get("path", ".") if isinstance(tool_input, dict) else "."
        if not query:
            return json.dumps({"error": "query 为空"})
        search_dir = (SANDBOX_DIR / rel_path).resolve()
        if not str(search_dir).startswith(str(SANDBOX_DIR.resolve())):
            return json.dumps({"error": "路径不在沙盒范围内"})
        results = []
        try:
            glob_pattern = "**/*"
            for f in search_dir.glob(glob_pattern):
                if f.is_file() and f.stat().st_size < 500_000:
                    try:
                        text = f.read_text(encoding="utf-8", errors="ignore")
                        for i, line in enumerate(text.splitlines(), 1):
                            if query.lower() in line.lower():
                                results.append({
                                    "file": str(f.relative_to(SANDBOX_DIR)).replace("\\", "/"),
                                    "line": i,
                                    "text": line.strip()[:200],
                                })
                                if len(results) >= 50:
                                    break
                    except Exception:
                        pass
                if len(results) >= 50:
                    break
        except Exception as e:
            return json.dumps({"error": str(e)})
        return json.dumps({"query": query, "matches": results}, ensure_ascii=False)

    if tool_name == "run_tests":
        test_cmd = tool_input.get("command", "") if isinstance(tool_input, dict) else str(tool_input)
        if not test_cmd.strip():
            # 默认尝试常见测试命令
            test_cmd = "python -m pytest -v --tb=short 2>&1 || echo [test_done]"
        import subprocess, sys
        enc = "cp936" if sys.platform == "win32" else "utf-8"
        try:
            proc = subprocess.run(
                test_cmd, shell=True, capture_output=True,
                timeout=60, cwd=str(SANDBOX_DIR),
            )
            stdout = proc.stdout.decode(enc, errors="replace") if proc.stdout else ""
            stderr = proc.stderr.decode(enc, errors="replace") if proc.stderr else ""
            output = stdout + ("\n--- STDERR ---\n" + stderr if stderr.strip() else "")
            return json.dumps({
                "exit_code": proc.returncode,
                "output": output[:5000],
                "passed": proc.returncode == 0,
            }, ensure_ascii=False)
        except subprocess.TimeoutExpired:
            return json.dumps({"error": "测试执行超时（>60s）", "passed": False})
        except Exception as e:
            return json.dumps({"error": str(e), "passed": False})

    if tool_name == "create_dir":
        rel_path = tool_input.get("path", "") if isinstance(tool_input, dict) else str(tool_input)
        if not rel_path:
            return json.dumps({"error": "path 为空"})
        target = (SANDBOX_DIR / rel_path).resolve()
        if not str(target).startswith(str(SANDBOX_DIR.resolve())):
            return json.dumps({"error": "路径不在沙盒范围内"})
        target.mkdir(parents=True, exist_ok=True)
        return json.dumps({"ok": True, "path": rel_path, "created": True})

    if tool_name == "delete_file":
        rel_path = tool_input.get("path", "") if isinstance(tool_input, dict) else str(tool_input)
        if not rel_path:
            return json.dumps({"error": "path 为空"})
        target = (SANDBOX_DIR / rel_path).resolve()
        if not str(target).startswith(str(SANDBOX_DIR.resolve())):
            return json.dumps({"error": "路径不在沙盒范围内"})
        if not target.exists():
            return json.dumps({"error": f"文件不存在: {rel_path}"})
        if target.is_dir():
            import shutil
            shutil.rmtree(target)
        else:
            target.unlink()
        return json.dumps({"ok": True, "path": rel_path, "deleted": True})

    if tool_name == "rename_file":
        old_path = tool_input.get("old_path", "") if isinstance(tool_input, dict) else ""
        new_path = tool_input.get("new_path", "") if isinstance(tool_input, dict) else ""
        if not old_path or not new_path:
            return json.dumps({"error": "old_path 和 new_path 均为必填"})
        src = (SANDBOX_DIR / old_path).resolve()
        dst = (SANDBOX_DIR / new_path).resolve()
        if not str(src).startswith(str(SANDBOX_DIR.resolve())) or not str(dst).startswith(str(SANDBOX_DIR.resolve())):
            return json.dumps({"error": "路径不在沙盒范围内"})
        if not src.exists():
            return json.dumps({"error": f"源文件不存在: {old_path}"})
        dst.parent.mkdir(parents=True, exist_ok=True)
        import shutil
        shutil.move(str(src), str(dst))
        return json.dumps({"ok": True, "old_path": old_path, "new_path": new_path})

    return f"[mock] {tool_name} executed"


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8088)
