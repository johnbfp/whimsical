from __future__ import annotations

from datetime import datetime, timezone
from enum import Enum
from typing import Any

from pydantic import BaseModel, Field


# ---------------------------------------------------------------------------
# Error codes
# ---------------------------------------------------------------------------

class ErrorCode(str, Enum):
    TASK_NOT_FOUND = "TASK_NOT_FOUND"
    PLUGIN_NOT_FOUND = "PLUGIN_NOT_FOUND"
    TOOL_NOT_ALLOWED = "TOOL_NOT_ALLOWED"
    PERMISSION_DENIED = "PERMISSION_DENIED"
    MODEL_UNAVAILABLE = "MODEL_UNAVAILABLE"
    PROVIDER_UNSUPPORTED = "PROVIDER_UNSUPPORTED"
    SCHEMA_VALIDATION_ERROR = "SCHEMA_VALIDATION_ERROR"
    EXECUTOR_TIMEOUT = "EXECUTOR_TIMEOUT"
    EXECUTOR_ERROR = "EXECUTOR_ERROR"
    INTERNAL_ERROR = "INTERNAL_ERROR"


class ErrorResponse(BaseModel):
    code: ErrorCode
    detail: str


class TaskState(str, Enum):
    CREATED = "CREATED"
    PLANNING = "PLANNING"
    RUNNING = "RUNNING"
    WAITING_TOOL = "WAITING_TOOL"
    COMPLETED = "COMPLETED"
    FAILED = "FAILED"
    CANCELLED = "CANCELLED"


class ToolStep(BaseModel):
    tool_name: str
    input: dict[str, Any] = Field(default_factory=dict)


class Plan(BaseModel):
    task_breakdown: list[str]
    selected_tools: list[str]
    execution_steps: list[ToolStep]
    reflection_hint: str


class TaskCreateRequest(BaseModel):
    tenant_id: str = "default"
    user_id: str
    prompt: str


class TaskResponse(BaseModel):
    task_id: str
    tenant_id: str
    user_id: str
    prompt: str
    state: TaskState
    plan: Plan | None = None
    result: str | None = None
    error: str | None = None
    model_provider: str = "local"
    model_name: str = ""
    created_at: datetime
    updated_at: datetime


class TaskCancelResponse(BaseModel):
    task_id: str
    cancelled: bool


class MemoryRecallRequest(BaseModel):
    tenant_id: str = "default"
    user_id: str
    query: str
    top_k: int = 5


class MemoryRecallResponse(BaseModel):
    items: list[str]


class PluginManifest(BaseModel):
    plugin_id: str
    version: str
    tool_name: str
    permissions: list[str]
    input_schema: dict[str, Any] = Field(default_factory=dict)
    timeout_ms: int = 4000


class PluginInstallRequest(BaseModel):
    manifest: PluginManifest


class PluginEnableResponse(BaseModel):
    plugin_id: str
    enabled: bool


class ModelSwitchRequest(BaseModel):
    provider: str
    model_name: str


class ModelSwitchResponse(BaseModel):
    provider: str
    model_name: str


class AgentEvent(BaseModel):
    event_type: str
    task_id: str
    payload: dict[str, Any] = Field(default_factory=dict)
    timestamp: datetime = Field(default_factory=lambda: datetime.now(timezone.utc))
