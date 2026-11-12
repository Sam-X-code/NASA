import unittest

from bridge import MockBridge, XV6Shell
from tools import Tools, validate_action


class RecordingXV6Shell(XV6Shell):
    def __init__(self, response):
        self.commands = []
        self.response = response

    def _shell(self, command):
        self.commands.append(command)
        return self.response


class EffectBridgeTests(unittest.TestCase):
    def test_append_requires_a_key_and_replays_once_in_mock(self):
        bridge = MockBridge()
        tools = Tools(bridge)
        action = {"tool": "append_file", "args": {"path": "/report.txt", "data": "XY"}}
        blocked = tools.execute(action)
        self.assertFalse(blocked["ok"])
        key = 0x1234567890ABCDEF
        first = tools.execute(action, effect_key=key)
        second = tools.execute(action, effect_key=key)
        self.assertTrue(first["ok"])
        self.assertEqual(first, second)
        self.assertTrue(bridge.files["/report.txt"].endswith("XY"))
        self.assertEqual(bridge.files["/report.txt"].count("XY"), 1)

    def test_payload_is_bounded_and_shell_safe(self):
        action = {"tool": "append_file", "args": {"path": "/report.txt", "data": "A; rm x"}}
        validate_action(action)
        bridge = RecordingXV6Shell("EFFECT_RESULT:7")
        result = bridge.run("append_file", action["args"], effect_key=0x123)
        self.assertTrue(result["ok"])
        self.assertEqual(bridge.commands[0],
                         "effectctl append 0000000000000123 /report.txt 413b20726d2078")
        self.assertNotIn(";", bridge.commands[0].split("/report.txt ", 1)[1])

    def test_create_bridge_command(self):
        bridge = RecordingXV6Shell("EFFECT_RESULT:0")
        result = bridge.run("create_file", {"path": "/agentlog.txt"}, effect_key=1)
        self.assertTrue(result["ok"])
        self.assertEqual(bridge.commands[0],
                         "effectctl create 0000000000000001 /agentlog.txt")

    def test_persistent_bridge_refuses_missing_image(self):
        import os
        import tempfile
        from bridge import BridgeError
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(BridgeError):
                XV6Shell(directory, fresh=False)

    def test_persistent_bridge_refuses_stale_image_instead_of_rebuilding(self):
        import os
        import tempfile
        from bridge import BridgeError
        with tempfile.TemporaryDirectory() as directory:
            with open(os.path.join(directory, "fs.img"), "wb") as handle:
                handle.write(b"not a real xv6 image")
            with self.assertRaises(BridgeError):
                XV6Shell(directory, fresh=False)

    def test_payload_over_shell_transport_limit_rejected(self):
        with self.assertRaises(ValueError):
            validate_action({"tool": "append_file",
                             "args": {"path": "/report.txt", "data": "x" * 21}})


if __name__ == "__main__":
    unittest.main()
