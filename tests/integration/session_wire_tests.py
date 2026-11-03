import json
import stat
import struct
import tempfile
from pathlib import Path
import sys
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from sdk import session_v1_pb2 as pb
from sdk.session_wire import MAGIC, encode_request, decode_response
from sdk.sandbox_client import SandboxError
from sdk.session_client import SessionHandle


class SessionWireTests(unittest.TestCase):
    def test_environment_presence_and_binary_input(self):
        base = {'op': 'execute', 'request_id': 'r1', 'argv': ['/bin/true']}
        absent = pb.SessionRequest.FromString(encode_request(base)[4:])
        empty = pb.SessionRequest.FromString(encode_request({**base, 'env': {}, 'wall_ms': 0, 'cwd': ''})[4:])
        self.assertFalse(absent.execute.HasField('env'))
        self.assertTrue(empty.execute.HasField('env'))
        self.assertTrue(empty.execute.HasField('wall_ms'))
        self.assertTrue(empty.execute.HasField('cwd'))
        self.assertEqual(empty.application_version, 2)
        data = bytes([0, 255]) * 2048
        request = pb.SessionRequest.FromString(encode_request({'op': 'stdin', 'command_id': 'cmd-1', 'bytes': list(data)})[4:])
        self.assertEqual(request.stdin_write.data, data)
        with self.assertRaises(ValueError):
            encode_request({'op': 'stdin', 'command_id': 'cmd-1', 'bytes': [0] * 4097})

    def test_response_preserves_large_metrics_nulls_and_zero(self):
        response = pb.SessionResponse(protocol_version=1)
        command = response.command
        command.command_id, command.state = 'cmd-1', 'interrupted'
        command.has_exit_code_field = True
        command.exit_code = 0
        command.has_signal_field = True
        command.metrics_json = json.dumps({'counter': 2**64 - 1, 'missing': None}).encode()
        result = decode_response(MAGIC + response.SerializeToString())
        self.assertEqual(result['exit_code'], 0)
        self.assertIsNone(result['signal'])
        self.assertNotIn('exec_errno', result)
        self.assertEqual(result['metrics'], {'counter': 2**64 - 1, 'missing': None})

    def test_errors_and_invalid_messages(self):
        response = pb.SessionResponse(protocol_version=1)
        response.error.code, response.error.message, response.error.retryable = 'REQUEST_CONFLICT', 'conflict', False
        with self.assertRaises(SandboxError) as caught:
            decode_response(MAGIC + response.SerializeToString())
        self.assertEqual(caught.exception.code, 'REQUEST_CONFLICT')
        for data in (b'', MAGIC + b'\xff', MAGIC + pb.SessionResponse(protocol_version=2).SerializeToString()):
            with self.assertRaises(SandboxError):
                decode_response(data)

    def test_binary_output(self):
        response = pb.SessionResponse(protocol_version=1)
        response.output.offset, response.output.next_offset = 7, 10
        response.output.data = b'\x00\xff\x01'
        self.assertEqual(decode_response(MAGIC + response.SerializeToString())['bytes'], [0, 255, 1])

    def test_direct_request_does_not_spawn_cli_or_create_request_file(self):
        with tempfile.TemporaryDirectory() as root:
            session = SessionHandle(Mock(), Path(root), 'run-' + 'a'*24)
            response = pb.SessionResponse(protocol_version=1)
            response.input.accepted_bytes = 2
            channel = Mock()
            channel.__enter__ = Mock(return_value=channel)
            channel.__exit__ = Mock(return_value=False)
            channel.getsockopt.return_value = struct.pack('3i', 123, 1000, 1000)
            channel.send.side_effect = lambda packet: len(packet)
            channel.recvmsg.return_value = (MAGIC + response.SerializeToString(), [], 0, None)
            info = Mock(st_mode=stat.S_IFDIR | 0o700, st_uid=1000)
            with patch('pathlib.Path.lstat', return_value=info), \
                 patch('sdk.session_client.os.getuid', return_value=1000, create=True), \
                 patch('sdk.session_client.socket.AF_UNIX', 1, create=True), \
                 patch('sdk.session_client.socket.SO_PEERCRED', 17, create=True), \
                 patch('sdk.session_client.socket.CMSG_SPACE', return_value=64, create=True), \
                 patch('sdk.session_client.socket.socket', return_value=channel), \
                 patch('sdk.session_client.tempfile.NamedTemporaryFile', side_effect=AssertionError('temporary file')):
                result = session.request(op='stdin', command_id='cmd-1', bytes=[0, 255, 1])
            self.assertEqual(result, {'accepted_bytes': 2})
            packet = channel.send.call_args.args[0]
            self.assertEqual(pb.SessionRequest.FromString(packet[4:]).stdin_write.data, b'\x00\xff\x01')
            session.client._command.assert_not_called()


if __name__ == '__main__':
    unittest.main()
