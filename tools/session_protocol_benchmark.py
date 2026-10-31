#!/usr/bin/env python3
"""Compare the legacy CLI path, direct JSON, and direct Protobuf on an idle session."""
import argparse
import json
import math
import os
from pathlib import Path
import platform
import resource
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from sdk.session_client import SessionClient
from sdk.session_wire import encode_request

def percentile(values, fraction):
    return sorted(values)[max(0, math.ceil(len(values)*fraction)-1)]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True)
    parser.add_argument('--legacy-binary', required=True, help='old CLI that sends JSON to the control socket')
    parser.add_argument('--state-root', type=Path, required=True)
    parser.add_argument('--run-id', required=True)
    parser.add_argument('--iterations', type=int, default=500)
    args = parser.parse_args()
    if not 1 <= args.iterations <= 100000:
        parser.error('iterations must be 1..100000')
    session = SessionClient(args.binary, args.state_root).attach(args.state_root, args.run_id)
    if session.status()['session_state'] != 'idle':
        raise RuntimeError('benchmark requires an idle session')
    json_request = json.dumps({'protocol_version': 2, 'op': 'status'}).encode()
    def legacy():
        name = None
        try:
            with tempfile.NamedTemporaryFile(dir=session.state_root, suffix='.json', delete=False) as f:
                name = Path(f.name); f.write(json_request)
            result = subprocess.run([args.legacy_binary, 'session-request', session.run_id, '--request', str(name),
                                     '--state-root', str(session.state_root)], capture_output=True, check=True, timeout=5)
            json.loads(result.stdout)
        finally:
            if name: name.unlink(missing_ok=True)
    def direct_json():
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as channel:
            channel.settimeout(5); channel.connect(str(session.directory/'control.sock'))
            channel.sendall(json_request)
            reply = json.loads(channel.recv(16384))
            if not reply['ok']: raise RuntimeError(reply)
    def protobuf():
        session.request(op='status')
    def cpu():
        own = resource.getrusage(resource.RUSAGE_SELF)
        children = resource.getrusage(resource.RUSAGE_CHILDREN)
        return own.ru_utime + own.ru_stime + children.ru_utime + children.ru_stime
    results = {}
    for name, call in [('A_cli_json', legacy), ('B_direct_json', direct_json), ('C_direct_protobuf', protobuf)]:
        for _ in range(min(10,args.iterations)): call()
        samples = []; cpu_start = cpu(); started = time.perf_counter()
        for _ in range(args.iterations):
            t = time.perf_counter_ns(); call(); samples.append((time.perf_counter_ns()-t)/1e6)
        elapsed = time.perf_counter()-started
        results[name] = {'p50_ms':percentile(samples,.5), 'p95_ms':percentile(samples,.95),
                         'p99_ms':percentile(samples,.99), 'requests_per_second':args.iterations/elapsed,
                         'client_and_child_cpu_seconds':cpu()-cpu_start}
    sizes = {}
    for count in (16,4096):
        request = {'op':'stdin','command_id':'cmd-1','bytes':[255]*count}
        sizes[str(count)] = {'json':len(json.dumps({'protocol_version':2,**request}).encode()),
                             'protobuf':len(encode_request(request))}
    print(json.dumps({'environment':platform.platform(), 'python':platform.python_version(),
        'iterations':args.iterations, 'connections':'one per request', 'status_request_bytes':
        {'json':len(json_request), 'protobuf':len(encode_request({'op':'status'}))},
        'stdin_request_bytes':sizes, 'results':results}, indent=2))

if __name__ == '__main__':
    main()
