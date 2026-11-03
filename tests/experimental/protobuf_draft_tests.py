"""Presence checks retained from the early contract prototype."""
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from sdk import session_v1_pb2 as wire


class PresenceAndWireTests(unittest.TestCase):
    def test_empty_environment_is_distinct_from_missing(self):
        absent = wire.ExecuteRequest(request_id="one", argv=["/bin/true"])
        empty = wire.ExecuteRequest(request_id="one", argv=["/bin/true"])
        empty.env.SetInParent()
        self.assertFalse(wire.ExecuteRequest.FromString(absent.SerializeToString()).HasField("env"))
        self.assertTrue(wire.ExecuteRequest.FromString(empty.SerializeToString()).HasField("env"))

    def test_optional_zero_and_missing_are_distinct(self):
        request = wire.ExecuteRequest(wall_ms=0, cwd="")
        parsed = wire.ExecuteRequest.FromString(request.SerializeToString())
        self.assertTrue(parsed.HasField("wall_ms"))
        self.assertTrue(parsed.HasField("cwd"))
        self.assertEqual(parsed.wall_ms, 0)
        self.assertFalse(wire.ExecuteRequest().HasField("wall_ms"))

    def test_arbitrary_bytes_and_oneof_roundtrip(self):
        request = wire.SessionRequest(protocol_version=1)
        request.stdin_write.command_id = "cmd-1"
        request.stdin_write.data = bytes([0, 255, 1]) * 1000
        parsed = wire.SessionRequest.FromString(request.SerializeToString())
        self.assertEqual(parsed.WhichOneof("operation"), "stdin_write")
        self.assertEqual(parsed.stdin_write.data, request.stdin_write.data)
        parsed.status.SetInParent()
        self.assertEqual(parsed.WhichOneof("operation"), "status")

    def test_unknown_and_zero_exit_are_distinct(self):
        pending = wire.CommandRecord()
        done = wire.CommandRecord(exit_code=0)
        self.assertFalse(pending.HasField("exit_code"))
        self.assertTrue(wire.CommandRecord.FromString(done.SerializeToString()).HasField("exit_code"))


if __name__ == "__main__":
    unittest.main()
