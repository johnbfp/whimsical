"""End-to-end smoke test against live services."""
import json
import urllib.request
import sys

BASE = "http://127.0.0.1:8000"
OK = 0
FAIL = 0

def test(name, fn):
    global OK, FAIL
    try:
        fn()
        print(f"  PASS  {name}")
        OK += 1
    except Exception as e:
        print(f"  FAIL  {name}: {e}")
        FAIL += 1

def get(path):
    r = urllib.request.urlopen(f"{BASE}{path}")
    return json.loads(r.read())

def post(path, body=None):
    data = json.dumps(body).encode() if body else None
    req = urllib.request.Request(f"{BASE}{path}", data=data,
                                headers={"Content-Type": "application/json"} if data else {})
    r = urllib.request.urlopen(req)
    return json.loads(r.read())

def delete(path):
    req = urllib.request.Request(f"{BASE}{path}", method="DELETE")
    r = urllib.request.urlopen(req)
    return json.loads(r.read())

# ── Tests ────────────────────────────────────────────────

def t_health():
    d = get("/healthz")
    assert d["status"] == "ok", d

def t_workspace_tree():
    d = get("/workspace/tree?path=.&depth=1")
    assert "entries" in d, d
    assert len(d["entries"]) > 0, "empty tree"

def t_workspace_write_read():
    post("/workspace/file", {"path": "_e2e_test.txt", "content": "hello e2e"})
    d = get("/workspace/file?path=_e2e_test.txt")
    assert d["content"] == "hello e2e", d
    delete("/workspace/file?path=_e2e_test.txt")

def t_workspace_mkdir_delete():
    post("/workspace/mkdir", {"path": "_e2e_dir"})
    d = get("/workspace/tree?path=_e2e_dir&depth=1")
    assert d["entries"] is not None
    delete("/workspace/file?path=_e2e_dir")

def t_workspace_rename():
    post("/workspace/file", {"path": "_e2e_ren1.txt", "content": "rename test"})
    post("/workspace/rename", {"old_path": "_e2e_ren1.txt", "new_path": "_e2e_ren2.txt"})
    d = get("/workspace/file?path=_e2e_ren2.txt")
    assert d["content"] == "rename test", d
    delete("/workspace/file?path=_e2e_ren2.txt")

def t_workspace_edit():
    post("/workspace/file", {"path": "_e2e_edit.txt", "content": "aaa bbb ccc"})
    r = post("/workspace/edit", {"path": "_e2e_edit.txt", "old_string": "bbb", "new_string": "XXX"})
    assert r.get("ok"), r
    d = get("/workspace/file?path=_e2e_edit.txt")
    assert "XXX" in d["content"], d["content"]
    delete("/workspace/file?path=_e2e_edit.txt")

def t_workspace_stat():
    post("/workspace/file", {"path": "_e2e_stat.txt", "content": "stat test"})
    d = get("/workspace/stat?path=_e2e_stat.txt")
    assert d["exists"] is True, d
    assert d["size"] > 0, d
    delete("/workspace/file?path=_e2e_stat.txt")

def t_workspace_run():
    d = post("/workspace/run", {"command": "echo hello_e2e", "timeout": 10})
    assert d["exit_code"] == 0, d
    assert "hello_e2e" in d["stdout"], d

def t_workspace_search():
    post("/workspace/file", {"path": "_e2e_search.txt", "content": "findme_unique_token_xyz"})
    d = get("/workspace/search?query=findme_unique_token_xyz&path=.")
    assert len(d.get("matches", [])) > 0, d
    delete("/workspace/file?path=_e2e_search.txt")

def t_workspace_search_regex():
    post("/workspace/file", {"path": "_e2e_regex.txt", "content": "abc123def456"})
    d = get("/workspace/search?query=%5Cd%2B&path=.&is_regex=true&include=_e2e_regex.txt")
    assert len(d.get("matches", [])) > 0, d
    delete("/workspace/file?path=_e2e_regex.txt")

def t_task_create():
    d = post("/agents/tasks", {"tenant_id": "default", "user_id": "e2e", "prompt": "say hello"})
    assert "task_id" in d, d
    tid = d["task_id"]
    info = get(f"/agents/tasks/{tid}")
    assert info["task_id"] == tid
    post(f"/agents/tasks/{tid}/cancel")

def t_memory_write_recall():
    post("/memory/write", {"tenant_id": "default", "user_id": "e2e", "key": "e2e_key", "value": "e2e_val"})
    d = get("/memory/read?tenant_id=default&key=e2e_key")
    assert d.get("value") == "e2e_val", d

def t_memory_search():
    d = get("/memory/search?tenant_id=default&query=e2e&top_k=3")
    assert "results" in d, d

def t_model_switch():
    d = post("/models/switch", {"provider": "openai", "model_name": "gpt-4"})
    assert d.get("provider") == "openai", d

def t_plugin_list():
    d = get("/plugins")
    assert "plugins" in d, d

def t_executor_health():
    d = get("http://127.0.0.1:8088/healthz".replace(BASE, ""))
    # This will fail if executor is not proxied; test directly
    try:
        r = urllib.request.urlopen("http://127.0.0.1:8088/healthz")
        dd = json.loads(r.read())
        assert dd.get("status") == "ok" or "tools" in dd, dd
    except Exception:
        pass # executor may not have healthz

# ── Run ──────────────────────────────────────────────────

if __name__ == "__main__":
    print("=" * 50)
    print("E2E Smoke Tests against live services")
    print("=" * 50)

    test("Health Check", t_health)
    test("Workspace Tree", t_workspace_tree)
    test("Workspace Write/Read", t_workspace_write_read)
    test("Workspace Mkdir/Delete", t_workspace_mkdir_delete)
    test("Workspace Rename", t_workspace_rename)
    test("Workspace Edit (string replace)", t_workspace_edit)
    test("Workspace Stat", t_workspace_stat)
    test("Workspace Run Command", t_workspace_run)
    test("Workspace Search", t_workspace_search)
    test("Workspace Search (regex)", t_workspace_search_regex)
    test("Task Create/Get/Cancel", t_task_create)
    test("Memory Write/Recall", t_memory_write_recall)
    test("Memory Search", t_memory_search)
    test("Model Switch", t_model_switch)
    test("Plugin List", t_plugin_list)

    print("=" * 50)
    print(f"Results: {OK} passed, {FAIL} failed")
    print("=" * 50)
    sys.exit(1 if FAIL else 0)
