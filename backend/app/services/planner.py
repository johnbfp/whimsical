from __future__ import annotations

import json
import logging
import re
from typing import TYPE_CHECKING

from app.models.schemas import Plan, ToolStep

if TYPE_CHECKING:
    from app.services.model_gateway import ModelGateway

logger = logging.getLogger(__name__)

PLAN_SYSTEM_PROMPT = """你是一个AI任务规划器。根据用户请求，生成一个JSON执行计划。

可用工具及输入格式：
- echo:           {{"text": "要输出的文本"}}                        ← 仅用于简单文本输出
- file_write:     {{"path": "output.txt",  "content": "内容"}}    ← 创建或写入文件
- file_read:      {{"path": "output.txt"}}                          ← 读取文件
- http_fetch:     {{"url": "https://example.com", "method": "GET"}} ← HTTP请求
- shell_exec:     {{"command": "dir"}}                              ← 执行 shell 命令（Windows CMD）
- json_transform: {{"data": {{}}, "expression": "data['key']"}}      ← JSON运算
- memory.write:   {{"key": "键名", "value": "内容"}}                   ← 写入记忆
- memory.read:    {{"key": "键名"}}                               ← 读取记忆
- memory.search:  {{"query": "关键词"}}                             ← 搜索记忆

工具选择规则（严格遵守）：
★ 用户要求「创建文件」或「写入文件」 → 必须使用 file_write，不需要 echo
★ 用户要求「读取文件或查看文件」 → 必须使用 file_read
★ 用户要求「列出文件」或「运行命令」 → 必须使用 shell_exec，命令用 Windows dir 实现
★ 仅当用户明确要求输出纯文本时才用 echo
★ file_write/file_read/shell_exec 均在沙盒目录下运行，只需提供相对路径（如 hello.txt）

示例：
用户: 「创建一个 hello.txt 内容为 hello」
正确规划: {{"execution_steps": [{{"tool_name": "file_write", "input": {{"path": "hello.txt", "content": "hello"}}}}]}}

返回格式：只返回合法JSON（不含 markdown 代码块）：
{{
  "task_breakdown": ["步骤描述", ...],
  "selected_tools": ["tool_name", ...],
  "execution_steps": [{{"tool_name": "...", "input": {{...}}}}, ...],
  "reflection_hint": "..."
}}

重要：最终回复用户时，始终使用中文（简体）。"""


class Planner:
    def __init__(self, model_gateway: ModelGateway | None = None) -> None:
        self._gateway = model_gateway

    async def build_plan(self, prompt: str, memory_context: str = "") -> Plan:
        """Attempt LLM-driven planning; fall back to deterministic stub."""
        if self._gateway and self._gateway.provider_name != "local":
            try:
                return await self._llm_plan(prompt, memory_context)
            except Exception as exc:
                logger.warning("LLM planning failed (%s), falling back to stub", exc)
        return self._stub_plan(prompt)

    async def _llm_plan(self, prompt: str, memory_context: str = "") -> Plan:
        context_block = (
            f"\n\n## 近期对话历史（供参考，了解用户偏好和上下文）：\n{memory_context}\n"
            if memory_context else ""
        )
        full_prompt = f"{PLAN_SYSTEM_PROMPT}{context_block}\n\n用户请求: {prompt}"
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
