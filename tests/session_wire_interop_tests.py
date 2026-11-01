import json
from pathlib import Path
import subprocess
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from sdk.session_wire import MAGIC, encode_request, decode_response
from sdk import session_v1_pb2 as pb

bridge = sys.argv.pop(1)

def run(operation, payload):
    return subprocess.run([bridge, operation], input=payload, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, check=True).stdout

class CrossLanguageTests(unittest.TestCase):
    def test_python_request_to_cpp(self):
        # Valid values on purpose: the C++ decoder now rejects what the business
        # types reject (empty cwd, zero wall_ms), so a fixture with those would be
        # testing that two front doors disagree.
        request = {'op':'execute', 'request_id':'r1', 'argv':['/bin/true'], 'env':{},
                   'cwd':'/tmp', 'wall_ms':1000}
        result = json.loads(run('decode-request', encode_request(request)))
        self.assertEqual(result, {'protocol_version':2, **request})
        data = bytes([0,255])*2048
        packet = encode_request({'op':'stdin','command_id':'cmd-1','bytes':list(data)})
        decoded = json.loads(run('decode-request', packet))
        self.assertEqual(bytes(decoded['bytes']), data)

    def test_cpp_request_to_python(self):
        request = {'protocol_version':2, 'op':'execute', 'request_id':'r1', 'argv':['/bin/true'], 'env':{}}
        packet = run('encode-request', json.dumps(request).encode())
        message = pb.SessionRequest.FromString(packet[4:])
        self.assertEqual(message.execute.request_id, 'r1')
        self.assertTrue(message.execute.HasField('env'))
        self.assertFalse(message.execute.HasField('cwd'))

    def test_cpp_response_to_python(self):
        value = {'request':{'op':'result'},'answer':{'command_id':'cmd-1','state':'interrupted',
                 'exit_code':None,'signal':0,'metrics':{'counter':2**64-1}}}
        packet = run('encode-response', json.dumps(value).encode())
        self.assertEqual(decode_response(packet), value['answer'])

    def test_python_response_to_cpp(self):
        response=pb.SessionResponse(protocol_version=1)
        response.output.offset, response.output.next_offset = 3, 6
        response.output.data=b'\x00\xff\x01'
        value=json.loads(run('decode-response',MAGIC+response.SerializeToString()))
        self.assertEqual(value, {'offset':3, 'next_offset':6, 'bytes':[0,255,1], 'finished':False})

if __name__ == '__main__':
    unittest.main()
