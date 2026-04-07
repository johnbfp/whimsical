from __future__ import annotations

import json
import logging
import re
from typing import TYPE_CHECKING

from app.models.schemas import Plan, ToolStep

if TYPE_CHECKING:
    from app.services.model_gateway import ModelGateway

logger = logging.getLogger(__name__)

PLAN_SYSTEM_PROMPT = """你是一个专业级AI编程助手（类似 Claude Code）。根据用户请求，生成一个JSON执行计划。

可用工具及输入格式：
# 文件操作
- file_write:     {{"path": "output.txt",  "content": "内容"}}    ← 创建或覆写文件
- file_read:      {{"path": "output.txt"}}                          ← 读取文件内容
- edit_file:      {{"path": "file.py", "old_string": "旧代码", "new_string": "新代码"}} ← 精确替换文件中的代码片段
- list_dir:       {{"path": "."}}                                    ← 列出目录结构
- grep_search:    {{"query": "搜索词", "path": "."}}              ← 全文搜索
- create_dir:     {{"path": "src/utils"}}                            ← 创建目录
- delete_file:    {{"path": "old_file.py"}}                          ← 删除文件或目录
- rename_file:    {{"old_path": "a.py", "new_path": "b.py"}}     ← 重命名或移动文件

# 执行 & 测试
- shell_exec:     {{"command": "python main.py"}}                    ← 执行任何 shell 命令
- run_tests:      {{"command": "python -m pytest -v"}}              ← 运行测试，返回结果和通过状态

# 网络 & 数据
- http_fetch:     {{"url": "https://example.com", "method": "GET"}} ← HTTP 请求
- json_transform: {{"data": {{}}, "expression": "data['key']"}}      ← JSON 运算
- echo:           {{"text": "输出文本"}}                              ← 纯文本输出（少用）

# 记忆
- memory.write:   {{"key": "键名", "value": "内容"}}
- memory.read:    {{"key": "键名"}}
- memory.search:  {{"query": "关键词"}}

## 编程工作流（最佳实践）

★ 编程任务必须按以下流程执行：
  1. 理解需求 → 分析用户要什么
  2. 探索代码 → list_dir 查看文件结构，file_read 阅读关键文件
  3. 搜索上下文 → grep_search 找到相关代码、函数定义
  4. 实施修改 → edit_file 精确替换（首选）或 file_write 完整重写
  5. 验证结果 → run_tests 运行测试 或 shell_exec 运行程序

★ 编辑文件时注意：
  - edit_file 的 old_string 必须完全匹配文件中的内容（含空格缩进）
  - old_string 应包含足够上下文（3-5行）以确保唯一匹配
  - 创建新文件用 file_write，修改已有文件用 edit_file

★ 工具选择规则：
  - 「创建/写入文件」 → file_write
  - 「编辑/修改代码」 → edit_file
  - 「读取文件」 → file_read
  - 「浏览目录」 → list_dir
  - 「搜索代码」 → grep_search
  - 「运行命令」 → shell_exec
  - 「运行测试」 → run_tests
  - 「联网查询」 → http_fetch
  - 「创建目录」 → create_dir
  - 「删除文件」 → delete_file
  - 「重命名」   → rename_file

## 示例

示例1 — 简单文件创建：
用户: 「创建一个hello.txt」
{{"task_breakdown": ["创建文件"], "selected_tools": ["file_write"], "execution_steps": [{{"tool_name": "file_write", "input": {{"path": "hello.txt", "content": "hello world"}}}}], "reflection_hint": "检查文件是否创建成功"}}

示例2 — 编程任务（完整流程）：
用户: 「写一个Python HTTP服务器并测试」
{{"task_breakdown": ["查看当前文件结构", "创建服务器代码", "创建测试文件", "运行测试"],
  "selected_tools": ["list_dir", "file_write", "file_write", "run_tests"],
  "execution_steps": [
    {{"tool_name": "list_dir", "input": {{"path": "."}}}},
    {{"tool_name": "file_write", "input": {{"path": "server.py", "content": "from http.server import HTTPServer, BaseHTTPRequestHandler\\n\\nclass Handler(BaseHTTPRequestHandler):\\n    def do_GET(self):\\n        self.send_response(200)\\n        self.send_header('Content-Type', 'text/plain')\\n        self.end_headers()\\n        self.wfile.write(b'Hello World')\\n"}}}},
    {{"tool_name": "file_write", "input": {{"path": "test_server.py", "content": "import unittest\\nfrom server import Handler\\n\\nclass TestHandler(unittest.TestCase):\\n    def test_class_exists(self):\\n        self.assertTrue(hasattr(Handler, 'do_GET'))\\n\\nif __name__ == '__main__': unittest.main()"}}}},
    {{"tool_name": "run_tests", "input": {{"command": "python -m pytest test_server.py -v"}}}}
  ],
  "reflection_hint": "确认测试通过；检查服务器代码是否规范"}}

示例3 — 修改已有文件：
用户: 「把 calc.py 里的 add 函数改为支持任意数量参数」
{{"task_breakdown": ["读取文件", "修改函数", "测试"],
  "selected_tools": ["file_read", "edit_file", "run_tests"],
  "execution_steps": [
    {{"tool_name": "file_read", "input": {{"path": "calc.py"}}}},
    {{"tool_name": "edit_file", "input": {{"path": "calc.py", "old_string": "def add(a, b):\\n    return a + b", "new_string": "def add(*args):\\n    return sum(args)"}}}},
    {{"tool_name": "run_tests", "input": {{"command": "python -m pytest -v"}}}}
  ],
  "reflection_hint": "确认所有测试仍然通过"}}

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
