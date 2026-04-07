# Android Agent Client

Mobile companion for the Whimsical Agent runtime — Jetpack Compose + Material 3.

## Architecture

```
data/
  models.kt          — Kotlin data classes matching backend schemas + JSON helpers
  ApiClient.kt       — Blocking HTTP client covering all REST endpoints
  TaskStreamClient.kt — WebSocket client for real-time task event streaming
  AgentRepository.kt — Repository layer (suspend + Dispatchers.IO)
ui/
  MainActivity.kt    — Entry point, DI wiring, bottom navigation shell
  MainViewModel.kt   — Task create / cancel / refresh + event stream
  MemoryViewModel.kt — Memory recall / write / search / history
  PluginViewModel.kt — List / enable / disable plugins
  ModelViewModel.kt  — Switch model provider & health check
  screens/
    AgentScreen.kt   — Task creation, event timeline, state badge
    MemoryScreen.kt  — 4-tab memory panel (Recall, Write, Search, History)
    PluginScreen.kt  — Plugin list with enable/disable toggle
    ModelScreen.kt   — Model gateway switch + server health indicator
```

## Backend API Coverage

| Endpoint                          | Status |
|-----------------------------------|--------|
| `POST /agents/tasks`              | ✅     |
| `GET  /agents/tasks/{id}`         | ✅     |
| `POST /agents/tasks/{id}/cancel`  | ✅     |
| `WS   /agents/stream?task_id=`    | ✅     |
| `POST /memory/recall`             | ✅     |
| `POST /memory/write`              | ✅     |
| `GET  /memory/read`               | ✅     |
| `GET  /memory/search`             | ✅     |
| `GET  /memory/history/{user_id}`  | ✅     |
| `GET  /plugins`                   | ✅     |
| `POST /plugins/{id}/enable`       | ✅     |
| `POST /plugins/{id}/disable`      | ✅     |
| `POST /models/switch`             | ✅     |
| `GET  /healthz`                   | ✅     |

## Build

Requires Android Studio Hedgehog+ with AGP 8.2+.

```bash
cd android
./gradlew assembleDebug
```

## Emulator Networking

The default base URL uses `10.0.2.2` which maps to the host machine's localhost from the Android emulator. For physical devices, update the IP in `MainActivity.kt`.

