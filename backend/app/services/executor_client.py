from __future__ import annotations

from typing import Any

import httpx


class ExecutorClient:
    def __init__(self, base_url: str) -> None:
        self.base_url = base_url.rstrip("/")

    async def execute_tool(
        self,
        task_id: str,
        tool_name: str,
        payload: dict[str, Any],
        timeout_ms: int = 4000,
        idempotency_key: str | None = None,
    ) -> dict[str, Any]:
        req = {
            "task_id": task_id,
            "tool_name": tool_name,
            "input": payload,
            "permissions": ["tool.execute"],
            "idempotency_key": idempotency_key or f"{task_id}:{tool_name}",
            "timeout_ms": timeout_ms,
            "trace_id": task_id,
        }
        async with httpx.AsyncClient(timeout=timeout_ms / 1000 + 2) as client:
            response = await client.post(f"{self.base_url}/execute", json=req)
            response.raise_for_status()
            data = response.json()
        return data

    async def cancel(self, execution_id: str) -> dict[str, Any]:
        async with httpx.AsyncClient(timeout=5) as client:
            response = await client.post(f"{self.base_url}/cancel/{execution_id}")
            response.raise_for_status()
            return response.json()
