import json
import os
import tempfile
import unittest

from agent import Agent, RulePlanner
from bridge import MockBridge
from tools import Tools


class SafeAssessment:
    def assess(self, path, text):
        return {"status": "REVIEWED_SAFE", "reason": "test assessor"}


class FailedAssessment:
    def assess(self, path, text):
        raise RuntimeError("assessment service unavailable")


class AgentAssessmentTests(unittest.TestCase):
    def run_with_assessor(self, assessor):
        with tempfile.TemporaryDirectory() as directory:
            trace = os.path.join(directory, "trace.jsonl")
            agent = Agent(
                RulePlanner(),
                Tools(MockBridge()),
                log_path=trace,
                assessor=assessor,
            )
            agent.run("read /inbox.txt")
            with open(trace) as stream:
                return json.loads(stream.readline())["observation"]["assessment"]

    def test_assessment_is_recorded(self):
        self.assertEqual(
            self.run_with_assessor(SafeAssessment())["status"],
            "REVIEWED_SAFE",
        )

    def test_assessment_failure_is_uncertain(self):
        result = self.run_with_assessor(FailedAssessment())
        self.assertEqual(result["status"], "UNCERTAIN")
        self.assertIn("assessment service unavailable", result["reason"])


if __name__ == "__main__":
    unittest.main()
