from __future__ import annotations

import asyncio
import logging
from contextlib import asynccontextmanager

from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware

from app.core.config import settings
from app.models.schemas import (
    AgentEvent,
    ErrorCode,
    ErrorResponse,
    MemoryRecallRequest,
    MemoryRecallResponse,
    ModelSwitchRequest,
    ModelSwitchResponse,
    PluginEnableResponse,
    PluginInstallRequest,
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
    items = memory_service.recall(request.tenant_id, request.user_id, request.top_k)
    return MemoryRecallResponse(items=items)


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
