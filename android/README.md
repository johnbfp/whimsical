# Android Link Layer (MVP)

This folder provides a minimal Android-side skeleton for websocket-based task stream sync.

## Included
- `TaskStreamClient.kt`: websocket client for `/agents/stream?task_id=...`
- `MainViewModel.kt`: create task + bind stream + cancel task

## Notes
- This is intentionally lightweight and framework-agnostic.
- You can plug this into Jetpack Compose or XML UI.
