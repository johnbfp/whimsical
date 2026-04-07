from __future__ import annotations

import asyncio
import base64
import json
import logging
import pathlib
import re as _re
import shutil
import subprocess
import sys
import time
from contextlib import asynccontextmanager

from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import StreamingResponse

from app.core.config import settings
from app.models.schemas import (
    AgentEvent,
    ErrorCode,
    ErrorResponse,
    MemoryRecallRequest,
    MemoryRecallResponse,
    MemoryWriteRequest,
    ModelSwitchRequest,
    ModelSwitchResponse,
    PluginEnableResponse,
    PluginInstallRequest,
    PluginManifest,
    TaskCancelResponse,
    TaskCreateRequest,
    TaskResponse,
)
from app.services.agent_runtime import AgentRuntime
from app.services.event_bus import EventBus
from app.services.executor_client import ExecutorClient
from app.services.memory_service import MemoryService
from app.services.model_gateway import ModelGateway
from app.services.planner import Planner
from app.services.plugin_manager import PluginManager
from app.services.state_store import StateStore

logging.basicConfig(
    level=getattr(logging, settings.log_level, logging.INFO),
    format="%(asctime)s %(levelname)s %(name)s [%(funcName)s] %(message)s",
)
logger = logging.getLogger(__name__)


@asynccontextmanager
async def lifespan(app: FastAPI):
    await model_gateway.try_ollama_then_fallback()
    logger.info("Agent runtime started, model_provider=%s", model_gateway.provider_name)

    # 自动注册并启用所有内置工具插件
    _default_plugins = [
        PluginManifest(plugin_id="builtin.echo",           version="1.0.0", tool_name="echo",           permissions=["tool.execute"], timeout_ms=1000),
        PluginManifest(plugin_id="builtin.file_write",     version="1.0.0", tool_name="file_write",     permissions=["tool.execute"], timeout_ms=5000),
        PluginManifest(plugin_id="builtin.file_read",      version="1.0.0", tool_name="file_read",      permissions=["tool.execute"], timeout_ms=3000),
        PluginManifest(plugin_id="builtin.http_fetch",     version="1.0.0", tool_name="http_fetch",     permissions=["tool.execute"], timeout_ms=15000),
        PluginManifest(plugin_id="builtin.shell_exec",     version="1.0.0", tool_name="shell_exec",     permissions=["tool.execute"], timeout_ms=30000),
        PluginManifest(plugin_id="builtin.json_transform", version="1.0.0", tool_name="json_transform", permissions=["tool.execute"], timeout_ms=2000),
        PluginManifest(plugin_id="builtin.memory_write",   version="1.0.0", tool_name="memory.write",   permissions=["tool.execute"], timeout_ms=2000),
        PluginManifest(plugin_id="builtin.memory_read",    version="1.0.0", tool_name="memory.read",    permissions=["tool.execute"], timeout_ms=2000),
        PluginManifest(plugin_id="builtin.memory_search",  version="1.0.0", tool_name="memory.search",  permissions=["tool.execute"], timeout_ms=2000),
        PluginManifest(plugin_id="builtin.memory_delete",  version="1.0.0", tool_name="memory.delete",  permissions=["tool.execute"], timeout_ms=2000),
        PluginManifest(plugin_id="builtin.list_dir",       version="1.0.0", tool_name="list_dir",       permissions=["tool.execute"], timeout_ms=3000),
        PluginManifest(plugin_id="builtin.edit_file",      version="1.0.0", tool_name="edit_file",      permissions=["tool.execute"], timeout_ms=5000),
        PluginManifest(plugin_id="builtin.grep_search",    version="1.0.0", tool_name="grep_search",    permissions=["tool.execute"], timeout_ms=10000),
        PluginManifest(plugin_id="builtin.run_tests",      version="1.0.0", tool_name="run_tests",      permissions=["tool.execute"], timeout_ms=65000),
    ]
    for _p in _default_plugins:
        plugin_manager.install(_p)
        plugin_manager.enable(_p.plugin_id)
    logger.info("Registered %d built-in tool plugins", len(_default_plugins))

    yield


app = FastAPI(title=settings.app_name, lifespan=lifespan)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

state_store = StateStore()
event_bus = EventBus()
model_gateway = ModelGateway(
    settings.default_local_model,
    ollama_base_url=settings.ollama_base_url,
    cloud_api_key=settings.cloud_api_key,
    cloud_base_url=settings.cloud_base_url,
)
planner = Planner(model_gateway)
memory_service = MemoryService()
plugin_manager = PluginManager()
executor_client = ExecutorClient(settings.executor_url)
runtime = AgentRuntime(
    state_store=state_store,
    planner=planner,
    model_gateway=model_gateway,
    memory_service=memory_service,
    executor_client=executor_client,
    plugin_manager=plugin_manager,
    event_bus=event_bus,
)


@app.post("/agents/tasks", response_model=TaskResponse)
async def create_task(request: TaskCreateRequest) -> TaskResponse:
    task = await runtime.create_task(request)
    asyncio.create_task(runtime.execute_task(task.task_id))
    return task


@app.get("/agents/tasks/{task_id}", response_model=TaskResponse)
async def get_task(task_id: str) -> TaskResponse:
    task = state_store.get_task(task_id)
    if not task:
        raise HTTPException(
            status_code=404,
            detail=ErrorResponse(code=ErrorCode.TASK_NOT_FOUND, detail="Task not found").model_dump(),
        )
    return task


@app.post("/agents/tasks/{task_id}/cancel", response_model=TaskCancelResponse)
async def cancel_task(task_id: str) -> TaskCancelResponse:
    cancelled = await runtime.cancel_task(task_id)
    if not cancelled:
        raise HTTPException(
            status_code=404,
            detail=ErrorResponse(code=ErrorCode.TASK_NOT_FOUND, detail="Task not found").model_dump(),
        )
    return TaskCancelResponse(task_id=task_id, cancelled=True)


@app.websocket("/agents/stream")
async def stream(websocket: WebSocket, task_id: str) -> None:
    await websocket.accept()
    queue = event_bus.subscribe(task_id)
    try:
        while True:
            event: AgentEvent = await queue.get()
            await websocket.send_json(event.model_dump(mode="json"))
    except WebSocketDisconnect:
        event_bus.unsubscribe(task_id, queue)


@app.post("/memory/recall", response_model=MemoryRecallResponse)
async def recall_memory(request: MemoryRecallRequest) -> MemoryRecallResponse:
    items = memory_service.recall(request.tenant_id, request.user_id, request.top_k, request.query)
    return MemoryRecallResponse(items=items)


@app.post("/memory/write")
async def write_memory(request: MemoryWriteRequest) -> dict:
    memory_service.kv_write(request.tenant_id, request.user_id, request.key, request.value)
    return {"ok": True, "key": request.key}


@app.get("/memory/read")
async def read_memory(tenant_id: str = "default", key: str = "") -> dict:
    value = memory_service.kv_read(tenant_id, key)
    if value is None:
        raise HTTPException(status_code=404, detail=f"Key '{key}' not found")
    return {"key": key, "value": value}


@app.get("/memory/history/{user_id}")
async def get_history(user_id: str, last_n: int = 20) -> list:
    return memory_service.get_history_dicts(user_id, last_n)


@app.get("/memory/search")
async def search_memory(tenant_id: str = "default", query: str = "", top_k: int = 5) -> dict:
    results = memory_service.kv_search(tenant_id, query, top_k)
    return {"query": query, "results": results}


@app.post("/plugins/install")
async def install_plugin(request: PluginInstallRequest) -> dict[str, str]:
    plugin_manager.install(request.manifest)
    return {"status": "installed", "plugin_id": request.manifest.plugin_id}


@app.get("/plugins")
async def list_plugins() -> list[dict]:
    return plugin_manager.list_plugins()


@app.post("/plugins/{plugin_id}/enable", response_model=PluginEnableResponse)
async def enable_plugin(plugin_id: str) -> PluginEnableResponse:
    enabled = plugin_manager.enable(plugin_id)
    if not enabled:
        raise HTTPException(
            status_code=404,
            detail=ErrorResponse(code=ErrorCode.PLUGIN_NOT_FOUND, detail="Plugin not found").model_dump(),
        )
    return PluginEnableResponse(plugin_id=plugin_id, enabled=True)


@app.post("/plugins/{plugin_id}/disable", response_model=PluginEnableResponse)
async def disable_plugin(plugin_id: str) -> PluginEnableResponse:
    enabled = plugin_manager.disable(plugin_id)
    if not enabled:
        raise HTTPException(
            status_code=404,
            detail=ErrorResponse(code=ErrorCode.PLUGIN_NOT_FOUND, detail="Plugin not found").model_dump(),
        )
    return PluginEnableResponse(plugin_id=plugin_id, enabled=False)


@app.delete("/plugins/{plugin_id}")
async def uninstall_plugin(plugin_id: str) -> dict[str, str]:
    removed = plugin_manager.uninstall(plugin_id)
    if not removed:
        raise HTTPException(
            status_code=404,
            detail=ErrorResponse(code=ErrorCode.PLUGIN_NOT_FOUND, detail="Plugin not found").model_dump(),
        )
    return {"status": "uninstalled", "plugin_id": plugin_id}


@app.post("/models/switch", response_model=ModelSwitchResponse)
async def switch_model(request: ModelSwitchRequest) -> ModelSwitchResponse:
    try:
        model_gateway.switch(request.provider, request.model_name)
    except ValueError as exc:
        raise HTTPException(
            status_code=400,
            detail=ErrorResponse(code=ErrorCode.PROVIDER_UNSUPPORTED, detail=str(exc)).model_dump(),
        ) from exc

    return ModelSwitchResponse(provider=model_gateway.provider_name, model_name=model_gateway.model_name)


@app.get("/healthz")
async def healthz() -> dict[str, str]:
    return {"status": "ok"}


# ── Workspace (coding) APIs ────────────────────────────────────────────

WORKSPACE_DIR = pathlib.Path(settings.workspace_dir).resolve()
WORKSPACE_DIR.mkdir(parents=True, exist_ok=True)

# Binary / image extensions for base64 encoding
_IMAGE_EXTS = {".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp", ".svg", ".ico"}
_BINARY_EXTS = _IMAGE_EXTS | {".pdf", ".zip", ".apk", ".jar", ".class", ".wasm"}


def _safe_path(rel: str) -> pathlib.Path:
    """Resolve a relative path within the workspace; raise 403 on escape."""
    target = (WORKSPACE_DIR / rel).resolve()
    if not str(target).startswith(str(WORKSPACE_DIR)):
        raise HTTPException(status_code=403, detail="Path outside workspace")
    return target


@app.get("/workspace/tree")
async def workspace_tree(path: str = ".", depth: int = 3) -> dict:
    """List directory entries (recursive up to `depth`)."""
    root = _safe_path(path)
    if not root.exists():
        raise HTTPException(status_code=404, detail="Path not found")

    def _walk(p: pathlib.Path, d: int) -> list[dict]:
        entries = []
        try:
            for item in sorted(p.iterdir()):
                name = item.name
                # skip hidden, __pycache__, node_modules, .git, target
                if name.startswith(".") or name in {"__pycache__", "node_modules", ".git", "target", "build"}:
                    continue
                rel = str(item.relative_to(WORKSPACE_DIR)).replace("\\", "/")
                entry: dict = {"name": name, "path": rel, "is_dir": item.is_dir()}
                if item.is_file():
                    entry["size"] = item.stat().st_size
                elif item.is_dir() and d > 1:
                    entry["children"] = _walk(item, d - 1)
                entries.append(entry)
        except PermissionError:
            pass
        return entries

    return {"path": path, "entries": _walk(root, depth)}


@app.get("/workspace/file")
async def workspace_read_file(path: str, start_line: int = 0, end_line: int = 0) -> dict:
    """Read a file's content, optionally a line range. Returns base64 for binary."""
    target = _safe_path(path)
    if not target.is_file():
        raise HTTPException(status_code=404, detail="File not found")

    ext = target.suffix.lower()
    if ext in _BINARY_EXTS:
        data = target.read_bytes()
        return {
            "path": path,
            "binary": True,
            "is_image": ext in _IMAGE_EXTS,
            "mime": f"image/{ext.lstrip('.')}" if ext in _IMAGE_EXTS else "application/octet-stream",
            "size": len(data),
            "content_base64": base64.b64encode(data).decode(),
        }

    try:
        text = target.read_text(encoding="utf-8", errors="replace")
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))

    lines = text.splitlines(keepends=True)
    total = len(lines)
    if start_line > 0 and end_line > 0:
        lines = lines[max(0, start_line - 1):end_line]

    return {
        "path": path,
        "binary": False,
        "content": "".join(lines),
        "total_lines": total,
        "start_line": start_line if start_line > 0 else 1,
        "end_line": end_line if end_line > 0 else total,
        "language": _guess_language(ext),
    }


@app.post("/workspace/file")
async def workspace_write_file(request: dict) -> dict:
    """Write or create a file. Returns diff if file existed."""
    path = request.get("path", "")
    content = request.get("content", "")
    if not path:
        raise HTTPException(status_code=400, detail="path is required")
    target = _safe_path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    old_content: str | None = None
    if target.is_file():
        try:
            old_content = target.read_text(encoding="utf-8", errors="replace")
        except Exception:
            pass
    target.write_text(content, encoding="utf-8")
    result: dict = {"ok": True, "path": path, "size": len(content)}
    if old_content is not None:
        result["had_previous"] = True
        result["old_lines"] = old_content.count("\n") + 1
        result["new_lines"] = content.count("\n") + 1
    return result


@app.post("/workspace/run")
async def workspace_run_command(request: dict) -> dict:
    """Run a shell command in the workspace directory."""
    cmd = request.get("command", "")
    timeout = min(request.get("timeout", 30), 120)
    if not cmd.strip():
        raise HTTPException(status_code=400, detail="command is required")
    enc = "cp936" if sys.platform == "win32" else "utf-8"
    t0 = time.monotonic()
    try:
        proc = subprocess.run(
            cmd, shell=True, capture_output=True,
            timeout=timeout, cwd=str(WORKSPACE_DIR),
        )
        elapsed = round(time.monotonic() - t0, 2)
        stdout = proc.stdout.decode(enc, errors="replace") if proc.stdout else ""
        stderr = proc.stderr.decode(enc, errors="replace") if proc.stderr else ""
        return {
            "exit_code": proc.returncode,
            "stdout": stdout[:16000],
            "stderr": stderr[:8000],
            "elapsed_s": elapsed,
        }
    except subprocess.TimeoutExpired:
        return {"exit_code": -1, "stdout": "", "stderr": f"Command timed out (>{timeout}s)", "elapsed_s": timeout}
    except Exception as e:
        return {"exit_code": -1, "stdout": "", "stderr": str(e), "elapsed_s": 0}


@app.post("/workspace/run/stream")
async def workspace_run_stream(request: dict):
    """Run command and stream output line-by-line as SSE."""
    cmd = request.get("command", "")
    timeout = min(request.get("timeout", 60), 120)
    if not cmd.strip():
        raise HTTPException(status_code=400, detail="command is required")

    async def _generate():
        enc = "cp936" if sys.platform == "win32" else "utf-8"
        proc = await asyncio.create_subprocess_shell(
            cmd, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
            cwd=str(WORKSPACE_DIR),
        )
        try:
            async def _read_stream(stream, tag):
                while True:
                    line = await asyncio.wait_for(stream.readline(), timeout=timeout)
                    if not line:
                        break
                    text = line.decode(enc, errors="replace").rstrip("\n")
                    yield f"data: {json.dumps({'tag': tag, 'line': text})}\n\n"
            async for chunk in _read_stream(proc.stdout, "stdout"):
                yield chunk
            async for chunk in _read_stream(proc.stderr, "stderr"):
                yield chunk
        except asyncio.TimeoutError:
            proc.kill()
            yield f"data: {json.dumps({'tag': 'error', 'line': f'Timed out (>{timeout}s)'})}\n\n"
        code = await proc.wait()
        yield f"data: {json.dumps({'tag': 'exit', 'code': code})}\n\n"

    return StreamingResponse(_generate(), media_type="text/event-stream")


@app.get("/workspace/search")
async def workspace_search(
    query: str, path: str = ".",
    is_regex: bool = False,
    include: str = "",
    max_results: int = 200,
) -> dict:
    """Search for text in workspace files. Supports regex and glob include filter."""
    search_dir = _safe_path(path)
    pattern = None
    if is_regex:
        try:
            pattern = _re.compile(query, _re.IGNORECASE)
        except _re.error as e:
            raise HTTPException(status_code=400, detail=f"Invalid regex: {e}")
    include_exts = set()
    if include:
        for part in include.split(","):
            part = part.strip()
            if part.startswith("."):
                include_exts.add(part.lower())
            elif part.startswith("*."):
                include_exts.add(part[1:].lower())
    results = []
    _skip = {"__pycache__", "node_modules", ".git", "target", "build", ".gradle"}
    for f in search_dir.rglob("*"):
        if not f.is_file() or f.stat().st_size > 500_000:
            continue
        if any(p in f.parts for p in _skip) or f.name.startswith("."):
            continue
        if include_exts and f.suffix.lower() not in include_exts:
            continue
        try:
            text = f.read_text(encoding="utf-8", errors="ignore")
            for i, line in enumerate(text.splitlines(), 1):
                match = False
                if pattern:
                    match = bool(pattern.search(line))
                else:
                    match = query.lower() in line.lower()
                if match:
                    results.append({
                        "file": str(f.relative_to(WORKSPACE_DIR)).replace("\\", "/"),
                        "line": i,
                        "text": line.strip()[:300],
                    })
                    if len(results) >= max_results:
                        break
        except Exception:
            pass
        if len(results) >= max_results:
            break
    return {"query": query, "total": len(results), "matches": results}


# ── File management APIs ───────────────────────────────────────────────

@app.post("/workspace/mkdir")
async def workspace_mkdir(request: dict) -> dict:
    """Create a directory."""
    path = request.get("path", "")
    if not path:
        raise HTTPException(status_code=400, detail="path is required")
    target = _safe_path(path)
    target.mkdir(parents=True, exist_ok=True)
    return {"ok": True, "path": path}


@app.delete("/workspace/file")
async def workspace_delete_file(path: str) -> dict:
    """Delete a file or empty directory."""
    target = _safe_path(path)
    if not target.exists():
        raise HTTPException(status_code=404, detail="Path not found")
    if target.is_dir():
        if any(target.iterdir()):
            raise HTTPException(status_code=400, detail="Directory not empty. Use recursive=true")
        target.rmdir()
    else:
        target.unlink()
    return {"ok": True, "path": path, "deleted": True}


@app.post("/workspace/rename")
async def workspace_rename(request: dict) -> dict:
    """Rename / move a file or directory."""
    old = request.get("old_path", "")
    new = request.get("new_path", "")
    if not old or not new:
        raise HTTPException(status_code=400, detail="old_path and new_path are required")
    src = _safe_path(old)
    dst = _safe_path(new)
    if not src.exists():
        raise HTTPException(status_code=404, detail="Source not found")
    if dst.exists():
        raise HTTPException(status_code=409, detail="Destination already exists")
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.move(str(src), str(dst))
    return {"ok": True, "old_path": old, "new_path": new}


@app.post("/workspace/edit")
async def workspace_edit_file(request: dict) -> dict:
    """Replace old_string with new_string in a file (like Claude Code edit)."""
    path = request.get("path", "")
    old_string = request.get("old_string", "")
    new_string = request.get("new_string", "")
    if not path or not old_string:
        raise HTTPException(status_code=400, detail="path and old_string are required")
    target = _safe_path(path)
    if not target.is_file():
        raise HTTPException(status_code=404, detail="File not found")
    content = target.read_text(encoding="utf-8", errors="replace")
    count = content.count(old_string)
    if count == 0:
        raise HTTPException(status_code=400, detail="old_string not found in file")
    if count > 1:
        raise HTTPException(status_code=400, detail=f"old_string found {count} times; must be unique")
    new_content = content.replace(old_string, new_string, 1)
    target.write_text(new_content, encoding="utf-8")
    return {
        "ok": True, "path": path,
        "old_lines": content.count("\n") + 1,
        "new_lines": new_content.count("\n") + 1,
    }


@app.get("/workspace/stat")
async def workspace_stat(path: str) -> dict:
    """Get file/dir metadata."""
    target = _safe_path(path)
    if not target.exists():
        raise HTTPException(status_code=404, detail="Path not found")
    st = target.stat()
    return {
        "path": path,
        "is_dir": target.is_dir(),
        "is_file": target.is_file(),
        "size": st.st_size,
        "modified": st.st_mtime,
        "language": _guess_language(target.suffix.lower()) if target.is_file() else None,
    }


def _guess_language(ext: str) -> str:
    return {
        ".py": "python", ".js": "javascript", ".ts": "typescript",
        ".kt": "kotlin", ".java": "java", ".rs": "rust",
        ".vue": "vue", ".html": "html", ".css": "css",
        ".json": "json", ".yaml": "yaml", ".yml": "yaml",
        ".toml": "toml", ".md": "markdown", ".xml": "xml",
        ".gradle": "groovy", ".kts": "kotlin", ".sh": "bash",
        ".bat": "batch", ".sql": "sql", ".c": "c", ".cpp": "cpp",
        ".h": "c", ".go": "go", ".rb": "ruby", ".swift": "swift",
    }.get(ext, "text")
