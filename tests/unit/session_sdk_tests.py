"""Deterministic SDK close-race tests; no Linux host or binary required."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from sdk.session_client import SessionHandle, CommandHandle, SandboxError


class CloseRaceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.session = SessionHandle(None, self.root, "run-test")
        self.session.directory.mkdir()
        self.save("idle")

    def save(self, state):
        (self.session.directory / "session.json").write_text(
            json.dumps({"session_state": state}), encoding="utf-8")

    def test_status_request_loses_race_to_close(self):
        def request(**kw):
            self.save("closed")
            raise SandboxError("invalid/missing response")
        self.session.request = Mock(side_effect=request)
        self.assertEqual(self.session.status()["session_state"], "closed")

    def test_live_transport_failure_is_not_hidden(self):
        self.session.request = Mock(side_effect=SandboxError("connection failed"))
        with self.assertRaisesRegex(SandboxError, "connection failed"):
            self.session.status()

    def test_close_does_not_send_status_rpc(self):
        def request(**kw):
            self.assertEqual(kw["op"], "close")
            self.assertGreaterEqual(kw["_timeout"], 0)
            self.save("closed")
            return {"status": "accepted"}
        self.session.request = Mock(side_effect=request)
        self.assertEqual(self.session.close()["session_state"], "closed")
        self.assertEqual(self.session.request.call_count, 1)

    def test_close_request_loses_race_to_host_shutdown(self):
        def request(**kw):
            self.save("closed")
            raise SandboxError("connect session failed")
        self.session.request = Mock(side_effect=request)
        self.assertEqual(self.session.close()["session_state"], "closed")

    def test_close_failure_without_terminal_state_is_not_hidden(self):
        self.session.request = Mock(side_effect=SandboxError("connection failed"))
        with self.assertRaises(SandboxError):
            self.session.close()

    def test_close_wait_timeout(self):
        self.session.request = Mock(return_value={"status": "accepted"})
        with self.assertRaises(TimeoutError):
            self.session.close(timeout=0)

    def test_already_closed_is_idempotent(self):
        self.save("closed")
        self.session.request = Mock(side_effect=AssertionError("unexpected RPC"))
        self.session.close()
        self.session.close()
        self.session.request.assert_not_called()

    def test_cleanup_does_not_mask_original_exception(self):
        original = ValueError("original execution failure")
        self.session.close = Mock(side_effect=SandboxError("cleanup failed"))
        with self.assertRaises(ValueError) as caught:
            with self.session:
                raise original
        self.assertIs(caught.exception, original)

    def test_cleanup_failure_without_body_exception_is_reported(self):
        self.session.close = Mock(side_effect=SandboxError("cleanup failed"))
        with self.assertRaises(SandboxError):
            with self.session:
                pass

    def test_execute_response_loss_keeps_request_id_without_retry(self):
        self.session.request = Mock(side_effect=SandboxError('lost response', code='TRANSPORT_ERROR'))
        with self.assertRaises(SandboxError) as caught:
            self.session.execute(['/bin/true'], request_id='once')
        self.assertEqual(caught.exception.request_id, 'once')
        self.session.request.assert_called_once()

    def test_wait_propagates_remaining_deadline_and_does_not_cancel(self):
        command = CommandHandle(self.session, 'cmd-1')
        self.session.request = Mock(return_value={'state':'pending'})
        with self.assertRaises(TimeoutError):
            command.wait(timeout=0.03)
        for call in self.session.request.call_args_list:
            self.assertEqual(call.kwargs['op'], 'result')
            self.assertGreater(call.kwargs['_timeout'], 0)
            self.assertLessEqual(call.kwargs['_timeout'], 0.031)


if __name__ == "__main__":
    unittest.main()
