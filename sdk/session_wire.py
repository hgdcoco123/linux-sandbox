"""External session protocol mapping; generated classes stay internal to the SDK."""
import json
from . import session_v1_pb2 as pb
from .sandbox_client import SandboxError

MAX_PACKET = 16384
MAGIC = b"SBP1"


def encode_request(value):
    op = value["op"]
    allowed = {
        "execute": {"op", "request_id", "argv", "cwd", "env", "wall_ms"},
        "status": {"op"}, "close": {"op"}, "cancel": {"op", "command_id"},
        "stdin": {"op", "command_id", "bytes"}, "stdin-close": {"op", "command_id"},
        "result": {"op", "command_id"}, "output": {"op", "command_id", "stream", "offset"},
        "request-status": {"op", "request_id"}, "files": {"op", "request"},
    }
    if op not in allowed or set(value) - allowed[op]:
        raise ValueError("unknown session operation or field")
    request = pb.SessionRequest(protocol_version=1, application_version=2)
    if op == "execute":
        command = request.execute
        command.request_id = value["request_id"]
        command.argv.extend(value["argv"])
        if "cwd" in value:
            command.cwd = value["cwd"]
        if "env" in value:
            command.env.SetInParent()
            command.env.values.update(value["env"])
        if "wall_ms" in value:
            command.wall_ms = value["wall_ms"]
    elif op in ("status", "close"):
        getattr(request, op).SetInParent()
    elif op == "cancel":
        request.cancel.SetInParent()
        if "command_id" in value:
            request.cancel.command_id = value["command_id"]
    elif op == "stdin":
        request.stdin_write.command_id = value["command_id"]
        request.stdin_write.data = bytes(value["bytes"])
        if len(request.stdin_write.data) > 4096:
            raise ValueError("stdin exceeds 4096 bytes")
    elif op in ("stdin-close", "result"):
        field = request.stdin_close if op == "stdin-close" else request.result
        field.command_id = value["command_id"]
    elif op == "output":
        request.output.command_id = value["command_id"]
        request.output.stream = {"stdout": pb.STDOUT, "stderr": pb.STDERR}[value.get("stream", "stdout")]
        request.output.offset = value.get("offset", 0)
    elif op == "request-status":
        request.request_status.request_id = value["request_id"]
    elif op == "files":
        item = value["request"]
        action = item["op"]
        keys = {"snapshot": {"op", "source", "name"}, "manifest": {"op"},
                "export": {"op", "destination"}}
        if action not in keys or set(item) - keys[action]:
            raise ValueError("unknown workspace operation or field")
        if action == "snapshot":
            request.workspace.snapshot.source = item["source"]
            request.workspace.snapshot.name = item["name"]
        elif action == "manifest":
            request.workspace.manifest.SetInParent()
        elif action == "export":
            request.workspace.export_files.destination = item["destination"]
        else:
            raise ValueError("unknown workspace operation")
    else:
        raise ValueError("unknown session operation")
    packet = MAGIC + request.SerializeToString()
    if len(packet) > MAX_PACKET:
        raise ValueError("encoded session packet exceeds 16KiB")
    return packet


def _command(value):
    result = {"state": value.state}
    if value.command_id:
        result["command_id"] = value.command_id
    for name in ("request_id", "cwd", "stop_reason", "execution_ms", "outcome_known", "schema_version"):
        if value.HasField(name):
            result[name] = getattr(value, name)
    if value.has_argv:
        result["argv"] = list(value.argv)
    for name in ("exit_code", "signal", "exec_errno"):
        if getattr(value, "has_" + name + "_field"):
            result[name] = getattr(value, name) if value.HasField(name) else None
    for name in ("stdout", "stderr"):
        field = name + "_stream"
        if value.HasField(field):
            stream = getattr(value, field)
            result[name] = {key: getattr(stream, key) for key in (
                "saved_bytes", "discarded_bytes", "eof", "flushed", "queue_peak_bytes",
                "backpressure_events", "failed_bytes", "write_error", "seen", "stored")}
    if value.HasField("metrics_json"):
        result["metrics"] = json.loads(value.metrics_json)
    return result


def _decode_response(packet):
    if len(packet) > MAX_PACKET or not packet.startswith(MAGIC):
        raise SandboxError("invalid session response framing", code="PROTOCOL_ERROR")
    response = pb.SessionResponse()
    try:
        response.ParseFromString(packet[len(MAGIC):])
    except Exception as error:
        raise SandboxError("invalid session response", code="PROTOCOL_ERROR") from error
    if response.protocol_version != 1:
        raise SandboxError("unsupported session protocol version", code="PROTOCOL_ERROR")
    outcome = response.WhichOneof("outcome")
    if outcome == "error":
        error = response.error
        exception = SandboxError(error.message, error.code, error.retryable)
        if error.HasField("request_id"):
            exception.request_id = error.request_id
        raise exception
    if outcome == "command":
        return _command(response.command)
    if outcome == "status":
        status = response.status
        return {"run_id": status.run_id, "schema_version": status.schema_version,
                "protocol_version": status.session_protocol_version,
                "session_state": status.session_state, "commands": status.commands,
                "current": _command(status.current) if status.HasField("current") else None,
                "last": _command(status.last) if status.HasField("last") else None,
                "metrics": json.loads(status.metrics_json)}
    if outcome == "input":
        return {"accepted_bytes": response.input.accepted_bytes}
    if outcome == "output":
        output = response.output
        return {"offset": output.offset, "next_offset": output.next_offset,
                "bytes": list(output.data), "finished": output.finished}
    if outcome == "acknowledgement":
        ack = response.acknowledgement
        result = {}
        if ack.status:
            result["status"] = ack.status
        for name in ("active", "closed"):
            if ack.HasField(name):
                result[name] = getattr(ack, name)
        return result
    if outcome == "workspace":
        workspace = response.workspace
        entries = []
        for entry in workspace.entries:
            item = {"path": entry.path, "type": entry.type}
            for name in ("size", "sha256"):
                if entry.HasField(name):
                    item[name] = getattr(entry, name)
            entries.append(item)
        result = {"bytes": workspace.bytes, "entries": entries,
                  "entry_count": workspace.entry_count,
                  "manifest_truncated": workspace.manifest_truncated}
        for name in ("complete", "destination"):
            if workspace.HasField(name):
                result[name] = getattr(workspace, name)
        return result
    raise SandboxError("missing session response outcome", code="PROTOCOL_ERROR")


def decode_response(packet):
    try:
        return _decode_response(packet)
    except SandboxError:
        raise
    except (ValueError, TypeError, KeyError, UnicodeError) as error:
        raise SandboxError("invalid session response fields", code="PROTOCOL_ERROR") from error
