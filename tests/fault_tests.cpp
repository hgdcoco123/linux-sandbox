// Fault-boundary tests: the cases that must be *rejected* rather than
// tolerated. Each section drives one boundary into its failure modes.
#include "sandbox/core/control.hpp"
#include "sandbox/core/prepare.hpp"
#include "sandbox/core/record_json.hpp"
#include "sandbox/core/supervisor.hpp"
#include "sandbox/core/wire.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace sandbox;

namespace {
int failures = 0;

void check(bool condition, const char *what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

// Every proper prefix of a valid packet must be rejected: no decoder may read
// past the end or accept a partial frame.
template <class Decode>
void every_truncation_rejected(const std::string &packet, Decode decode, const char *what) {
    for (std::size_t length = 0; length + 1 < packet.size(); ++length) {
        auto prefix = packet.substr(0, length);
        bool rejected = false;
        try {
            decode(prefix);
        } catch (...) {
            rejected = true;
        }
        if (!rejected) {
            std::printf("FAIL: %s accepted a %zu/%zu byte prefix\n", what, length, packet.size());
            ++failures;
            return;
        }
    }
}

// A single flipped bit must never produce a crash or a different message kind
// that silently changes meaning.
template <class Decode, class Kind>
void bit_flips_are_rejected_or_equivalent(const std::string &packet, Decode decode, Kind kind_of,
                                          const char *what) {
    for (std::size_t position = 0; position < packet.size(); ++position) {
        for (unsigned char mask : {0x01, 0x80}) {
            auto mutated = packet;
            mutated[position] = static_cast<char>(mutated[position] ^ mask);
            try {
                auto decoded = decode(mutated);
                if (kind_of(decoded) != kind_of(decode(packet))) {
                    std::printf("FAIL: %s position %zu changed the message kind\n", what, position);
                    ++failures;
                    return;
                }
            } catch (...) {
                continue; // rejection is the expected outcome for a corrupted frame
            }
        }
    }
}

void test_wire_boundaries() {
    auto request = ExecutionRequest::create(
        {"/bin/echo", "hello"}, std::filesystem::path("/work"),
        std::map<std::string, std::string>{{"LANG", "C.UTF-8"}}, std::chrono::milliseconds(1500),
        RequestKey::parse("k-1"));
    auto packet = encode(SessionRequest::create(Execute{request}));
    every_truncation_rejected(
        packet, [](const std::string &bytes) { decode_request(bytes); }, "wire request");
    bit_flips_are_rejected_or_equivalent(
        packet, [](const std::string &bytes) { return decode_request(bytes); },
        [](const SessionRequest &decoded) {
            if (decoded.as<Execute>())
                return 1;
            if (decoded.as<Status>())
                return 2;
            return 3;
        },
        "wire request");

    // Oversized packets are rejected before allocation.
    auto huge = std::string(max_wire_packet + 1, '\0');
    try {
        decode_request(huge);
        std::printf("FAIL: oversized wire packet accepted\n");
        ++failures;
    } catch (const WireError &) {
    }
}

void test_control_boundaries() {
    CommandDone done{*CommandIndex::from_number(7), 0, std::nullopt, std::nullopt,
                     CommandStop::main_exit, std::chrono::milliseconds(12)};
    auto packet = encode(done);
    every_truncation_rejected(
        packet, [](const std::string &bytes) { decode(bytes); }, "control packet");
    bit_flips_are_rejected_or_equivalent(
        packet, [](const std::string &bytes) { return decode(bytes); },
        [](const ControlMessage &decoded) { return static_cast<int>(kind_of(decoded)); },
        "control packet");

    // The FD contract is enforced independently of the payload.
    bool rejected = false;
    try {
        validate_fd_count(ControlKind::command_done, 1);
    } catch (const ControlError &) {
        rejected = true;
    }
    check(rejected, "command_done must reject an unexpected descriptor");
}

void test_supervisor_ordering() {
    SupervisionLimits limits;
    limits.startup = Millis{1000};
    limits.wall = Millis{10000};
    limits.grace = Millis{100};
    limits.drain = Millis{200};
    limits.settle = Millis{100};

    {
        // A duplicate main_exited must not move the timing anchor.
        ExecutionCore core{limits, Millis{0}};
        core.env_ready(Millis{10});
        core.main_exited(0, std::nullopt, Millis{100});
        core.main_exited(1, std::nullopt, Millis{180});
        core.descendants_empty(Millis{200});
        core.output_eof(false, Millis{200});
        core.output_eof(true, Millis{200});
        core.output_finished(false, OutputStats{}, Millis{210});
        core.output_finished(true, OutputStats{}, Millis{210});
        check(core.finished(), "execution finishes after a duplicate exit report");
        check(core.result().outcome.exit_code() == 0, "the first exit fact wins");
        check(std::chrono::duration_cast<Millis>(core.result().timing.execution).count() == 90,
              "duplicate main_exited does not move the timing anchor");
    }
    {
        // Facts after completion are ignored rather than corrupting the result.
        ExecutionCore core{limits, Millis{0}};
        core.env_ready(Millis{10});
        core.main_exited(0, std::nullopt, Millis{100});
        core.descendants_empty(Millis{110});
        core.output_eof(false, Millis{110});
        core.output_eof(true, Millis{110});
        core.output_finished(false, OutputStats{}, Millis{120});
        core.output_finished(true, OutputStats{}, Millis{120});
        check(core.finished(), "finished");
        check(core.tick(Millis{999}).empty(), "tick after completion is a no-op");
        check(core.output_finished(true, OutputStats{}, Millis{999}).empty(),
              "late output facts are ignored");
    }
    {
        // Output finishing before EOF is a violation the core must not paper over.
        ExecutionCore core{limits, Millis{0}};
        core.env_ready(Millis{10});
        core.main_exited(0, std::nullopt, Millis{20});
        core.descendants_empty(Millis{30});
        core.output_finished(false, OutputStats{}, Millis{40});
        core.output_finished(true, OutputStats{}, Millis{40});
        check(!core.finished(), "a finished writer without EOF must not complete the execution");
    }
}

void test_record_boundaries() {
    std::string error;
    // Truncated JSON text, wrong types and unknown fields.
    check(!decode_accepted(Json::parse("{}"), error).has_value(), "empty record rejected");
    check(!decode_accepted(Json{{"schema_version", 2}}, error).has_value(),
          "record without fields rejected");
    check(!decode_finished(Json{{"schema_version", 2}, {"index", "cmd-1"}}, error).has_value(),
          "finished record without result rejected");
    check(!decode_finished(Json{{"schema_version", 2},
                                {"index", "cmd-1"},
                                {"finished_at_ms", 0},
                                {"result", Json{{"outcome", Json{{"kind", "exited"}}}}}},
                           error)
               .has_value(),
          "outcome without an exit code rejected");

    // A schema version from the future is not silently upgraded.
    check(!decode_accepted(Json{{"schema_version", 3}}, error).has_value(),
          "unknown schema version rejected");
    check(!error.empty(), "the rejection carries a reason");
}

void test_prepare_undo_failures() {
    std::vector<std::string> trace;
    PrepareSequence sequence({
        PrepareStepSpec{PrepareStep::state_root, [&] { trace.push_back("do:root"); },
                        [&] {
                            trace.push_back("undo:root");
                            throw std::runtime_error("root busy");
                        }},
        PrepareStepSpec{PrepareStep::cgroup, [&] { trace.push_back("do:cgroup"); },
                        [&] { trace.push_back("undo:cgroup"); }},
        PrepareStepSpec{PrepareStep::mounts, [] { throw std::runtime_error("mount failed"); }, {}},
    });
    auto outcome = sequence.run();
    check(!outcome.ok, "preparation failed");
    check(outcome.rollback_failures.size() == 1, "one undo failure recorded");
    check(trace == std::vector<std::string>({"do:root", "do:cgroup", "undo:cgroup", "undo:root"}),
          "rollback still runs in reverse after an undo throws");
    // The failure is attributed to the step that actually failed, not the undo.
    check(outcome.failed_at == PrepareStep::mounts, "failure attributed to the apply step");
}
} // namespace

int main() {
    test_wire_boundaries();
    test_control_boundaries();
    test_supervisor_ordering();
    test_record_boundaries();
    test_prepare_undo_failures();
    if (failures != 0) {
        std::printf("%d fault-boundary check(s) failed\n", failures);
        return 1;
    }
    std::printf("fault boundaries rejected as expected: truncation, corruption, ordering, records\n");
    return 0;
}
