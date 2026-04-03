"""完整功能验证：创建文件、读取文件、HTTP 请求、Shell 命令"""
import json, time, urllib.request, urllib.error

BASE = "http://127.0.0.1:8000"
PASS = "\033[92m✓ PASS\033[0m"
FAIL = "\033[91m✗ FAIL\033[0m"


def switch_model(provider, model_name):
    req = urllib.request.Request(
        BASE + "/models/switch",
        data=json.dumps({"provider": provider, "model_name": model_name}).encode(),
        headers={"Content-Type": "application/json"}, method="POST",
    )
    return json.loads(urllib.request.urlopen(req, timeout=10).read().decode())


def run_task(prompt, max_wait=60):
    req = urllib.request.Request(
        BASE + "/agents/tasks",
        data=json.dumps({"tenant_id": "default", "user_id": "tester", "prompt": prompt}).encode(),
        headers={"Content-Type": "application/json"}, method="POST",
    )
    t = json.loads(urllib.request.urlopen(req, timeout=10).read().decode())
    task_id = t["task_id"]
    for i in range(max_wait // 2):
        time.sleep(2)
        cur = json.loads(urllib.request.urlopen(BASE + "/agents/tasks/" + task_id, timeout=10).read().decode())
        if cur["state"] in ("COMPLETED", "FAILED", "CANCELLED"):
            return cur
    return cur


def check(label, condition, detail=""):
    status = PASS if condition else FAIL
    print(f"  {status}  {label}")
    if not condition and detail:
        print(f"         详情: {detail}")
    return condition


print("\n=== 切换到云端模型 ===")
sw = switch_model("cloud", "qwen3-coder-plus")
check("切换成功", sw.get("provider") == "cloud" and sw.get("model_name") == "qwen3-coder-plus", str(sw))

print("\n=== 测试1: 创建文件 ===")
r1 = run_task("帮我在沙盒里创建一个名为 hello.txt 的文件，内容写：你好世界！这是Agent创建的文件。")
print(f"  状态: {r1['state']}")
check("任务完成", r1["state"] == "COMPLETED", r1.get("error", ""))
check("有结果内容", bool(r1.get("result")), "result 为空")
if r1.get("result"):
    print(f"  Agent回复: {r1['result'][:200]}")
# 验证文件是否真的被创建
import pathlib
sandbox = pathlib.Path(r"E:\new_work_space\whimsical_ideas\sandbox")
hello = sandbox / "hello.txt"
check("文件真实存在", hello.exists(), f"文件路径: {hello}")
if hello.exists():
    print(f"  文件内容: {hello.read_text(encoding='utf-8')[:100]}")

print("\n=== 测试2: 读取文件 ===")
r2 = run_task("读取沙盒里的 hello.txt 文件，告诉我里面写了什么内容")
print(f"  状态: {r2['state']}")
check("任务完成", r2["state"] == "COMPLETED", r2.get("error", ""))
if r2.get("result"):
    print(f"  Agent回复: {r2['result'][:200]}")

print("\n=== 测试3: Shell命令 ===")
r3 = run_task("用shell命令列出沙盒目录里的所有文件（用 dir 命令）")
print(f"  状态: {r3['state']}")
check("任务完成", r3["state"] == "COMPLETED", r3.get("error", ""))
if r3.get("result"):
    print(f"  Agent回复: {r3['result'][:300]}")

print("\n=== 测试4: 内存写入+读取 ===")
r4 = run_task("把这句话存入记忆：用户喜欢Python编程。然后再读出来确认。")
print(f"  状态: {r4['state']}")
check("任务完成", r4["state"] == "COMPLETED", r4.get("error", ""))
if r4.get("result"):
    print(f"  Agent回复: {r4['result'][:200]}")

print()
