import re

PATH_RE = re.compile(r"^/[A-Za-z0-9_.-]{1,13}$")  # xv6 names are max 14 chars
ALLOWED_PATHS = {"/report.txt", "/inbox.txt", "/important.txt", "/agentlog.txt"}
MAX_EFFECT_DATA_BYTES = 20

TOOLS = {
    "list_files":  {"required": {}, "destructive": False, "effect": False},
    "read_file":   {"required": {"path": str}, "destructive": False, "effect": False},
    "summarize":   {"required": {"text": str}, "destructive": False, "effect": False},
    "delete_file": {"required": {"path": str}, "destructive": True, "effect": False},
    "append_file": {"required": {"path": str, "data": str}, "destructive": False, "effect": True},
    "create_file": {"required": {"path": str}, "destructive": False, "effect": True},
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
        path = args["path"]
        if not PATH_RE.fullmatch(path) or path not in ALLOWED_PATHS:
            raise ValueError(f"path not allowed: {path!r}")
    if name == "append_file":
        encoded = args["data"].encode("utf-8")
        if not encoded:
            raise ValueError("append payload cannot be empty")
        if len(encoded) > MAX_EFFECT_DATA_BYTES:
            raise ValueError(f"append payload exceeds {MAX_EFFECT_DATA_BYTES} bytes")
        if "\x00" in args["data"]:
            raise ValueError("append payload cannot contain NUL")
    return action


class Tools:
    def __init__(self, bridge, confirm=lambda action: False):
        self.bridge = bridge
        self.confirm = confirm

    def execute(self, action, effect_key=None):
        validate_action(action)
        name, args = action["tool"], action["args"]
        if TOOLS[name]["destructive"] and not self.confirm(action):
            return {"ok": False, "output": "blocked: destructive action not confirmed"}
        if TOOLS[name]["effect"] and (type(effect_key) is not int or effect_key <= 0
                                      or effect_key >= (1 << 64)):
            return {"ok": False, "output": "blocked: valid uint64 effect key required"}
        if name == "summarize":
            lines = [line for line in args["text"].splitlines() if line.strip()]
            return {"ok": True, "output": " ".join(lines)[:300]}
        return self.bridge.run(name, args, effect_key=effect_key)
