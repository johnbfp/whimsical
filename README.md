# Whimsical Agent Runtime — 生产级 AI Agent 运行时

> Python (FastAPI) 编排 + Rust (Axum) 工具执行 + Vue 3 控制台 + Android 客户端

---

## 目录

- [项目简介](#项目简介)
- [架构总览](#架构总览)
- [目录结构](#目录结构)
- [环境要求](#环境要求)
- [快速启动（本地开发）](#快速启动本地开发)
- [Docker Compose 一键部署](#docker-compose-一键部署)
- [使用指南](#使用指南)
- [API 参考](#api-参考)
- [Rust 执行器工具清单](#rust-执行器工具清单)
- [模型提供者 & 切换](#模型提供者--切换)
- [插件系统](#插件系统)
- [WebSocket 事件流](#websocket-事件流)
- [任务状态机](#任务状态机)
- [运行测试](#运行测试)
- [配置项说明](#配置项说明)
- [常见问题 FAQ](#常见问题-faq)

---

## 项目简介

Whimsical Agent Runtime 是一个模块化 AI Agent 执行系统，具备六层解耦架构：

| 层 | 职责 | 技术 |
|----|------|------|
| **LLM（大脑）** | 自然语言理解与生成 | Ollama / OpenAI 兼容 API |
| **Planner（规划器）** | 四阶段闭环规划 | LLM 驱动 + Stub 兜底 |
| **Tools（工具）** | 具体动作执行 | echo, http_fetch, shell_exec, json_transform 等 |
| **Memory（记忆）** | 上下文召回 & 写回 | 三级记忆：session / user / org |
| **Executor（执行器）** | 沙箱隔离、并发控制 | Rust Axum 高性能服务 |
| **State（状态）** | 任务全生命周期管理 | 标准状态机 |

---

## 架构总览

```
┌──────────────┐     HTTP/WS      ┌──────────────────┐     HTTP      ┌─────────────┐
│  Vue 前端     │ ◄──────────────► │ Python Backend   │ ◄───────────► │ Rust 执行器  │
│  :5173       │                  │ (FastAPI) :8000   │               │ (Axum) :8088│
└──────────────┘                  └────────┬─────────┘               └─────────────┘
                                           │
                              ┌────────────┼────────────┐
                              ▼            ▼            ▼
                         ┌────────┐  ┌─────────┐  ┌─────────┐
                         │Ollama  │  │Postgres │  │ Qdrant  │
                         │:11434  │  │:5432    │  │ :6333   │
                         └────────┘  └─────────┘  └─────────┘
```

---

## 目录结构

```
whimsical_ideas/
├── backend/              # Python FastAPI 编排服务
│   ├── app/
│   │   ├── main.py           # 应用入口、所有 API 路由
│   │   ├── core/config.py    # 配置项（环境变量驱动）
│   │   ├── models/schemas.py # Pydantic 数据模型
│   │   └── services/
│   │       ├── agent_runtime.py   # Agent 核心运行时
│   │       ├── planner.py         # LLM 驱动的规划器
│   │       ├── model_gateway.py   # 模型网关（Ollama/Cloud/Mock）
│   │       ├── executor_client.py # Rust 执行器 HTTP 客户端
│   │       ├── plugin_manager.py  # 插件生命周期管理
│   │       ├── memory_service.py  # 三级记忆服务
│   │       ├── event_bus.py       # 内存事件总线（pub/sub）
│   │       └── state_store.py     # 任务状态存储
│   └── tests/test_api.py    # 12 个自动化测试
├── executor-rs/          # Rust 工具执行器
│   ├── Cargo.toml
│   └── src/main.rs           # 5 个工具 + 权限 + 幂等 + 超时
├── frontend/             # Vue 3 控制台
│   └── src/
│       ├── App.vue           # 主界面
│       └── components/       # 7 个子组件
├── android/              # Android Compose 客户端
├── mock_executor.py      # Python 模拟执行器（无需 Rust 时使用）
├── dev-start.bat         # Windows 一键启动脚本
├── docker-compose.yml    # 6 服务编排
└── docs/architecture.md  # 架构设计文档
```

---

## 环境要求

| 组件 | 最低版本 | 用途 |
|------|---------|------|
| Python | 3.11+ | 后端服务 |
| Node.js | 18+ | 前端构建 |
| Rust (cargo) | 1.75+ | 编译 Rust 执行器（可选，可用 mock_executor.py 替代）|
| Ollama | 最新版 | 本地 LLM（可选，无则自动降级为 Mock）|

---

## 快速启动（本地开发）

### 方式一：逐步启动

**① 启动 Rust 执行器**（二选一）

```powershell
# 如果安装了 Rust
cd executor-rs
cargo build --release
.\target\release\agent-executor.exe

# 如果没有 Rust，用 Python 模拟执行器
python mock_executor.py
```

**② 启动 Python 后端**

```powershell
cd backend
python -m venv .venv
.\.venv\Scripts\activate
pip install -e ".[test]"
uvicorn app.main:app --host 0.0.0.0 --port 8000 --reload
```

**③ 启动 Vue 前端**

```powershell
cd frontend
npm install
npm run dev
```

**④ 打开浏览器** → http://localhost:5173

### 方式二：一键启动（Windows）

```powershell
dev-start.bat
```

此脚本会在三个独立窗口中分别启动执行器、后端和前端。

---

## Docker Compose 一键部署

```bash
docker-compose up --build
```

启动 6 个服务：Postgres、Redis、Qdrant、Rust 执行器、Python 后端、Vue 前端。

---

## 使用指南

### 1. 创建任务

在前端页面顶部的文本框中输入任务提示词（如"帮我查一下天气"），点击 **Create Task** 按钮。

系统会自动：
1. 创建任务 → 状态变为 `CREATED`
2. 调用 LLM 生成计划 → `PLANNING`
3. 按计划逐步执行工具 → `RUNNING` → `WAITING_TOOL` → `RUNNING`
4. 汇总结果 → `COMPLETED`

### 2. 实时事件流

任务创建后会自动建立 WebSocket 连接，页面上的 **Event Log** 区域实时显示：
- 🔵 `task_event` — 任务创建 / 计划生成
- 🟢 `state_event` — 状态变更
- 🟠 `tool_event` — 工具执行结果
- 🟣 `notification_event` — 完成通知

### 3. 查看计划

**Plan View** 区域显示 LLM 生成的四阶段计划：
- **任务分解**：将用户请求拆分为子步骤
- **工具选择**：选取要使用的工具
- **执行步骤**：每步的工具名与输入参数
- **反思提示**：质量校验建议

### 4. 查看工具执行轨迹

**Tool Trace** 区域显示每个工具的执行详情：
- 工具名称、执行 ID
- 状态（COMPLETED / FAILED）
- 返回结果或错误信息

### 5. 切换模型

展开底部 **Model / Plugin / Memory** 面板，在 Model Switch 区域：

| Provider | 说明 | 前置条件 |
|----------|------|---------|
| `local` | 本地 Mock 模拟 | 无需任何配置 |
| `ollama` | Ollama 本地 LLM | 需运行 `ollama serve`，至少拉取一个模型 |
| `cloud` | OpenAI 兼容 API | 需设置环境变量 `CLOUD_API_KEY` |

### 6. 管理插件

在 **Plugin Panel** 中可以：
- 安装新插件（指定 ID、版本、工具名、超时时间）
- 启用 / 禁用已安装插件
- 卸载插件

### 7. 记忆召回

在 **Memory Recall** 中输入用户 ID 和查询数量 (top_k)，点击 Recall 查看历史记忆。

### 8. 取消任务

在页面的 State 行点击 **Cancel** 按钮可取消正在运行的任务。

### 9. 手动刷新

点击 **Refresh** 按钮手动轮询任务最新状态（WebSocket 断开时有用）。

---

## API 参考

### 任务管理

| 方法 | 路径 | 说明 |
|------|------|------|
| `POST` | `/agents/tasks` | 创建任务并自动开始执行 |
| `GET` | `/agents/tasks/{task_id}` | 查询任务状态、计划、结果 |
| `POST` | `/agents/tasks/{task_id}/cancel` | 取消任务 |
| `WS` | `/agents/stream?task_id={id}` | 实时事件推流 |

#### 创建任务

```bash
curl -X POST http://localhost:8000/agents/tasks \
  -H "Content-Type: application/json" \
  -d '{"user_id": "demo-user", "prompt": "说一句话"}'
```

**请求体**：

```json
{
  "tenant_id": "default",   // 可选，默认 "default"
  "user_id": "demo-user",   // 必填
  "prompt": "你的任务描述"    // 必填
}
```

**响应**：

```json
{
  "task_id": "uuid",
  "tenant_id": "default",
  "user_id": "demo-user",
  "prompt": "你的任务描述",
  "state": "CREATED",
  "plan": null,
  "result": null,
  "error": null,
  "model_provider": "ollama",
  "model_name": "llama3:8b",
  "created_at": "2026-03-31T09:00:00Z",
  "updated_at": "2026-03-31T09:00:00Z"
}
```

#### 查询任务

```bash
curl http://localhost:8000/agents/tasks/{task_id}
```

#### 取消任务

```bash
curl -X POST http://localhost:8000/agents/tasks/{task_id}/cancel
```

### 模型切换

```bash
curl -X POST http://localhost:8000/models/switch \
  -H "Content-Type: application/json" \
  -d '{"provider": "ollama", "model_name": "llama3:8b"}'
```

支持的 provider：`local`、`ollama`、`cloud`

### 记忆召回

```bash
curl -X POST http://localhost:8000/memory/recall \
  -H "Content-Type: application/json" \
  -d '{"tenant_id": "default", "user_id": "demo-user", "top_k": 5}'
```

### 插件管理

```bash
# 安装插件
curl -X POST http://localhost:8000/plugins/install \
  -H "Content-Type: application/json" \
  -d '{
    "manifest": {
      "plugin_id": "weather",
      "version": "1.0.0",
      "tool_name": "http_fetch",
      "permissions": ["tool.execute"],
      "input_schema": {"type": "object", "properties": {"url": {"type": "string"}}},
      "timeout_ms": 5000
    }
  }'

# 列出所有插件
curl http://localhost:8000/plugins

# 启用
curl -X POST http://localhost:8000/plugins/weather/enable

# 禁用
curl -X POST http://localhost:8000/plugins/weather/disable

# 卸载
curl -X DELETE http://localhost:8000/plugins/weather
```

### 健康检查

```bash
curl http://localhost:8000/healthz
# 响应：{"status": "ok"}
```

---

## Rust 执行器工具清单

Rust 执行器在端口 8088 运行，提供沙箱化的工具执行能力。

| 工具名 | 功能 | 输入格式 | 安全特性 |
|--------|------|---------|---------|
| **echo** | 文本回显 | `{"text": "hello"}` | — |
| **memory.write** | 记忆持久化占位 | 任意 | — |
| **http_fetch** | HTTP 请求 | `{"url": "...", "method": "GET", "headers": {}, "body": {}}` | 响应截断 64KB，限制 5 次重定向 |
| **shell_exec** | 沙箱命令执行 | `{"command": "echo hello"}` | **白名单**：echo, date, whoami, uname, cat, ls, dir, pwd, env |
| **json_transform** | JSON 路径提取 | `{"data": {...}, "path": "a.b.c"}` | 支持点路径、数组下标 |

### 执行器 API

| 方法 | 路径 | 说明 |
|------|------|------|
| `POST` | `/execute` | 执行工具 |
| `GET` | `/status/{execution_id}` | 查询执行状态 |
| `POST` | `/cancel/{execution_id}` | 取消执行 |
| `GET` | `/healthz` | 健康检查 |

### 安全机制

- **权限白名单**：请求必须携带 `"permissions": ["tool.execute"]`
- **工具白名单**：只允许已注册的工具名
- **幂等键**：相同 `idempotency_key` 不会重复执行
- **超时控制**：每个工具有独立超时时间
- **trace-id**：全链路可追踪

---

## 模型提供者 & 切换

| Provider | 实现类 | 说明 | 能力 |
|----------|--------|------|------|
| `local` | `LocalMockProvider` | 模拟返回，无需 GPU | chat ✓ embed ✓ |
| `ollama` | `OllamaProvider` | 连接本地 Ollama（如 llama3:8b）| chat ✓ embed ✓ |
| `cloud` | `CloudProvider` | OpenAI / DeepSeek 兼容 API | chat ✓ embed ✓ |

**启动自动探测**：启动时先探测 Ollama 是否可用（`GET /api/tags`），可用则使用 Ollama，否则降级为 LocalMock。

**运行时切换**：调用 `POST /models/switch` 即时生效，后续所有 LLM 调用使用新 provider。

---

## 插件系统

### 生命周期

```
安装 (install) → 启用 (enable) ⇄ 禁用 (disable) → 卸载 (uninstall)
```

### 插件清单格式 (PluginManifest)

```json
{
  "plugin_id": "my-plugin",
  "version": "1.0.0",
  "tool_name": "http_fetch",
  "permissions": ["tool.execute"],
  "input_schema": {
    "type": "object",
    "properties": {
      "url": {"type": "string"}
    },
    "required": ["url"]
  },
  "timeout_ms": 5000
}
```

### 输入验证

安装插件时如果提供了 `input_schema`，每次工具执行前都会用 JSON Schema 校验输入，不符合则拒绝执行。

---

## WebSocket 事件流

### 连接方式

```javascript
const ws = new WebSocket("ws://localhost:8000/agents/stream?task_id=YOUR_TASK_ID")
ws.onmessage = (e) => {
  const event = JSON.parse(e.data)
  console.log(event.event_type, event.payload)
}
```

### 事件类型

| event_type | 触发时机 | payload 示例 |
|------------|---------|-------------|
| `task_event` | 任务创建 / 计划生成 | `{state, plan}` |
| `state_event` | 状态变更 | `{state, tool?, result?, error?}` |
| `tool_event` | 工具执行完成 | `{execution_id, status, result}` |
| `notification_event` | 任务完成通知 | `{message: "Task completed"}` |

### 典型事件流

```
连接 WebSocket (task_id=abc)
← state_event:  {state: "PLANNING"}
← task_event:   {plan: {task_breakdown: [...], ...}}
← state_event:  {state: "RUNNING"}
← state_event:  {state: "WAITING_TOOL", tool: "echo"}
← tool_event:   {status: "COMPLETED", result: "Echo: hello"}
← state_event:  {state: "RUNNING"}
← state_event:  {state: "COMPLETED", result: "..."}
← notification: {message: "Task completed"}
```

---

## 任务状态机

```
  CREATED
     │
     ▼
  PLANNING ──────────────────┐
     │                       │
     ▼                       ▼
  RUNNING ◄──┐            FAILED
     │       │               ▲
     ▼       │               │
WAITING_TOOL─┘           (异常时)
     │
     ▼
  COMPLETED

  任何状态 ──► CANCELLED（用户取消）
```

---

## 运行测试

```powershell
cd backend
pip install -e ".[test]"
python -m pytest tests/ -v
```

### 测试列表（12 个）

| 测试名 | 覆盖场景 |
|--------|---------|
| `test_task_lifecycle` | 创建任务 → 轮询直至完成 |
| `test_task_not_found` | 查询不存在的任务返回 404 |
| `test_task_cancel` | 取消正在运行的任务 |
| `test_cancel_nonexistent_task` | 取消不存在的任务返回 404 |
| `test_model_switch` | 切换到 local 提供者 |
| `test_model_switch_ollama` | 切换到 ollama 提供者 |
| `test_model_switch_unsupported` | 未知 provider 返回 400 |
| `test_memory_recall` | 记忆召回 |
| `test_plugin_lifecycle` | 插件安装→启用→禁用→卸载 |
| `test_plugin_not_found` | 操作不存在的插件返回 404 |
| `test_plugin_schema_validation` | 插件 Schema 校验 |
| `test_healthz` | 健康检查 |

---

## 配置项说明

所有配置通过环境变量覆盖（基于 pydantic-settings）：

| 环境变量 | 默认值 | 说明 |
|----------|--------|------|
| `APP_NAME` | Whimsical Agent Runtime | 应用名 |
| `EXECUTOR_URL` | http://127.0.0.1:8088 | Rust 执行器地址 |
| `DEFAULT_LOCAL_MODEL` | llama3:8b | 默认模型名 |
| `OLLAMA_BASE_URL` | http://127.0.0.1:11434 | Ollama 地址 |
| `CLOUD_API_KEY` | （空） | Cloud 提供者 API Key |
| `CLOUD_BASE_URL` | https://api.openai.com/v1 | Cloud API 地址 |
| `POSTGRES_DSN` | postgresql+asyncpg://agent:agent@127.0.0.1:5432/agentdb | Postgres 连接 |
| `REDIS_URL` | redis://127.0.0.1:6379/0 | Redis 连接 |
| `QDRANT_URL` | http://127.0.0.1:6333 | Qdrant 向量库地址 |
| `QDRANT_COLLECTION` | agent_memory | Qdrant 集合名 |
| `LOG_LEVEL` | INFO | 日志级别 |

---

## 常见问题 FAQ

### Q: 没有安装 Ollama / GPU，能运行吗？
**可以。** 系统启动时自动探测，如果 Ollama 不可用会降级为 `LocalMockProvider`，返回模拟数据。所有功能正常运行，只是 LLM 输出为模拟文本。

### Q: 没有安装 Rust，能运行吗？
**可以。** 使用项目根目录的 `mock_executor.py` 作为替代：
```powershell
python mock_executor.py   # 监听 8088 端口，模拟所有工具
```

### Q: 如何连接真实的 OpenAI / DeepSeek？
设置环境变量：
```powershell
$env:CLOUD_API_KEY = "sk-your-api-key"
$env:CLOUD_BASE_URL = "https://api.openai.com/v1"  # 或 DeepSeek 地址
```
然后通过 API 或前端切换到 `cloud` 提供者。

### Q: Postgres / Redis / Qdrant 必须装吗？
**当前 MVP 不需要。** 所有存储使用内存实现。Docker Compose 包含这些服务是为生产部署做准备。

### Q: 前端报错 "WebSocket connection failed"？
确认后端正在运行（`curl http://localhost:8000/healthz`），并检查 Vite 代理配置是否正确。

### Q: 如何新增一个工具？
1. 在 `executor-rs/src/main.rs` 的 `is_tool_allowed()` 中添加工具名
2. 在 `run_tool()` 的 match 分支中实现工具逻辑
3. 在 `backend/app/services/planner.py` 的工具列表中注册
4. 重新编译 Rust 执行器（或同步更新 `mock_executor.py`）
