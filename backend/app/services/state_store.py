from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime, timezone

from app.models.schemas import Plan, TaskResponse, TaskState


@dataclass
class StateStore:
    tasks: dict[str, TaskResponse] = field(default_factory=dict)

    def create_task(self, task: TaskResponse) -> TaskResponse:
        self.tasks[task.task_id] = task
        return task

    def get_task(self, task_id: str) -> TaskResponse | None:
        return self.tasks.get(task_id)

    def update_task_state(self, task_id: str, state: TaskState) -> TaskResponse:
        task = self.tasks[task_id]
        task.state = state
        task.updated_at = datetime.now(timezone.utc)
        return task

    def set_plan(self, task_id: str, plan: Plan) -> TaskResponse:
        task = self.tasks[task_id]
        task.plan = plan
        task.updated_at = datetime.now(timezone.utc)
        return task

    def set_result(self, task_id: str, result: str) -> TaskResponse:
        task = self.tasks[task_id]
        task.result = result
        task.state = TaskState.COMPLETED
        task.updated_at = datetime.now(timezone.utc)
        return task

    def set_error(self, task_id: str, error: str, cancelled: bool = False) -> TaskResponse:
        task = self.tasks[task_id]
        task.error = error
        task.state = TaskState.CANCELLED if cancelled else TaskState.FAILED
        task.updated_at = datetime.now(timezone.utc)
        return task
