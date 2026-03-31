import time

from fastapi.testclient import TestClient

from app.main import app, runtime, state_store, plugin_manager, model_gateway


client = TestClient(app)


def _patch_executor():
    async def fake_execute_tool(task_id: str, tool_name: str, payload: dict, timeout_ms: int = 4000):
        return {
            "execution_id": f"exec-{task_id}",
            "task_id": task_id,
            "tool_name": tool_name,
            "status": "COMPLETED",
            "result": payload.get("text", ""),
            "trace_id": task_id,
        }
    runtime.executor_client.execute_tool = fake_execute_tool  # type: ignore[method-assign]


def test_task_lifecycle() -> None:
    _patch_executor()

    create_resp = client.post(
        "/agents/tasks",
        json={"tenant_id": "t1", "user_id": "u1", "prompt": "say hello"},
    )
    assert create_resp.status_code == 200
    task_id = create_resp.json()["task_id"]

    for _ in range(20):
        task_resp = client.get(f"/agents/tasks/{task_id}")
        data = task_resp.json()
        if data["state"] in {"COMPLETED", "FAILED"}:
            break
        time.sleep(0.05)

    final_resp = client.get(f"/agents/tasks/{task_id}")
    assert final_resp.status_code == 200
    assert final_resp.json()["state"] == "COMPLETED"


def test_task_not_found() -> None:
    resp = client.get("/agents/tasks/nonexistent-id")
    assert resp.status_code == 404
    detail = resp.json()["detail"]
    assert detail["code"] == "TASK_NOT_FOUND"


def test_cancel_nonexistent_task() -> None:
    resp = client.post("/agents/tasks/nonexistent-id/cancel")
    assert resp.status_code == 404


def test_task_cancel() -> None:
    _patch_executor()

    create_resp = client.post(
        "/agents/tasks",
        json={"tenant_id": "t1", "user_id": "u1", "prompt": "long running"},
    )
    assert create_resp.status_code == 200
    task_id = create_resp.json()["task_id"]
    time.sleep(0.02)

    cancel_resp = client.post(f"/agents/tasks/{task_id}/cancel")
    assert cancel_resp.status_code == 200
    assert cancel_resp.json()["cancelled"] is True


def test_model_switch() -> None:
    resp = client.post("/models/switch", json={"provider": "local", "model_name": "ollama:llama3"})
    assert resp.status_code == 200
    assert resp.json()["provider"] == "local"


def test_model_switch_ollama() -> None:
    resp = client.post("/models/switch", json={"provider": "ollama", "model_name": "llama3.1"})
    assert resp.status_code == 200
    assert resp.json()["provider"] == "ollama"


def test_model_switch_unsupported() -> None:
    resp = client.post("/models/switch", json={"provider": "unknown", "model_name": "x"})
    assert resp.status_code == 400
    assert resp.json()["detail"]["code"] == "PROVIDER_UNSUPPORTED"


def test_memory_recall() -> None:
    runtime.memory_service.persist_summary("t1", "u1", "summary one")
    resp = client.post("/memory/recall", json={"tenant_id": "t1", "user_id": "u1", "query": "s", "top_k": 3})
    assert resp.status_code == 200
    assert len(resp.json()["items"]) >= 1


def test_plugin_lifecycle() -> None:
    # Install
    manifest = {
        "plugin_id": "test-plugin",
        "version": "1.0.0",
        "tool_name": "echo",
        "permissions": ["tool.execute"],
        "input_schema": {"type": "object", "properties": {"text": {"type": "string"}}, "required": ["text"]},
        "timeout_ms": 3000,
    }
    resp = client.post("/plugins/install", json={"manifest": manifest})
    assert resp.status_code == 200

    # List
    resp = client.get("/plugins")
    assert resp.status_code == 200
    ids = [p["plugin_id"] for p in resp.json()]
    assert "test-plugin" in ids

    # Enable
    resp = client.post("/plugins/test-plugin/enable")
    assert resp.status_code == 200
    assert resp.json()["enabled"] is True

    # Disable
    resp = client.post("/plugins/test-plugin/disable")
    assert resp.status_code == 200
    assert resp.json()["enabled"] is False

    # Uninstall
    resp = client.delete("/plugins/test-plugin")
    assert resp.status_code == 200


def test_plugin_not_found() -> None:
    resp = client.post("/plugins/nonexistent/enable")
    assert resp.status_code == 404
    assert resp.json()["detail"]["code"] == "PLUGIN_NOT_FOUND"


def test_plugin_schema_validation() -> None:
    from app.services.plugin_manager import PluginValidationError
    from app.models.schemas import PluginManifest

    m = PluginManifest(
        plugin_id="schema-test",
        version="1.0.0",
        tool_name="echo_v2",
        permissions=["tool.execute"],
        input_schema={"type": "object", "properties": {"text": {"type": "string"}}, "required": ["text"]},
        timeout_ms=3000,
    )
    plugin_manager.install(m)
    plugin_manager.enable("schema-test")

    # Valid input — no error
    plugin_manager.validate_input("echo_v2", {"text": "hello"})

    # Invalid input — should raise
    try:
        plugin_manager.validate_input("echo_v2", {"number": 123})
        assert False, "Should have raised"
    except PluginValidationError:
        pass

    plugin_manager.uninstall("schema-test")


def test_healthz() -> None:
    resp = client.get("/healthz")
    assert resp.status_code == 200
    assert resp.json()["status"] == "ok"
