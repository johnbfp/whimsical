from __future__ import annotations

import logging
from datetime import datetime, timezone
from uuid import uuid4

from app.models.schemas import AgentEvent, TaskCreateRequest, TaskResponse, TaskState
from app.services.event_bus import EventBus
from app.services.executor_client import ExecutorClient
from app.services.memory_service import MemoryService
from app.services.model_gateway import ModelGateway
from app.services.planner import Planner
from app.services.plugin_manager import PluginManager
from app.services.state_store import StateStore

logger = logging.getLogger(__name__)


class AgentRuntime:
    def __init__(
        self,
        state_store: StateStore,
        planner: Planner,
        model_gateway: ModelGateway,
        memory_service: MemoryService,
        executor_client: ExecutorClient,
        plugin_manager: PluginManager,
        event_bus: EventBus,
    ) -> None:
        self.state_store = state_store
        self.planner = planner
        self.model_gateway = model_gateway
        self.memory_service = memory_service
        self.executor_client = executor_client
        self.plugin_manager = plugin_manager
        self.event_bus = event_bus
        self.running_executions: dict[str, str] = {}

    async def create_task(self, request: TaskCreateRequest) -> TaskResponse:
        task_id = str(uuid4())
        logger.info("Creating task task_id=%s user=%s tenant=%s", task_id, request.user_id, request.tenant_id)
        now = datetime.now(timezone.utc)
        task = TaskResponse(
            task_id=task_id,
            tenant_id=request.tenant_id,
            user_id=request.user_id,
            prompt=request.prompt,
            state=TaskState.CREATED,
            model_provider=self.model_gateway.provider_name,
            model_name=self.model_gateway.model_name,
            created_at=now,
            updated_at=now,
        )
        self.state_store.create_task(task)
        await self.event_bus.publish(AgentEvent(event_type="task_event", task_id=task_id, payload={"state": task.state}))
        return task

    async def execute_task(self, task_id: str) -> None:
        task = self.state_store.get_task(task_id)
        if not task:
            return
        try:
            self.state_store.update_task_state(task_id, TaskState.PLANNING)
            await self.event_bus.publish(AgentEvent(event_type="state_event", task_id=task_id, payload={"state": TaskState.PLANNING}))

            plan = await self.planner.build_plan(task.prompt)
            self.state_store.set_plan(task_id, plan)
            logger.info("Plan built task_id=%s tools=%s", task_id, plan.selected_tools)
            await self.event_bus.publish(AgentEvent(event_type="task_event", task_id=task_id, payload={"plan": plan.model_dump()}))

            self.state_store.update_task_state(task_id, TaskState.RUNNING)
            await self.event_bus.publish(AgentEvent(event_type="state_event", task_id=task_id, payload={"state": TaskState.RUNNING}))

            final_outputs: list[str] = []
            for step in plan.execution_steps:
                self.state_store.update_task_state(task_id, TaskState.WAITING_TOOL)
                await self.event_bus.publish(AgentEvent(event_type="state_event", task_id=task_id, payload={"state": TaskState.WAITING_TOOL, "tool": step.tool_name}))

                manifest = self.plugin_manager.get_manifest_by_tool(step.tool_name)
                timeout_ms = manifest.timeout_ms if manifest else 4000

                logger.info("Executing tool task_id=%s tool=%s timeout=%d", task_id, step.tool_name, timeout_ms)

                # Validate input against plugin schema before execution
                self.plugin_manager.validate_input(step.tool_name, step.input)

                tool_result = await self.executor_client.execute_tool(task_id, step.tool_name, step.input, timeout_ms)
                execution_id = tool_result.get("execution_id", "")
                if execution_id:
                    self.running_executions[task_id] = execution_id

                output = str(tool_result.get("result", ""))
                final_outputs.append(output)
                logger.info("Tool completed task_id=%s tool=%s exec_id=%s", task_id, step.tool_name, execution_id)
                await self.event_bus.publish(AgentEvent(event_type="tool_event", task_id=task_id, payload=tool_result))

                self.state_store.update_task_state(task_id, TaskState.RUNNING)

            llm_output = await self.model_gateway.provider.chat("\n".join(final_outputs))
            self.state_store.set_result(task_id, llm_output)
            self.memory_service.persist_summary(task.tenant_id, task.user_id, llm_output)

            await self.event_bus.publish(AgentEvent(event_type="notification_event", task_id=task_id, payload={"message": "Task completed"}))
            await self.event_bus.publish(AgentEvent(event_type="state_event", task_id=task_id, payload={"state": TaskState.COMPLETED, "result": llm_output}))
        except Exception as exc:
            logger.error("Task failed task_id=%s error=%s", task_id, exc, exc_info=True)
            self.state_store.set_error(task_id, str(exc), cancelled=False)
            await self.event_bus.publish(AgentEvent(event_type="state_event", task_id=task_id, payload={"state": TaskState.FAILED, "error": str(exc)}))

    async def cancel_task(self, task_id: str) -> bool:
        task = self.state_store.get_task(task_id)
        if not task:
            return False

        execution_id = self.running_executions.get(task_id)
        if execution_id:
            try:
                await self.executor_client.cancel(execution_id)
            except Exception:
                pass

        self.state_store.set_error(task_id, "Cancelled by user", cancelled=True)
        await self.event_bus.publish(AgentEvent(event_type="state_event", task_id=task_id, payload={"state": TaskState.CANCELLED}))
        return True
