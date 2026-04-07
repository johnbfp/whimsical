"""Quick smoke test - workspace APIs only (no task execution)."""
import json, urllib.request, sys

BASE = "http://127.0.0.1:8000"
OK = FAIL = 0

def test(name, fn):
    global OK, FAIL
    try:
        fn()
        print(f"  PASS  {name}", flush=True)
        OK += 1
    except Exception as e:
        print(f"  FAIL  {name}: {e}", flush=True)
        FAIL += 1

def get(path):
    return json.loads(urllib.request.urlopen(f"{BASE}{path}", timeout=5).read())

def post(path, body):
    req = urllib.request.Request(f"{BASE}{path}",
        data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json"})
    return json.loads(urllib.request.urlopen(req, timeout=5).read())

def delete(path):
    req = urllib.request.Request(f"{BASE}{path}", method="DELETE")
    return json.loads(urllib.request.urlopen(req, timeout=5).read())

print("=" * 50, flush=True)
print("Quick Workspace API Smoke Tests", flush=True)
print("=" * 50, flush=True)

test("Health", lambda: (lambda d: None)(get("/healthz")))
test("Tree", lambda: (lambda d: None)(get("/workspace/tree?path=.&depth=1")))

def t_write_read():
    post("/workspace/file", {"path": "_smoketest.txt", "content": "smoke"})
    d = get("/workspace/file?path=_smoketest.txt")
    assert d["content"] == "smoke"
    delete("/workspace/file?path=_smoketest.txt")
test("Write/Read/Delete", t_write_read)

def t_mkdir():
    post("/workspace/mkdir", {"path": "_smokedir"})
    delete("/workspace/file?path=_smokedir")
test("Mkdir/Rmdir", t_mkdir)

def t_rename():
    post("/workspace/file", {"path": "_s1.txt", "content": "ren"})
    post("/workspace/rename", {"old_path": "_s1.txt", "new_path": "_s2.txt"})
    d = get("/workspace/file?path=_s2.txt")
    assert d["content"] == "ren"
    delete("/workspace/file?path=_s2.txt")
test("Rename", t_rename)

def t_edit():
    post("/workspace/file", {"path": "_sedit.txt", "content": "aaa bbb ccc"})
    post("/workspace/edit", {"path": "_sedit.txt", "old_string": "bbb", "new_string": "XXX"})
    d = get("/workspace/file?path=_sedit.txt")
    assert "XXX" in d["content"]
    delete("/workspace/file?path=_sedit.txt")
test("Edit (string replace)", t_edit)

def t_stat():
    post("/workspace/file", {"path": "_sstat.txt", "content": "stat"})
    d = get("/workspace/stat?path=_sstat.txt")
    assert d["exists"] is True
    delete("/workspace/file?path=_sstat.txt")
test("Stat", t_stat)

def t_run():
    d = post("/workspace/run", {"command": "echo smoketest_ok", "timeout": 5})
    assert d["exit_code"] == 0
    assert "smoketest_ok" in d["stdout"]
test("Run Command", t_run)

def t_search():
    post("/workspace/file", {"path": "_ssearch.txt", "content": "unique_smoke_token_42"})
    d = get("/workspace/search?query=unique_smoke_token_42&path=.")
    assert len(d.get("matches", [])) > 0
    delete("/workspace/file?path=_ssearch.txt")
test("Search", t_search)

test("Memory Write", lambda: post("/memory/write", {"tenant_id": "default", "user_id": "smoke", "key": "skey", "value": "sval"}))
test("Memory Read", lambda: get("/memory/read?tenant_id=default&key=skey"))
test("Memory Search", lambda: get("/memory/search?tenant_id=default&query=smoke&top_k=3"))
test("Model Switch", lambda: post("/models/switch", {"provider": "openai", "model_name": "gpt-4"}))
test("Plugin List", lambda: get("/plugins"))

print("=" * 50, flush=True)
print(f"Results: {OK} passed, {FAIL} failed", flush=True)
print("=" * 50, flush=True)
sys.exit(1 if FAIL else 0)
