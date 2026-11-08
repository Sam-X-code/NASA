import sys
from bridge import MockBridge, XV6Shell
from tools import Tools, validate_action
from agent import Agent, RulePlanner, InjectedPlanner

# usage: python3 main.py mock        (no QEMU)
#        python3 main.py qemu ../xv6-riscv
def make_bridge():
    if len(sys.argv) > 2 and sys.argv[1] == "qemu":
        return XV6Shell(sys.argv[2])
    return MockBridge()

results = []
def record(name, cond, detail=""):
    results.append((name, "PASS" if cond else "FAIL", detail))

bridge = make_bridge()

# 1-3: normal tools via the deterministic planner
tools = Tools(bridge, confirm=lambda a: False)      # destructive actions need confirmation
agent = Agent(RulePlanner(), tools)

out = agent.run("list")
record("list files", "report.txt" in out)

out = agent.run("read /report.txt")
record("read trusted report", "100 requests" in out)

out = agent.run("summarize /report.txt")
record("summarize report", "100 requests" in out)

# 4: injection demo, layer 1 blocks the delete
out = Agent(InjectedPlanner(), tools).run("summarize /inbox.txt")
after = Agent(RulePlanner(), tools).run("list")
record("injection: delete blocked by confirmation", "important.txt" in after)

# 5: malformed or unsafe tool calls are rejected before reaching xv6
bad_actions = [
    {"tool": "rm_rf", "args": {}},
    {"tool": "read_file", "args": {"path": "/../etc/passwd"}},
    {"tool": "read_file", "args": {}},
    {"tool": "read_file", "args": {"path": 5}},
    {"tool": "read_file", "args": {"path": "/report.txt; rm /important.txt"}},
]
for bad in bad_actions:
    try:
        validate_action(bad)
        record(f"reject {bad}", False)
    except ValueError as e:
        record(f"reject {bad['tool']} {bad['args']}", True, str(e))

# 6: missing file returns an error observation, not fake success
out = Agent(RulePlanner(), tools).run("read /nothere.txt")
record("missing/disallowed path handled", "rejected" in out or "cannot" in out, out)

# 7: layer-1 limitation: if confirmation is bypassed, the file IS deleted
unsafe_tools = Tools(bridge, confirm=lambda a: True)
Agent(InjectedPlanner(), unsafe_tools).run("summarize /inbox.txt")
after = Agent(RulePlanner(), unsafe_tools).run("list")
record("limitation: bypassed confirm deletes file (why kernel policy is needed)",
       "important.txt" not in after)

bridge.close()
print(f"{'TEST':70} RESULT")
for n, s, d in results:
    print(f"{n[:70]:70} {s}")