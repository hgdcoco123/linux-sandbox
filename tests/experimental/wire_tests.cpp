// Portable tests for the typed session vocabulary and its binary codec.
#include "sandbox/core/ops.hpp"
#include "sandbox/core/wire.hpp"

#include <chrono>
#include <cstdio>
#include <string>

using namespace sandbox;

namespace {
int failures = 0;

void check(bool condition, const char *what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

void check_request_round_trip(const SessionRequest &request, const char *what) {
    try {
        auto bytes = encode(request);
        auto decoded = decode_request(bytes);
        if (encode(decoded) != bytes) {
            std::printf("FAIL: %s (bytes differ after round-trip)\n", what);
            ++failures;
        }
    } catch (const std::exception &error) {
        std::printf("FAIL: %s (%s)\n", what, error.what());
        ++failures;
    }
}

void check_reply_round_trip(const SessionReply &reply, const char *what) {
    try {
        auto bytes = encode(reply);
        auto decoded = decode_reply(bytes);
        if (encode(decoded) != bytes) {
            std::printf("FAIL: %s (bytes differ after round-trip)\n", what);
            ++failures;
        }
    } catch (const std::exception &error) {
        std::printf("FAIL: %s (%s)\n", what, error.what());
        ++failures;
    }
}

template <class F> void check_wire_rejects(F &&body, const char *what) {
    try {
        body();
        std::printf("FAIL: %s (accepted)\n", what);
        ++failures;
    } catch (const WireError &) {
    } catch (const std::exception &error) {
        std::printf("FAIL: %s (wrong exception: %s)\n", what, error.what());
        ++failures;
    }
}

void test_request_round_trip() {
    auto minimal = ExecutionRequest::create({"/bin/echo", "hi"});
    check_request_round_trip(SessionRequest::create(Execute{minimal}), "execute minimal");

    auto rich = ExecutionRequest::create(
        {"/bin/sh", "-c", "echo hi"}, std::filesystem::path("/work"),
        std::map<std::string, std::string>{{"LANG", "C.UTF-8"}, {"PATH", "/bin"}},
        std::chrono::milliseconds(2500), RequestKey::parse("req-42"));
    check_request_round_trip(SessionRequest::create(Execute{rich}), "execute all fields");

    // Explicitly empty environment must survive the round-trip as "explicit".
    auto empty_env = ExecutionRequest::create(
        {"/bin/true"}, std::nullopt, std::map<std::string, std::string>{});
    check_request_round_trip(SessionRequest::create(Execute{empty_env}), "execute empty env");

    check_request_round_trip(SessionRequest::create(Status{}), "status");
    check_request_round_trip(SessionRequest::create(Close{}), "close");
    check_request_round_trip(SessionRequest::create(Cancel{std::nullopt}), "cancel current");
    check_request_round_trip(
        SessionRequest::create(Cancel{*CommandIndex::from_number(7)}), "cancel specific");
    check_request_round_trip(
        SessionRequest::create(StdinWrite{*CommandIndex::from_number(1), "hello"}), "stdin write");
    check_request_round_trip(
        SessionRequest::create(StdinClose{*CommandIndex::from_number(1)}), "stdin close");
    check_request_round_trip(SessionRequest::create(ReadOutput{*CommandIndex::from_number(2),
                                                               Stream::standard_error, 4096}),
                             "read stderr at offset");
    check_request_round_trip(
        SessionRequest::create(ReadResult{*CommandIndex::from_number(3)}), "read result");
    check_request_round_trip(
        SessionRequest::create(RequestStatusQuery{*RequestKey::parse("abc")}), "request status");
    check_request_round_trip(
        SessionRequest::create(Workspace{WorkspaceSnapshot{"/tmp/src", "snap"}}), "workspace snapshot");
    check_request_round_trip(SessionRequest::create(Workspace{WorkspaceManifest{}}), "workspace manifest");
    check_request_round_trip(
        SessionRequest::create(Workspace{WorkspaceExport{"/tmp/out"}}), "workspace export");
}

void test_reply_round_trip() {
    check_reply_round_trip(Accepted{Accepted::Kind::cancel_requested}, "accepted cancel");
    check_reply_round_trip(Accepted{Accepted::Kind::stdin_closed}, "accepted stdin close");
    check_reply_round_trip(OutputChunk{0, 5, "hello", false}, "output chunk");
    check_reply_round_trip(OutputChunk{5, 5, "", true}, "output eof");

    check_reply_round_trip(CommandStatus{*CommandIndex::from_number(1), false, std::nullopt},
                           "command running");
    check_reply_round_trip(
        CommandStatus{*CommandIndex::from_number(1), true, ExecutionOutcome(Exited{3})},
        "command exited 3");
    check_reply_round_trip(
        CommandStatus{*CommandIndex::from_number(1), true, ExecutionOutcome(Signaled{15})},
        "command signaled");
    check_reply_round_trip(
        CommandStatus{*CommandIndex::from_number(1), true, ExecutionOutcome(Indeterminate{"lost"})},
        "command indeterminate");

    check_reply_round_trip(SessionStatus{SessionPhase::idle, std::nullopt, 4}, "session idle");
    check_reply_round_trip(
        SessionStatus{SessionPhase::running, *CommandIndex::from_number(5), 4}, "session running");

    ManifestReply manifest;
    manifest.entries.push_back({"a/b.txt", 12, false});
    manifest.entries.push_back({"bin/tool", 4096, true});
    check_reply_round_trip(manifest, "manifest");

    check_reply_round_trip(RequestStatusReply{*RequestKey::parse("k1"),
                                              *CommandIndex::from_number(2)},
                           "request status with command");
    check_reply_round_trip(RequestStatusReply{*RequestKey::parse("k1"), std::nullopt},
                           "request status without command");
    check_reply_round_trip(ErrorReply{"COMMAND_NOT_ACTIVE", "gone", true}, "error reply");

    ExecutionResult result;
    result.outcome = ExecutionOutcome(Exited{0});
    result.out.eof = true;
    result.out.flushed = true;
    result.out.saved_bytes = 99;
    result.cleanup.descendants_gone = true;
    result.cleanup.failures.push_back({CleanupStage::workspace, "busy"});
    result.timing.execution = std::chrono::microseconds(1500);
    FinishedCommand finished{*CommandIndex::from_number(2), result,
                             std::chrono::system_clock::time_point(std::chrono::milliseconds(1000))};
    check_reply_round_trip(ResultReply{finished}, "result reply");
}

void test_rejections() {
    check_wire_rejects([] { decode_request("nope"); }, "short packet");
    check_wire_rejects([] { decode_request("SBX2\x00\x02"); }, "missing payload length");
    check_wire_rejects([] {
        std::string packet = "SBX2";
        packet.push_back('\x00');
        packet.push_back('\x02'); // status
        packet.push_back('\x05'); // payload length 5
        decode_request(packet);   // payload is empty: must be rejected
    }, "truncated payload");
    check_wire_rejects([] {
        std::string packet = "SBX2";
        packet.push_back('\x00');
        packet.push_back('\x7f'); // unknown tag
        packet.push_back('\x00');
        decode_request(packet);
    }, "unknown tag");
    check_wire_rejects([] {
        auto bytes = encode(SessionRequest::create(Status{}));
        bytes.push_back('\x00'); // trailing byte
        decode_request(bytes);
    }, "trailing bytes");
    check_wire_rejects([] {
        auto bytes = encode(SessionRequest::create(Status{}));
        bytes[4] = '\x01'; // flip kind to reply
        decode_request(bytes);
    }, "wrong packet kind");
    check_wire_rejects([] {
        std::string packet = "SBX2";
        packet.push_back('\x00');
        packet.push_back('\x02');
        packet.append("\xff\xff\x7f"); // huge varint length
        decode_request(packet);
    }, "payload length overflow");
    check_wire_rejects([] {
        auto packet = encode(SessionRequest::create(Status{}));
        packet.resize(6);
        packet.append(9, static_cast<char>(0x80));
        packet.push_back(static_cast<char>(0x02));
        decode_request(packet);
    }, "varint exceeds uint64");

    try {
        SessionRequest::create(StdinWrite{*CommandIndex::from_number(1), std::string(4097, 'x')});
        std::printf("FAIL: oversized stdin chunk accepted\n");
        ++failures;
    } catch (const RequestError &) {
    }
    try {
        SessionRequest::create(Workspace{WorkspaceSnapshot{"relative", "n"}});
        std::printf("FAIL: relative snapshot source accepted\n");
        ++failures;
    } catch (const RequestError &) {
    }
    try {
        SessionRequest::create(Workspace{WorkspaceExport{"relative"}});
        std::printf("FAIL: relative export destination accepted\n");
        ++failures;
    } catch (const RequestError &) {
    }
    try {
        SessionRequest::create(Workspace{WorkspaceSnapshot{"/tmp/x", ""}});
        std::printf("FAIL: empty snapshot name accepted\n");
        ++failures;
    } catch (const RequestError &) {
    }
}

void test_decoded_values() {
    auto request = ExecutionRequest::create(
        {"/bin/echo", "hi"}, std::filesystem::path("/work"),
        std::map<std::string, std::string>{{"A", "1"}}, std::chrono::milliseconds(900),
        RequestKey::parse("k-9"));
    auto packet = encode(SessionRequest::create(Execute{request}));
    auto decoded = decode_request(packet);
    const auto *execute = decoded.as<Execute>();
    check(execute != nullptr, "decoded as execute");
    if (execute) {
        check(execute->request.same_intent(request), "decoded request keeps intent");
        check(execute->request.argv().size() == 2, "argv preserved");
        check(execute->request.timeout() == std::chrono::milliseconds(900), "timeout preserved");
        check(execute->request.key() && execute->request.key()->str() == "k-9", "request key preserved");
        check(execute->request.env() && execute->request.env()->at("A") == "1", "env preserved");
    }

    auto status_packet = encode(SessionRequest::create(ReadOutput{
        *CommandIndex::from_number(9), Stream::standard_error, 123456789}));
    auto decoded_output = decode_request(status_packet);
    const auto *output = decoded_output.as<ReadOutput>();
    check(output != nullptr, "decoded as read output");
    if (output) {
        check(output->offset == 123456789, "64-bit offset preserved");
        check(output->stream == Stream::standard_error, "stream preserved");
        check(output->command.number() == 9, "command index preserved");
    }

    // The largest legal stdin chunk must survive; one byte more is a request error.
    auto chunk = std::string(max_stdin_chunk, 'z');
    auto chunk_packet =
        encode(SessionRequest::create(StdinWrite{*CommandIndex::from_number(1), chunk}));
    auto decoded_chunk = decode_request(chunk_packet);
    const auto *write = decoded_chunk.as<StdinWrite>();
    check(write != nullptr && write->bytes.size() == max_stdin_chunk, "max stdin chunk survives");
}
} // namespace

int main() {
    test_request_round_trip();
    test_reply_round_trip();
    test_rejections();
    test_decoded_values();
    if (failures != 0) {
        std::printf("%d wire check(s) failed\n", failures);
        return 1;
    }
    std::printf("typed session operations and binary codec passed\n");
    return 0;
}
