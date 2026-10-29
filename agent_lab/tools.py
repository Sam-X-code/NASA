import re

PATH_RE = re.compile(r"^/[A-Za-z0-9_.-]{1,13}$")      # xv6 names are max 14 chars
ALLOWED_PATHS = {"/report.txt", "/inbox.txt", "/important.txt"}

TOOLS = {
    "list_files":  {"required": {},            "destructive": False},
    "read_file":   {"required": {"path": str}, "destructive": False},
    "summarize":   {"required": {"text": str}, "destructive": False},
    "delete_file": {"required": {"path": str}, "destructive": True},
}

def validate_action(action):
    if not isinstance(action, dict) or "tool" not in action:
        raise ValueError("malformed action")
    name, args = action["tool"], action.get("args", {})
    if name not in TOOLS:
        raise ValueError(f"unknown tool: {name!r}")
    if not isinstance(args, dict):
        raise ValueError("args must be an object")
    spec = TOOLS[name]["required"]
    if set(args) != set(spec):
        raise ValueError(f"{name}: expected args {sorted(spec)}, got {sorted(args)}")
    for key, typ in spec.items():
        if not isinstance(args[key], typ):
            raise ValueError(f"{name}.{key}: wrong type")
    if "path" in args:
        p = args["path"]
        if not PATH_RE.fullmatch(p) or p not in ALLOWED_PATHS:
            raise ValueError(f"path not allowed: {p!r}")
    return action


class Tools:
    def __init__(self, bridge, confirm=lambda action: False):
        self.bridge = bridge
        self.confirm = confirm          # destructive actions need approval

    def execute(self, action):
        validate_action(action)
        name, args = action["tool"], action["args"]
        if TOOLS[name]["destructive"] and not self.confirm(action):
            return {"ok": False, "output": "blocked: destructive action not confirmed"}
        if name == "summarize":         # host-only tool, no guest call
            lines = [l for l in args["text"].splitlines() if l.strip()]
            return {"ok": True, "output": " ".join(lines)[:300]}
        return self.bridge.run(name, args)