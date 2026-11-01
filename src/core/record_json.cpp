#include "sandbox/core/record_json.hpp"

#include <chrono>
#include <exception>

namespace sandbox {
namespace {
using Clock = std::chrono::system_clock;

long long to_ms(Clock::time_point point) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(point.time_since_epoch()).count();
}
Clock::time_point from_ms(long long value) {
    return Clock::time_point(std::chrono::milliseconds(value));
}

// `path::native()` is wide on Windows; records are UTF-8 text.
std::string utf8_of(const std::filesystem::path &path) {
    auto text = path.u8string();
    return std::string(reinterpret_cast<const char *>(text.data()), text.size());
}

Json encode_output(const OutputResult &output) {
    return Json{{"saved_bytes", output.saved_bytes},
                {"discarded_bytes", output.discarded_bytes},
                {"failed_bytes", output.failed_bytes},
                {"queue_peak_bytes", output.queue_peak_bytes},
                {"backpressure_events", output.backpressure_events},
                {"eof", output.eof},
                {"flushed", output.flushed},
                {"write_error", output.write_error}};
}

Json encode_outcome(const ExecutionOutcome &outcome) {
    Json value{{"kind", outcome.name()}};
    if (auto code = outcome.exit_code())
        value["exit_code"] = *code;
    if (auto signal = outcome.term_signal())
        value["signal"] = *signal;
    if (const auto *reason = outcome.reason())
        value["reason"] = *reason;
    if (const auto *cancelled = std::get_if<Cancelled>(&outcome.variant()))
        value["cause"] = cancel_cause_name(cancelled->cause);
    if (const auto *failed = std::get_if<StartupFailed>(&outcome.variant()))
        value["exec_errno"] = failed->exec_errno;
    return value;
}

ExecutionOutcome decode_outcome(const Json &value) {
    const auto kind = value.at("kind").get<std::string>();
    if (kind == "exited")
        return ExecutionOutcome(Exited{value.at("exit_code").get<int>()});
    if (kind == "signaled")
        return ExecutionOutcome(Signaled{value.at("signal").get<int>()});
    if (kind == "timeout")
        return ExecutionOutcome(TimedOut{});
    if (kind == "cancelled") {
        // Absent cause means "requested": that is what every v2 record said.
        auto cause = CancelCause::requested;
        if (value.contains("cause")) {
            const auto name = value.at("cause").get<std::string>();
            if (name == "session_closed")
                cause = CancelCause::session_closed;
            else if (name != "requested")
                throw std::runtime_error("unknown cancel cause: " + name);
        }
        return ExecutionOutcome(Cancelled{cause});
    }
    if (kind == "oom")
        return ExecutionOutcome(Oom{});
    if (kind == "startup_failure")
        return ExecutionOutcome(StartupFailed{value.at("reason").get<std::string>(),
                                              value.value("exec_errno", 0)});
    if (kind == "supervision_failure")
        return ExecutionOutcome(SupervisionFailed{value.at("reason").get<std::string>()});
    if (kind == "indeterminate")
        return ExecutionOutcome(Indeterminate{value.at("reason").get<std::string>()});
    throw std::runtime_error("unknown outcome kind: " + kind);
}

OutputResult decode_output(const Json &value) {
    OutputResult output;
    output.saved_bytes = value.at("saved_bytes").get<std::uint64_t>();
    output.discarded_bytes = value.at("discarded_bytes").get<std::uint64_t>();
    output.failed_bytes = value.at("failed_bytes").get<std::uint64_t>();
    output.queue_peak_bytes = value.at("queue_peak_bytes").get<std::uint64_t>();
    output.backpressure_events = value.at("backpressure_events").get<std::uint64_t>();
    output.eof = value.at("eof").get<bool>();
    output.flushed = value.at("flushed").get<bool>();
    output.write_error = value.at("write_error").get<std::string>();
    return output;
}

CleanupStage stage_from_name(const std::string &name) {
    for (auto stage : {CleanupStage::descendants, CleanupStage::launcher, CleanupStage::init,
                       CleanupStage::cgroup, CleanupStage::mounts, CleanupStage::workspace,
                       CleanupStage::control_socket})
        if (name == cleanup_stage_name(stage))
            return stage;
    throw std::runtime_error("unknown cleanup stage: " + name);
}

Json encode_request_fields(const ExecutionRequest &request) {
    Json value{{"argv", request.argv()}};
    if (request.cwd())
        value["cwd"] = utf8_of(*request.cwd());
    if (request.env()) {
        Json env = Json::object();
        for (const auto &[name, content] : *request.env())
            env[name] = content;
        value["env"] = std::move(env);
    }
    if (request.timeout())
        value["timeout_ms"] = request.timeout()->count();
    if (request.key())
        value["request_key"] = request.key()->str();
    return value;
}

ExecutionRequest decode_request_fields(const Json &value) {
    std::optional<std::filesystem::path> cwd;
    std::optional<std::map<std::string, std::string>> env;
    std::optional<std::chrono::milliseconds> timeout;
    std::optional<RequestKey> key;
    if (value.contains("cwd"))
        cwd = std::filesystem::path(value.at("cwd").get<std::string>());
    if (value.contains("env")) {
        std::map<std::string, std::string> parsed;
        for (const auto &entry : value.at("env").items())
            parsed.emplace(entry.key(), entry.value().get<std::string>());
        env = std::move(parsed);
    }
    if (value.contains("timeout_ms"))
        timeout = std::chrono::milliseconds(value.at("timeout_ms").get<std::int64_t>());
    if (value.contains("request_key")) {
        auto parsed = RequestKey::parse(value.at("request_key").get<std::string>());
        if (!parsed)
            throw std::runtime_error("invalid request_key in record");
        key = std::move(parsed);
    }
    return ExecutionRequest::create(value.at("argv").get<std::vector<std::string>>(), std::move(cwd),
                                    std::move(env), timeout, std::move(key));
}

Json encode_cleanup(const CleanupResult &cleanup) {
    Json failures = Json::array();
    for (const auto &failure : cleanup.failures)
        failures.push_back(
            Json{{"stage", cleanup_stage_name(failure.stage)}, {"detail", failure.detail}});
    return Json{{"descendants_gone", cleanup.descendants_gone},
                {"launcher_reaped", cleanup.launcher_reaped},
                {"init_reaped", cleanup.init_reaped},
                {"cgroup_removed", cleanup.cgroup_removed},
                {"workspace_removed", cleanup.workspace_removed},
                {"failures", std::move(failures)}};
}

CleanupResult decode_cleanup(const Json &value) {
    CleanupResult cleanup;
    cleanup.descendants_gone = value.at("descendants_gone").get<bool>();
    cleanup.launcher_reaped = value.at("launcher_reaped").get<bool>();
    cleanup.init_reaped = value.at("init_reaped").get<bool>();
    cleanup.cgroup_removed = value.at("cgroup_removed").get<bool>();
    cleanup.workspace_removed = value.at("workspace_removed").get<bool>();
    for (const auto &entry : value.at("failures"))
        cleanup.failures.push_back(
            CleanupFailure{stage_from_name(entry.at("stage").get<std::string>()),
                           entry.at("detail").get<std::string>()});
    return cleanup;
}

Json encode_timing(const Timing &timing) {
    return Json{{"startup_us", timing.startup.count()},
                {"execution_us", timing.execution.count()},
                {"total_us", timing.total.count()}};
}

Timing decode_timing(const Json &value) {
    Timing timing;
    timing.startup = std::chrono::microseconds(value.at("startup_us").get<std::int64_t>());
    timing.execution = std::chrono::microseconds(value.at("execution_us").get<std::int64_t>());
    timing.total = std::chrono::microseconds(value.at("total_us").get<std::int64_t>());
    return timing;
}

CommandIndex decode_index(const Json &value) {
    auto index = CommandIndex::parse(value.at("index").get<std::string>());
    if (!index)
        throw std::runtime_error("invalid command index in record");
    return *index;
}
} // namespace

Json encode(const AcceptedCommand &accepted) {
    Json value{{"schema_version", record_schema_version},
               {"index", accepted.index.str()},
               {"accepted_at_ms", to_ms(accepted.accepted_at)},
               {"request", encode_request_fields(accepted.request)}};
    if (accepted.effective_cwd)
        value["effective_cwd"] = utf8_of(*accepted.effective_cwd);
    return value;
}

Json encode(const FinishedCommand &finished) {
    Json result{{"outcome", encode_outcome(finished.result.outcome)},
                {"stdout", encode_output(finished.result.out)},
                {"stderr", encode_output(finished.result.err)},
                {"cleanup", encode_cleanup(finished.result.cleanup)},
                {"timing", encode_timing(finished.result.timing)}};
    // An empty policy name means no filter was installed; writing a version for a
    // policy that does not exist would invent a fact.
    if (!finished.result.security.name.empty())
        result["security"] = {{"policy", finished.result.security.name},
                              {"version", finished.result.security.version}};
    return Json{{"schema_version", record_schema_version},
                {"index", finished.index.str()},
                {"finished_at_ms", to_ms(finished.finished_at)},
                {"result", std::move(result)}};
}

std::optional<AcceptedCommand> decode_accepted(const Json &value, std::string &error) {
    try {
        const int version = value.at("schema_version").get<int>();
        if (version < record_schema_minimum_readable || version > record_schema_version) {
            error = "unsupported record schema_version " + std::to_string(version);
            return std::nullopt;
        }
        AcceptedCommand accepted{decode_index(value), decode_request_fields(value.at("request")),
                                 from_ms(value.at("accepted_at_ms").get<long long>()),
                                 std::nullopt};
        if (value.contains("effective_cwd"))
            accepted.effective_cwd =
                std::filesystem::path(value.at("effective_cwd").get<std::string>());
        return accepted;
    } catch (const std::exception &failure) {
        error = failure.what();
        return std::nullopt;
    }
}

std::optional<FinishedCommand> decode_finished(const Json &value, std::string &error) {
    try {
        const int version = value.at("schema_version").get<int>();
        if (version < record_schema_minimum_readable || version > record_schema_version) {
            error = "unsupported record schema_version " + std::to_string(version);
            return std::nullopt;
        }
        const auto &result = value.at("result");
        FinishedCommand finished{decode_index(value), ExecutionResult{},
                                 from_ms(value.at("finished_at_ms").get<long long>())};
        finished.result.outcome = decode_outcome(result.at("outcome"));
        finished.result.out = decode_output(result.at("stdout"));
        finished.result.err = decode_output(result.at("stderr"));
        finished.result.cleanup = decode_cleanup(result.at("cleanup"));
        finished.result.timing = decode_timing(result.at("timing"));
        // Absent in schema 3 records: no policy was recorded, which is not the same
        // as recording an empty one, but it is all an older record can tell us.
        if (result.contains("security")) {
            const auto &security = result.at("security");
            finished.result.security.name = security.at("policy").get<std::string>();
            finished.result.security.version = security.value("version", 0u);
        }
        return finished;
    } catch (const std::exception &failure) {
        error = failure.what();
        return std::nullopt;
    }
}

} // namespace sandbox
