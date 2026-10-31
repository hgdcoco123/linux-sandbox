#include "sandbox/core/wire.hpp"

#include "sandbox/core/binary.hpp"
#include "sandbox/core/record_json.hpp"

#include <cstdint>
#include <limits>
#include <type_traits>

namespace sandbox {
namespace {

enum class Kind : std::uint8_t { request = 0, reply = 1 };

enum class RequestTag : std::uint8_t {
    execute = 1,
    status = 2,
    close = 3,
    cancel = 4,
    stdin_write = 5,
    stdin_close = 6,
    read_output = 7,
    read_result = 8,
    request_status = 9,
    workspace = 10,
};

enum class ReplyTag : std::uint8_t {
    accepted = 1,
    output = 2,
    command_status = 3,
    session_status = 4,
    result = 5,
    request_status = 6,
    manifest = 7,
    error = 8,
};

enum class WorkspaceTag : std::uint8_t { snapshot = 1, manifest = 2, export_files = 3 };

[[noreturn]] void fail(std::string code, std::string message) {
    throw WireError(std::move(code), std::move(message));
}

// Packet-level errors from the shared binary primitives are reported as wire
// errors with the channel's own code.
struct Errors {
    [[noreturn]] static void fail(std::string code, std::string message) {
        if (code == "BAD_PACKET")
            code = "BAD_WIRE_PACKET";
        throw WireError(std::move(code), std::move(message));
    }
};
using Writer = binary::Writer;
using Reader = binary::Reader<Errors>;
void encode_outcome(Writer &writer, const ExecutionOutcome &outcome) {
    const auto &value = outcome.variant();
    if (std::holds_alternative<Exited>(value)) {
        writer.u8(0);
        writer.varint(static_cast<std::uint64_t>(std::get<Exited>(value).code));
    } else if (std::holds_alternative<Signaled>(value)) {
        writer.u8(1);
        writer.varint(static_cast<std::uint64_t>(std::get<Signaled>(value).signal));
    } else if (std::holds_alternative<TimedOut>(value)) {
        writer.u8(2);
    } else if (std::holds_alternative<Cancelled>(value)) {
        writer.u8(3);
    } else if (std::holds_alternative<Oom>(value)) {
        writer.u8(4);
    } else if (std::holds_alternative<StartupFailed>(value)) {
        writer.u8(5);
        writer.text(std::get<StartupFailed>(value).reason);
    } else if (std::holds_alternative<SupervisionFailed>(value)) {
        writer.u8(6);
        writer.text(std::get<SupervisionFailed>(value).reason);
    } else {
        writer.u8(7);
        writer.text(std::get<Indeterminate>(value).reason);
    }
}

ExecutionOutcome decode_outcome(Reader &reader) {
    switch (reader.u8()) {
    case 0:
        return ExecutionOutcome(Exited{static_cast<int>(reader.varint())});
    case 1:
        return ExecutionOutcome(Signaled{static_cast<int>(reader.varint())});
    case 2:
        return ExecutionOutcome(TimedOut{});
    case 3:
        return ExecutionOutcome(Cancelled{});
    case 4:
        return ExecutionOutcome(Oom{});
    case 5:
        return ExecutionOutcome(StartupFailed{reader.text()});
    case 6:
        return ExecutionOutcome(SupervisionFailed{reader.text()});
    case 7:
        return ExecutionOutcome(Indeterminate{reader.text()});
    default:
        fail("BAD_WIRE_PACKET", "unknown outcome tag");
    }
}

void encode_request_fields(Writer &writer, const ExecutionRequest &request) {
    writer.varint(request.argv().size());
    for (const auto &argument : request.argv())
        writer.text(argument);
    writer.optional(request.cwd(), [&](const auto &path) {
        auto text = path.u8string();
        writer.text(std::string_view(reinterpret_cast<const char *>(text.data()), text.size()));
    });
    writer.optional(request.env(), [&](const auto &environment) {
        writer.varint(environment.size());
        for (const auto &[name, value] : environment) {
            writer.text(name);
            writer.text(value);
        }
    });
    writer.optional(request.timeout(), [&](const auto &timeout) {
        writer.varint(static_cast<std::uint64_t>(timeout.count()));
    });
    writer.optional(request.key(), [&](const auto &key) { writer.text(key.str()); });
}

std::string encode_workspace(const Workspace &workspace) {
    Writer writer;
    std::visit(
        [&](const auto &value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, WorkspaceSnapshot>) {
                writer.u8(static_cast<std::uint8_t>(WorkspaceTag::snapshot));
                writer.text(value.source);
                writer.text(value.name);
            } else if constexpr (std::is_same_v<T, WorkspaceManifest>) {
                writer.u8(static_cast<std::uint8_t>(WorkspaceTag::manifest));
            } else {
                writer.u8(static_cast<std::uint8_t>(WorkspaceTag::export_files));
                writer.text(value.destination);
            }
        },
        workspace.operation);
    return std::move(writer.data);
}

Workspace decode_workspace(Reader &reader) {
    switch (reader.u8()) {
    case static_cast<std::uint8_t>(WorkspaceTag::snapshot):
        return Workspace{WorkspaceSnapshot{reader.text(), reader.text()}};
    case static_cast<std::uint8_t>(WorkspaceTag::manifest):
        return Workspace{WorkspaceManifest{}};
    case static_cast<std::uint8_t>(WorkspaceTag::export_files):
        return Workspace{WorkspaceExport{reader.text()}};
    default:
        fail("BAD_WIRE_PACKET", "unknown workspace tag");
    }
}

std::string frame(Kind kind, std::uint8_t tag, const std::string &payload) {
    Writer writer;
    writer.data.append(wire_magic);
    writer.u8(static_cast<std::uint8_t>(kind));
    writer.u8(tag);
    writer.varint(payload.size());
    writer.data.append(payload);
    if (writer.data.size() > max_wire_packet)
        fail("PACKET_TOO_LARGE", "packet exceeds 16KiB");
    return std::move(writer.data);
}

struct Frame {
    Kind kind;
    std::uint8_t tag;
    std::string_view payload;
};

Frame unframe(std::string_view packet) {
    if (packet.size() > max_wire_packet)
        fail("PACKET_TOO_LARGE", "packet exceeds 16KiB");
    if (packet.size() < wire_magic.size() + 2)
        fail("BAD_WIRE_PACKET", "packet too short");
    if (packet.substr(0, wire_magic.size()) != wire_magic)
        fail("BAD_WIRE_PACKET", "bad magic");
    Reader reader(packet.substr(wire_magic.size()));
    auto kind_byte = reader.u8();
    if (kind_byte > 1)
        fail("BAD_WIRE_PACKET", "unknown packet kind");
    auto tag = reader.u8();
    auto length = reader.varint();
    if (length > reader.remaining())
        fail("BAD_WIRE_PACKET", "payload length exceeds packet");
    if (length != reader.remaining())
        fail("BAD_WIRE_PACKET", "trailing bytes after payload");
    auto payload = packet.substr(packet.size() - static_cast<std::size_t>(length),
                                 static_cast<std::size_t>(length));
    return Frame{static_cast<Kind>(kind_byte), tag, payload};
}

} // namespace

std::string encode(const SessionRequest &request) {
    Writer writer;
    std::visit(
        [&](const auto &value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, Execute>) {
                writer.u8(static_cast<std::uint8_t>(RequestTag::execute));
                Writer fields;
                encode_request_fields(fields, value.request);
                writer.text(fields.data);
            } else if constexpr (std::is_same_v<T, Status>) {
                writer.u8(static_cast<std::uint8_t>(RequestTag::status));
            } else if constexpr (std::is_same_v<T, Close>) {
                writer.u8(static_cast<std::uint8_t>(RequestTag::close));
            } else if constexpr (std::is_same_v<T, Cancel>) {
                writer.u8(static_cast<std::uint8_t>(RequestTag::cancel));
                writer.optional(value.command, [&](const auto &index) { writer.varint(index.number()); });
            } else if constexpr (std::is_same_v<T, StdinWrite>) {
                writer.u8(static_cast<std::uint8_t>(RequestTag::stdin_write));
                writer.varint(value.command.number());
                writer.text(value.bytes);
            } else if constexpr (std::is_same_v<T, StdinClose>) {
                writer.u8(static_cast<std::uint8_t>(RequestTag::stdin_close));
                writer.varint(value.command.number());
            } else if constexpr (std::is_same_v<T, ReadOutput>) {
                writer.u8(static_cast<std::uint8_t>(RequestTag::read_output));
                writer.varint(value.command.number());
                writer.u8(value.stream == Stream::standard_output ? 0 : 1);
                writer.varint(value.offset);
            } else if constexpr (std::is_same_v<T, ReadResult>) {
                writer.u8(static_cast<std::uint8_t>(RequestTag::read_result));
                writer.varint(value.command.number());
            } else if constexpr (std::is_same_v<T, RequestStatusQuery>) {
                writer.u8(static_cast<std::uint8_t>(RequestTag::request_status));
                writer.text(value.key.str());
            } else {
                writer.u8(static_cast<std::uint8_t>(RequestTag::workspace));
                writer.text(encode_workspace(value));
            }
        },
        request.operation());
    return frame(Kind::request, writer.data[0], writer.data.substr(1));
}

SessionRequest decode_request(std::string_view packet) {
    auto parsed = unframe(packet);
    if (parsed.kind != Kind::request)
        fail("BAD_WIRE_PACKET", "expected a request packet");
    Reader reader(parsed.payload);
    SessionOperation operation = Status{};
    switch (static_cast<RequestTag>(parsed.tag)) {
    case RequestTag::execute: {
        auto field_blob = reader.text();
        Reader fields(field_blob);
        auto count = fields.varint();
        if (count == 0 || count > 4096)
            fail("BAD_WIRE_PACKET", "argv length out of range");
        std::vector<std::string> argv;
        for (std::uint64_t i = 0; i < count; ++i)
            argv.push_back(fields.text());
        std::optional<std::filesystem::path> cwd;
        std::optional<std::map<std::string, std::string>> env;
        std::optional<std::chrono::milliseconds> timeout;
        std::optional<RequestKey> key;
        if (fields.boolean()) {
            auto text = fields.text();
            cwd = std::filesystem::path(text);
        }
        if (fields.boolean()) {
            std::map<std::string, std::string> parsed_env;
            auto entries = fields.varint();
            for (std::uint64_t i = 0; i < entries; ++i) {
                auto name = fields.text();
                parsed_env.emplace(std::move(name), fields.text());
            }
            env = std::move(parsed_env);
        }
        if (fields.boolean())
            timeout = std::chrono::milliseconds(static_cast<std::int64_t>(fields.varint()));
        if (fields.boolean()) {
            auto parsed_key = RequestKey::parse(fields.text());
            if (!parsed_key)
                fail("INVALID_REQUEST", "malformed request key");
            key = std::move(parsed_key);
        }
        fields.require_end();
        operation = Execute{ExecutionRequest::create(std::move(argv), std::move(cwd), std::move(env),
                                                     timeout, std::move(key))};
        break;
    }
    case RequestTag::status:
        operation = Status{};
        break;
    case RequestTag::close:
        operation = Close{};
        break;
    case RequestTag::cancel: {
        std::optional<CommandIndex> command;
        if (reader.boolean()) {
            auto index = CommandIndex::from_number(static_cast<std::uint32_t>(reader.varint()));
            if (!index)
                fail("INVALID_REQUEST", "invalid command index");
            command = *index;
        }
        operation = Cancel{command};
        break;
    }
    case RequestTag::stdin_write: {
        auto index = CommandIndex::from_number(static_cast<std::uint32_t>(reader.varint()));
        if (!index)
            fail("INVALID_REQUEST", "invalid command index");
        operation = StdinWrite{*index, reader.text()};
        break;
    }
    case RequestTag::stdin_close: {
        auto index = CommandIndex::from_number(static_cast<std::uint32_t>(reader.varint()));
        if (!index)
            fail("INVALID_REQUEST", "invalid command index");
        operation = StdinClose{*index};
        break;
    }
    case RequestTag::read_output: {
        auto index = CommandIndex::from_number(static_cast<std::uint32_t>(reader.varint()));
        if (!index)
            fail("INVALID_REQUEST", "invalid command index");
        auto stream_byte = reader.u8();
        if (stream_byte > 1)
            fail("INVALID_REQUEST", "unknown stream");
        operation = ReadOutput{*index, stream_byte == 0 ? Stream::standard_output : Stream::standard_error,
                               reader.varint()};
        break;
    }
    case RequestTag::read_result: {
        auto index = CommandIndex::from_number(static_cast<std::uint32_t>(reader.varint()));
        if (!index)
            fail("INVALID_REQUEST", "invalid command index");
        operation = ReadResult{*index};
        break;
    }
    case RequestTag::request_status: {
        auto key = RequestKey::parse(reader.text());
        if (!key)
            fail("INVALID_REQUEST", "malformed request key");
        operation = RequestStatusQuery{*key};
        break;
    }
    case RequestTag::workspace: {
        auto blob = reader.text();
        Reader workspace(blob);
        operation = decode_workspace(workspace);
        break;
    }
    default:
        fail("UNKNOWN_OPERATION", "unknown request tag");
    }
    reader.require_end();
    return SessionRequest::create(std::move(operation));
}

std::string encode(const SessionReply &reply) {
    Writer writer;
    std::visit(
        [&](const auto &value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, Accepted>) {
                writer.u8(static_cast<std::uint8_t>(ReplyTag::accepted));
                writer.u8(static_cast<std::uint8_t>(value.kind));
            } else if constexpr (std::is_same_v<T, OutputChunk>) {
                writer.u8(static_cast<std::uint8_t>(ReplyTag::output));
                writer.varint(value.offset);
                writer.varint(value.next_offset);
                writer.boolean(value.finished);
                writer.text(value.bytes);
            } else if constexpr (std::is_same_v<T, CommandStatus>) {
                writer.u8(static_cast<std::uint8_t>(ReplyTag::command_status));
                writer.varint(value.command.number());
                writer.boolean(value.done);
                writer.optional(value.outcome, [&](const auto &outcome) {
                    Writer nested;
                    encode_outcome(nested, outcome);
                    writer.text(nested.data);
                });
            } else if constexpr (std::is_same_v<T, SessionStatus>) {
                writer.u8(static_cast<std::uint8_t>(ReplyTag::session_status));
                writer.u8(static_cast<std::uint8_t>(value.state));
                writer.optional(value.current, [&](const auto &index) { writer.varint(index.number()); });
                writer.varint(value.completed_commands);
            } else if constexpr (std::is_same_v<T, ResultReply>) {
                writer.u8(static_cast<std::uint8_t>(ReplyTag::result));
                writer.text(encode(value.command).dump());
            } else if constexpr (std::is_same_v<T, RequestStatusReply>) {
                writer.u8(static_cast<std::uint8_t>(ReplyTag::request_status));
                writer.text(value.key.str());
                writer.optional(value.command, [&](const auto &index) { writer.varint(index.number()); });
            } else if constexpr (std::is_same_v<T, ManifestReply>) {
                writer.u8(static_cast<std::uint8_t>(ReplyTag::manifest));
                writer.varint(value.entries.size());
                for (const auto &entry : value.entries) {
                    writer.text(entry.path);
                    writer.varint(entry.size);
                    writer.boolean(entry.executable);
                }
            } else {
                writer.u8(static_cast<std::uint8_t>(ReplyTag::error));
                writer.text(value.code);
                writer.text(value.message);
                writer.boolean(value.retryable);
            }
        },
        reply);
    return frame(Kind::reply, writer.data[0], writer.data.substr(1));
}

SessionReply decode_reply(std::string_view packet) {
    auto parsed = unframe(packet);
    if (parsed.kind != Kind::reply)
        fail("BAD_WIRE_PACKET", "expected a reply packet");
    Reader reader(parsed.payload);
    SessionReply reply = ErrorReply{"BAD_WIRE_PACKET", "empty reply", false};
    switch (static_cast<ReplyTag>(parsed.tag)) {
    case ReplyTag::accepted: {
        auto kind = reader.u8();
        if (kind > 2)
            fail("BAD_WIRE_PACKET", "unknown accepted kind");
        reply = Accepted{static_cast<Accepted::Kind>(kind)};
        break;
    }
    case ReplyTag::output: {
        auto offset = reader.varint();
        auto next_offset = reader.varint();
        auto finished = reader.boolean();
        auto bytes = reader.text();
        reply = OutputChunk{offset, next_offset, std::move(bytes), finished};
        break;
    }
    case ReplyTag::command_status: {
        auto index = CommandIndex::from_number(static_cast<std::uint32_t>(reader.varint()));
        if (!index)
            fail("BAD_WIRE_PACKET", "invalid command index");
        auto done = reader.boolean();
        std::optional<ExecutionOutcome> outcome;
        if (reader.boolean()) {
            auto blob = reader.text();
            Reader nested(blob);
            outcome = decode_outcome(nested);
            nested.require_end();
        }
        reply = CommandStatus{*index, done, outcome};
        break;
    }
    case ReplyTag::session_status: {
        auto state = reader.u8();
        if (state > 7)
            fail("BAD_WIRE_PACKET", "unknown session state");
        std::optional<CommandIndex> current;
        if (reader.boolean()) {
            auto index = CommandIndex::from_number(static_cast<std::uint32_t>(reader.varint()));
            if (!index)
                fail("BAD_WIRE_PACKET", "invalid command index");
            current = *index;
        }
        reply = SessionStatus{static_cast<SessionState>(state), current,
                              static_cast<std::uint32_t>(reader.varint())};
        break;
    }
    case ReplyTag::result: {
        std::string error;
        std::optional<FinishedCommand> record;
        try {
            record = decode_finished(Json::parse(reader.text()), error);
        } catch (const std::exception &failure) {
            fail("BAD_WIRE_PACKET", std::string("unparsable result record: ") + failure.what());
        }
        if (!record)
            fail("BAD_WIRE_PACKET", "invalid result record: " + error);
        reply = ResultReply{std::move(*record)};
        break;
    }
    case ReplyTag::request_status: {
        auto key = RequestKey::parse(reader.text());
        if (!key)
            fail("BAD_WIRE_PACKET", "malformed request key");
        std::optional<CommandIndex> command;
        if (reader.boolean()) {
            auto index = CommandIndex::from_number(static_cast<std::uint32_t>(reader.varint()));
            if (!index)
                fail("BAD_WIRE_PACKET", "invalid command index");
            command = *index;
        }
        reply = RequestStatusReply{*key, command};
        break;
    }
    case ReplyTag::manifest: {
        ManifestReply manifest;
        auto count = reader.varint();
        if (count > 65536)
            fail("BAD_WIRE_PACKET", "manifest too large");
        for (std::uint64_t i = 0; i < count; ++i) {
            auto path = reader.text();
            auto size = reader.varint();
            manifest.entries.push_back(FileEntry{std::move(path), size, reader.boolean()});
        }
        reply = std::move(manifest);
        break;
    }
    case ReplyTag::error:
        reply = ErrorReply{reader.text(), reader.text(), reader.boolean()};
        break;
    default:
        fail("BAD_WIRE_PACKET", "unknown reply tag");
    }
    reader.require_end();
    return reply;
}

} // namespace sandbox
