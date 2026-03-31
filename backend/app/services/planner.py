from __future__ import annotations

import json
import logging
import re
from typing import TYPE_CHECKING

from app.models.schemas import Plan, ToolStep

if TYPE_CHECKING:
    from app.services.model_gateway import ModelGateway

logger = logging.getLogger(__name__)

PLAN_SYSTEM_PROMPT = """You are an AI task planner. Given a user request, produce a JSON execution plan.
Follow the four-stage planning loop:
1. Task Decomposition — break the request into atomic sub-tasks.
2. Tool Selection — choose the best tool for each sub-task from the available tool list.
3. Execution Planning — order the steps logically.
4. Reflection — add a hint for post-execution quality validation.

Available tools: echo, memory.write, http_fetch, shell_exec, json_transform

Return ONLY valid JSON (no markdown fences) with this exact schema:
{
  "task_breakdown": ["step description", ...],
  "selected_tools": ["tool_name", ...],
  "execution_steps": [{"tool_name": "...", "input": {...}}, ...],
  "reflection_hint": "..."
}"""


class Planner:
    def __init__(self, model_gateway: ModelGateway | None = None) -> None:
        self._gateway = model_gateway

    async def build_plan(self, prompt: str) -> Plan:
        """Attempt LLM-driven planning; fall back to deterministic stub."""
        if self._gateway and self._gateway.provider_name != "local":
            try:
                return await self._llm_plan(prompt)
            except Exception as exc:
                logger.warning("LLM planning failed (%s), falling back to stub", exc)
        return self._stub_plan(prompt)

    async def _llm_plan(self, prompt: str) -> Plan:
        full_prompt = f"{PLAN_SYSTEM_PROMPT}\n\nUser request: {prompt}"
        raw = await self._gateway.provider.chat(full_prompt)  # type: ignore[union-attr]
        # Strip markdown code fences if present
        cleaned = re.sub(r"^```(?:json)?\s*", "", raw.strip())
        cleaned = re.sub(r"\s*```$", "", cleaned)
        data = json.loads(cleaned)
        steps = [ToolStep(tool_name=s["tool_name"], input=s.get("input", {})) for s in data["execution_steps"]]
        return Plan(
            task_breakdown=data["task_breakdown"],
            selected_tools=data["selected_tools"],
            execution_steps=steps,
            reflection_hint=data.get("reflection_hint", ""),
        )

    @staticmethod
    def _stub_plan(prompt: str) -> Plan:
        return Plan(
            task_breakdown=[
                f"Understand request: {prompt[:120]}",
                "Select appropriate tool",
                "Execute tool with input",
                "Validate result quality",
            ],
            selected_tools=["echo"],
            execution_steps=[ToolStep(tool_name="echo", input={"text": prompt})],
            reflection_hint="Validate result quality and persist summary into memory.",
        )
