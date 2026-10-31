// Portable tests for the redesigned core: identifiers, request validation,
// outcome/result separation and the storage record boundary.
#include "sandbox/core/ids.hpp"
#include "sandbox/core/record_json.hpp"
#include "sandbox/core/request.hpp"
#include "sandbox/core/result.hpp"

#include <cstdio>
#include <cstdlib>
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

template <class F> void check_throws(F &&body, const char *what, const char *code) {
    try {
        body();
        std::printf("FAIL: %s (no throw)\n", what);
        ++failures;
    } catch (const RequestError &error) {
        if (error.code != code) {
            std::printf("FAIL: %s (code %s)\n", what, error.code.c_str());
            ++failures;
        }
    }
}

void test_ids() {
    const std::string good = "run-0123456789abcdef01234567";
    check(ExecutionId::parse(good).has_value(), "valid execution id");
    check(!ExecutionId::parse("run-0123456789abcdef0123456").has_value(), "short execution id");
    check(!ExecutionId::parse("run-0123456789ABCDEF01234567").has_value(), "uppercase hex rejected");
    check(!ExecutionId::parse("cmd-0123456789abcdef01234567").has_value(), "wrong prefix rejected");
    check(!ExecutionId::parse("").has_value(), "empty id rejected");

    const unsigned char random[12] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55,
                                      0x66, 0x77, 0x88, 0x99, 0xaa, 0xff};
    auto minted = ExecutionId::mint(random);
    check(minted.str() == "run-00112233445566778899aaff", "mint hex encoding");
    check(ExecutionId::parse(minted.str()) == minted, "minted id round-trips");

    check(RequestKey::parse("abc-123_XYZ").has_value(), "valid request key");
    check(!RequestKey::parse("").has_value(), "empty request key");
    check(!RequestKey::parse(std::string(65, 'a')).has_value(), "over-long request key");
    check(!RequestKey::parse("has space").has_value(), "space rejected in request key");

    check(CommandIndex::parse("cmd-1")->number() == 1, "first command index");
    check(!CommandIndex::parse("cmd-0").has_value(), "zero command index rejected");
    check(!CommandIndex::parse("cmd-01").has_value(), "leading zero rejected");
    check(!CommandIndex::parse("cmd-").has_value(), "empty digits rejected");
    check(!CommandIndex::parse("cmd-1x").has_value(), "trailing junk rejected");
    check(CommandIndex::parse("cmd-1024")->str() == "cmd-1024", "index formatting");
}

void test_request_validation() {
    check_throws([] { ExecutionRequest::create({}); }, "empty argv", "INVALID_REQUEST");
    check_throws([] { ExecutionRequest::create({"/bin/true", std::string("a\0b", 3)}); },
                 "NUL in argv", "INVALID_REQUEST");
    check_throws([] { ExecutionRequest::create({"bin/true"}); }, "relative argv[0]",
                 "INVALID_REQUEST");
    check_throws(
        [] {
            ExecutionRequest::create({"/bin/true"}, std::filesystem::path("relative"));
        },
        "relative cwd", "INVALID_REQUEST");
    check_throws([] { ExecutionRequest::create({"/bin/true"}, std::nullopt, std::nullopt,
                                               std::chrono::milliseconds(0)); },
                 "zero timeout", "INVALID_REQUEST");
    check_throws([] { ExecutionRequest::create({"/bin/true"}, std::nullopt, std::nullopt,
                                               ExecutionRequest::max_timeout + std::chrono::milliseconds(1)); },
                 "oversized timeout", "INVALID_REQUEST");
    check_throws(
        [] {
            ExecutionRequest::create({"/bin/true"}, std::nullopt,
                                     std::map<std::string, std::string>{{"A=B", "1"}});
        },
        "name containing '='", "INVALID_REQUEST");

    auto base = ExecutionRequest::create({"/bin/echo", "hi"});
    check(base.same_intent(ExecutionRequest::create({"/bin/echo", "hi"})), "identical intent");
    check(!base.same_intent(ExecutionRequest::create({"/bin/echo", "hi!"})), "argv differs");

    auto with_cwd = ExecutionRequest::create({"/bin/echo", "hi"}, std::filesystem::path("/work"));
    check(!base.same_intent(with_cwd), "cwd presence is part of the intent");
    auto empty_env = ExecutionRequest::create({"/bin/echo", "hi"}, std::nullopt,
                                              std::map<std::string, std::string>{});
    check(!base.same_intent(empty_env), "explicit empty env differs from inherited env");
    check(empty_env.same_intent(ExecutionRequest::create({"/bin/echo", "hi"}, std::nullopt,
                                                         std::map<std::string, std::string>{})),
          "two explicit empty envs match");
}

void test_outcome_mapping() {
    check(outcome_exit_code(ExecutionOutcome(Exited{0})) == 0, "success maps to 0");
    check(outcome_exit_code(ExecutionOutcome(Exited{3})) == 10, "non-zero exit maps to 10");
    check(outcome_exit_code(ExecutionOutcome(Signaled{9})) == 11, "signal maps to 11");
    check(outcome_exit_code(ExecutionOutcome(TimedOut{})) == 124, "timeout maps to 124");
    check(outcome_exit_code(ExecutionOutcome(Cancelled{})) == 130, "cancel maps to 130");
    check(outcome_exit_code(ExecutionOutcome(Oom{})) == 13, "oom maps to 13");
    check(outcome_exit_code(ExecutionOutcome(StartupFailed{"x"})) == 12, "startup maps to 12");
    check(outcome_exit_code(ExecutionOutcome(SupervisionFailed{"x"})) == 125,
          "supervision maps to 125");
    check(outcome_exit_code(ExecutionOutcome(Indeterminate{"x"})) == 125,
          "indeterminate maps to 125");

    // A signal must never be turned into an exit code.
    check(!ExecutionOutcome(Signaled{9}).exit_code().has_value(), "signal has no exit code");
    check(ExecutionOutcome(Signaled{9}).term_signal() == 9, "signal is reported as signal");
    check(!ExecutionOutcome(Exited{0}).term_signal().has_value(), "exit has no signal");
    check(ExecutionOutcome(Indeterminate{"lost"}).reason() != nullptr, "indeterminate carries a reason");
}

ExecutionResult make_result(ExecutionOutcome outcome, bool clean) {
    ExecutionResult result;
    result.outcome = std::move(outcome);
    result.out.eof = true;
    result.out.flushed = true;
    result.cleanup.descendants_gone = true;
    result.cleanup.launcher_reaped = true;
    result.cleanup.init_reaped = true;
    result.cleanup.cgroup_removed = true;
    result.cleanup.workspace_removed = true;
    if (!clean)
        result.cleanup.failures.push_back({CleanupStage::workspace, "busy"});
    return result;
}

void test_result_separation() {
    auto ok = make_result(ExecutionOutcome(Exited{0}), true);
    check(verdict_of(ok) == Verdict::ok, "clean success is ok");

    // The case the old report could not express: target fine, cleanup broken.
    auto dirty = make_result(ExecutionOutcome(Exited{0}), false);
    check(dirty.target_succeeded(), "exit 0 stays a target success");
    check(dirty.cleanup_failed(), "cleanup failure is reported separately");
    check(verdict_of(dirty) == Verdict::cleanup_incomplete, "dirty cleanup gets its own verdict");
    check(outcome_exit_code(dirty.outcome) == 0, "target exit code is not rewritten by cleanup");

    auto failed = make_result(ExecutionOutcome(Exited{7}), true);
    check(verdict_of(failed) == Verdict::target_failed, "non-zero exit is a target failure");

    auto unknown = make_result(ExecutionOutcome(Indeterminate{"reaped by supervisor"}), true);
    check(verdict_of(unknown) == Verdict::harness_error, "indeterminate is a harness error");

    OutputResult partial;
    partial.eof = true;
    partial.flushed = false;
    check(!partial.complete(), "unflushed output is incomplete");
    partial.flushed = true;
    partial.failed_bytes = 12;
    check(!partial.complete(), "failed bytes make output incomplete");
    partial.failed_bytes = 0;
    partial.discarded_bytes = 5;
    check(partial.complete() && partial.truncated(), "discard is intentional truncation, not a failure");
}

void test_record_round_trip() {
    auto request = ExecutionRequest::create(
        {"/bin/sh", "-c", "echo hi"}, std::filesystem::path("/work"),
        std::map<std::string, std::string>{{"LANG", "C.UTF-8"}},
        std::chrono::milliseconds(1500), RequestKey::parse("req-1"));
    AcceptedCommand accepted{*CommandIndex::from_number(3), request,
                             std::chrono::system_clock::time_point(std::chrono::milliseconds(1700000000123))};
    auto encoded = encode(accepted);
    std::string error;
    auto decoded = decode_accepted(encoded, error);
    check(decoded.has_value(), "accepted record decodes");
    if (decoded) {
        check(encoded == encode(*decoded), "accepted record round-trips byte-identically");
        check(decoded->request.same_intent(request), "decoded request keeps its intent");
        check(decoded->index == accepted.index, "decoded index matches");
    }

    FinishedCommand finished{*CommandIndex::from_number(3), make_result(ExecutionOutcome(Exited{0}), false),
                             std::chrono::system_clock::time_point(std::chrono::milliseconds(1700000009999))};
    finished.result.timing.startup = std::chrono::microseconds(1200);
    finished.result.timing.execution = std::chrono::microseconds(3400);
    finished.result.out.saved_bytes = 42;
    auto encoded_finished = encode(finished);
    auto decoded_finished = decode_finished(encoded_finished, error);
    check(decoded_finished.has_value(), "finished record decodes");
    if (decoded_finished) {
        check(encoded_finished == encode(*decoded_finished), "finished record round-trips");
        check(decoded_finished->result.out.saved_bytes == 42, "output bytes survive");
        check(decoded_finished->result.cleanup.failures.size() == 1, "cleanup failures survive");
        check(!decoded_finished->result.cleanup.complete(), "cleanup stays incomplete");
    }
}

void test_corrupt_records() {
    std::string error;
    Json bad_version = Json{{"schema_version", 99}};
    check(!decode_accepted(bad_version, error).has_value(), "unknown schema version rejected");
    check(!error.empty(), "error text is filled in");

    Json missing_index = Json{{"schema_version", record_schema_version},
                              {"accepted_at_ms", 0},
                              {"request", Json{{"argv", Json::array({"/bin/true"})}}}};
    check(!decode_accepted(missing_index, error).has_value(), "missing index rejected");

    Json bad_index = missing_index;
    bad_index["index"] = "command-1";
    check(!decode_accepted(bad_index, error).has_value(), "malformed index rejected");

    Json bad_argv = missing_index;
    bad_argv["index"] = "cmd-1";
    bad_argv["request"]["argv"] = Json::array({"relative"});
    check(!decode_accepted(bad_argv, error).has_value(), "invalid request payload rejected");

    Json bad_outcome = Json{{"schema_version", record_schema_version},
                            {"index", "cmd-1"},
                            {"finished_at_ms", 0},
                            {"result", Json{{"outcome", Json{{"kind", "exploded"}}}}}};
    check(!decode_finished(bad_outcome, error).has_value(), "unknown outcome kind rejected");
}
} // namespace

int main() {
    test_ids();
    test_request_validation();
    test_outcome_mapping();
    test_result_separation();
    test_record_round_trip();
    test_corrupt_records();
    if (failures != 0) {
        std::printf("%d core check(s) failed\n", failures);
        return 1;
    }
    std::printf("core model, result separation and record boundary passed\n");
    return 0;
}
