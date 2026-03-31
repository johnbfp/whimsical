# Architecture Notes

## Runtime Flow

1. Client creates task via `POST /agents/tasks`
2. Runtime transitions: `CREATED -> PLANNING -> RUNNING -> WAITING_TOOL`
3. Python orchestrator sends tool execution request to Rust executor
4. Rust executor enforces permission + idempotency + timeout + trace log
5. Python aggregates tool outputs, runs model provider chat, persists memory
6. Runtime transitions to `COMPLETED` (or `FAILED` / `CANCELLED`)
7. Events stream to web/android via websocket

## Separation of Concerns

- Python: business orchestration, planner, memory strategy, model gateway, APIs
- Rust: execution sandbox boundary, performance-sensitive concurrent tool runner

## Storage Strategy (MVP mock + production target)

MVP uses in-memory stores for fast bootstrapping while keeping interfaces production-ready.

Production mapping:
- Postgres: task metadata, plans, tool logs
- Redis: hot state cache, queue indexing
- Vector DB: semantic memory entries and recall ranking

## Plugin Contract

Plugin manifest includes:
- `plugin_id`
- `version`
- `tool_name`
- `permissions`
- `input_schema`
- `timeout_ms`

Only enabled plugins can bind tools into execution plans.


## 生产级 Agent 开发提示词方案（Python+Rust / Vue / Android）

### Summary
基于你已确认的方向，第一阶段先做“核心 Agent 内核”并确保可生产落地：  
- 模型策略：先本地后云端  
- 架构优先级：先内核能力，前端与 Android 作为可观测与联动入口  
- 存储：Postgres + Redis + 向量库（Milvus/Qdrant）  
- 联动协议：WebSocket 实时双向同步  
- Rust 角色：高性能执行、并发调度、沙箱与安全审计；Python 负责编排与模型链路

### Key Changes（按子系统拆分）
1. **Agent Core（Python）**
- 定义统一 Agent Runtime：`LLM + Planner + Tools + Memory + Executor + State` 六层解耦。
- 先实现单 Agent 会话，再扩展多 Agent 协同（Supervisor/Worker）能力。
- 规划器采用“任务分解 -> 工具选择 -> 执行计划 -> 结果反思”四阶段闭环。
- 状态机标准化：`CREATED -> PLANNING -> RUNNING -> WAITING_TOOL -> COMPLETED/FAILED/CANCELLED`。

2. **Executor & Sandbox（Rust）**
- Rust 提供独立执行服务：任务队列消费、工具调用隔离、并发控制、超时重试、审计日志。
- 工具执行必须具备：权限白名单、资源配额（CPU/内存/时长）、幂等键、可追踪 trace-id。
- Python 通过 gRPC/HTTP 调用 Rust Executor，保持语言边界清晰。

3. **Model Gateway（先本地后云端）**
- 抽象统一 Provider 接口：`chat() / embed() / rerank() / health_check()`。
- 第一版接入本地模型（Ollama/vLLM 任一），完成可替换适配层。
- 预留云端适配器（OpenAI/Anthropic），但先不作为发布阻塞项。

4. **Memory & State（生产存储）**
- Postgres：会话、任务、计划、工具调用记录、用户偏好与策略版本。
- Redis：运行态状态缓存、Executor 队列、短期上下文加速。
- 向量库：长期语义记忆（RAG），支持写入策略（摘要化、去重、过期）。
- 记忆读取策略明确优先级：会话短记忆 > 用户长期记忆 > 组织知识库。

5. **Plugin System（可插拔）**
- 定义插件清单（manifest）与生命周期：`install/enable/disable/uninstall`。
- 插件能力边界：工具声明、权限声明、输入输出 schema、超时策略。
- 插件执行走统一 Executor 通道，禁止绕过审计与权限系统。

6. **Vue Frontend + Android 联动**
- Vue 提供运行控制台：任务创建、计划可视化、工具调用轨迹、状态流转、错误回放。
- Android 提供移动端会话与任务监控，支持与 Web 端实时同步。
- 双端通过 WebSocket 订阅同一任务流：`task_event`, `tool_event`, `state_event`, `notification_event`。

### Public APIs / Interfaces（第一版需冻结）
- `POST /agents/tasks`：创建任务  
- `GET /agents/tasks/{id}`：查询任务详情与状态  
- `POST /agents/tasks/{id}/cancel`：取消任务  
- `WS /agents/stream?task_id=`：任务实时事件流  
- `POST /memory/recall`：按上下文召回记忆  
- `POST /plugins/install`、`POST /plugins/{id}/enable`：插件管理  
- `POST /models/switch`：本地模型路由切换  
- Rust Executor 内部接口：`ExecuteTool`, `GetExecutionStatus`, `CancelExecution`

### Test Plan（验收场景）
- 核心链路：任务从创建到完成全链路通过（含 Planner -> Tool -> Memory -> State 更新）。
- 稳定性：高并发任务压测、工具超时/失败重试、任务取消一致性。
- 安全性：越权工具调用拦截、插件权限冲突、沙箱逃逸测试。
- 模型路由：本地模型不可用时降级策略（明确失败原因与用户提示）。
- 双端联动：Web 创建任务，Android 实时接收状态；Android 操作取消，Web 同步更新。
- 可观测性：日志、指标、trace 能关联到单次任务全链路。

### 开发提示词文案（可直接喂给实现 Agent）
- 你是一个生产级 Agent 系统实现工程师。请按“先核心内核、后多端增强”的策略开发。  
- 技术栈固定：后端 Python + Rust，前端 Vue，移动端 Android。  
- 必须实现六层架构：LLM、Planner、Tools、Memory、Executor、State；每层独立接口、可替换、可测试。  
- 模型接入顺序：先本地模型（Ollama/vLLM），再云端模型；统一 Provider 接口，不允许业务层直连具体模型 SDK。  
- Rust 负责执行器、并发调度、沙箱和安全审计；Python 负责编排、策略、API 聚合。  
- 数据层固定：Postgres（持久化）+ Redis（状态与队列）+ 向量库（长期记忆）。  
- 插件必须使用 manifest + 权限声明 + schema 校验，并接入统一执行器与审计链路。  
- Web 与 Android 必须通过 WebSocket 实时同步任务状态与工具事件。  
- 输出要求：先交付可运行最小闭环（创建任务->规划->执行->回写记忆->完成），再逐步补齐插件管理、联动体验与运维能力。  
- 代码必须包含：接口契约、错误码规范、可观测埋点、单元测试与关键集成测试。

### Assumptions
- 第一版不做复杂多租户计费，仅预留租户字段与鉴权扩展点。
- 默认单区域部署，后续再扩展跨地域与多活。
- Android 第一版以“任务联动与通知”为主，不阻塞内核上线。
