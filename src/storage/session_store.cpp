#include "sandbox/storage/session_store.hpp"

#include "sandbox/core/record_json.hpp"
#include "sandbox/storage/store.hpp"

#include <chrono>
#include <map>
#include <string>

namespace sandbox {
namespace {
using Clock = std::chrono::system_clock;

// On-disk shapes this build still has to read. Version 2 is what every session
// written before the typed records existed carries; it is read-only support, not
// a second writer.
constexpr int legacy_session_record_version = 2;

OutputResult decode_stream_fields(const Json &value) {
    OutputResult output;
    output.saved_bytes = value.value("saved_bytes", std::uint64_t{0});
    output.discarded_bytes = value.value("discarded_bytes", std::uint64_t{0});
    output.failed_bytes = value.value("failed_bytes", std::uint64_t{0});
    output.queue_peak_bytes = value.value("queue_peak_bytes", std::uint64_t{0});
    output.backpressure_events = value.value("backpressure_events", std::uint64_t{0});
    output.eof = value.value("eof", false);
    output.flushed = value.value("flushed", false);
    output.write_error = value.value("write_error", std::string());
    return output;
}

// Legacy records may carry an explicit null for a fact that was never learned.
// Reading one must not turn "unknown" into a type error.
int int_or_zero(const Json &value, const char *key) {
    if (!value.contains(key) || !value.at(key).is_number())
        return 0;
    return value.at(key).get<int>();
}

ExecutionOutcome outcome_of_legacy(const Json &command) {
    const auto reason = command.value("stop_reason", std::string());
    const auto exit_code = command.contains("exit_code") && command.at("exit_code").is_number()
                               ? std::optional<int>(command.at("exit_code").get<int>())
                               : std::nullopt;
    const auto signal = command.contains("signal") && command.at("signal").is_number()
                            ? std::optional<int>(command.at("signal").get<int>())
                            : std::nullopt;
    if (reason.empty())
        return ExecutionOutcome(Indeterminate{"no terminal result recorded"});
    if (reason == "main_exited")
        return signal && *signal != no_signal ? ExecutionOutcome(Signaled{*signal})
                                             : ExecutionOutcome(Exited{exit_code.value_or(no_exit_code)});
    if (reason == "cancelled")
        return ExecutionOutcome(Cancelled{CancelCause::requested});
    if (reason == "session_closed")
        return ExecutionOutcome(Cancelled{CancelCause::session_closed});
    if (reason == "timeout")
        return ExecutionOutcome(TimedOut{});
    if (reason == "startup_failure")
        return ExecutionOutcome(StartupFailed{"exec failed", int_or_zero(command, "exec_errno")});
    if (reason == "host_error")
        return ExecutionOutcome(SupervisionFailed{"command supervision failed"});
    throw std::runtime_error("unknown stop reason in record: " + reason);
}

// A record is compared without its timestamps: re-finishing the same command with
// the same facts is idempotent, re-finishing it with different ones is refused.
Json comparable(const FinishedCommand &finished) {
    auto value = encode(finished);
    value.erase("finished_at_ms");
    return value;
}

std::optional<AcceptedCommand> decode_accepted_record(const Json &value) {
    std::string error;
    if (value.contains("index")) {
        auto accepted = decode_accepted(value, error);
        if (!accepted)
            throw StorageError("unreadable acceptance record: " + error);
        return accepted;
    }
    // Legacy shape: {schema_version, request, command}.
    const auto &command = value.at("command");
    auto index = CommandIndex::parse(command.at("command_id").get<std::string>());
    if (!index)
        throw StorageError("invalid command index in acceptance record");
    const auto &request = value.at("request");
    std::optional<RequestKey> key;
    if (request.contains("request_id")) {
        key = RequestKey::parse(request.at("request_id").get<std::string>());
        if (!key)
            throw StorageError("invalid request key in acceptance record");
    }
    std::optional<std::filesystem::path> cwd;
    if (request.contains("cwd"))
        cwd = std::filesystem::path(request.at("cwd").get<std::string>());
    std::optional<std::map<std::string, std::string>> env;
    if (request.contains("env"))
        env = request.at("env").get<std::map<std::string, std::string>>();
    std::optional<std::chrono::milliseconds> timeout;
    if (request.contains("wall_ms"))
        timeout = std::chrono::milliseconds(request.at("wall_ms").get<std::int64_t>());
    AcceptedCommand accepted{
        *index,
        ExecutionRequest::create(request.at("argv").get<std::vector<std::string>>(), cwd,
                                 std::move(env), timeout, std::move(key)),
        Clock::time_point{}, std::nullopt};
    if (command.contains("cwd"))
        accepted.effective_cwd = std::filesystem::path(command.at("cwd").get<std::string>());
    return accepted;
}

std::optional<FinishedCommand> decode_finished_record(const Json &value) {
    std::string error;
    if (value.contains("result")) {
        auto finished = decode_finished(value, error);
        if (!finished)
            throw StorageError("unreadable result record: " + error);
        return finished;
    }
    // Legacy shape: the session's own command JSON, terminal states only.
    auto index = CommandIndex::parse(value.at("command_id").get<std::string>());
    if (!index)
        throw StorageError("invalid command index in result record");
    const auto state = value.value("state", std::string());
    if (state != "finished" && state != "interrupted")
        throw StorageError("existing command result is invalid; refusing to overwrite");
    FinishedCommand finished{*index, ExecutionResult{}, Clock::time_point{}};
    finished.result.outcome = state == "interrupted"
                                  ? ExecutionOutcome(Indeterminate{"no terminal result recorded"})
                                  : outcome_of_legacy(value);
    if (value.contains("stdout"))
        finished.result.out = decode_stream_fields(value.at("stdout"));
    if (value.contains("stderr"))
        finished.result.err = decode_stream_fields(value.at("stderr"));
    finished.result.cleanup.descendants_gone = state != "interrupted";
    finished.result.timing.execution =
        std::chrono::microseconds(static_cast<std::int64_t>(int_or_zero(value, "execution_ms")) *
                                  1000);
    return finished;
}

std::optional<FinishedCommand> read_finished_record(const fs::path &directory) {
    auto path = directory / "result.json";
    if (!fs::exists(path))
        return std::nullopt;
    try {
        return decode_finished_record(SessionStore::read(path));
    } catch (const StorageError &) {
        throw;
    } catch (const std::exception &e) {
        throw StorageError(e.what());
    }
}

std::optional<AcceptedCommand> read_accepted_record(const fs::path &directory) {
    auto path = directory / "request.json";
    if (!fs::exists(path))
        return std::nullopt;
    try {
        return decode_accepted_record(SessionStore::read(path));
    } catch (const StorageError &) {
        throw;
    } catch (const std::exception &e) {
        throw StorageError(e.what());
    }
}

// The record written by recovery for a command whose outcome was never observed.
FinishedCommand indeterminate_record(const AcceptedCommand &accepted) {
    FinishedCommand finished{accepted.index, ExecutionResult{}, Clock::now()};
    finished.result.outcome = ExecutionOutcome(Indeterminate{"no terminal result recorded"});
    return finished;
}
} // namespace

Json SessionStore::read(const fs::path &path) {
    try {
        auto value = Json::parse(read_file(path));
        // 3 is the current typed record; 1 and 2 are still readable session files
        // and legacy command records written by earlier builds.
        if (!value.is_object() ||
            (value.contains("schema_version") && value.at("schema_version") != 1 &&
             value.at("schema_version") != legacy_session_record_version &&
             value.at("schema_version") != record_schema_version))
            throw std::runtime_error("unsupported or corrupt session record");
        return value;
    } catch (const std::exception &e) {
        throw StorageError(e.what());
    }
}

void SessionStore::write(const std::string &name, Json value) {
    try {
        // Session snapshots keep their own version; command records carry theirs
        // from record_json and must not be restamped here.
        value["schema_version"] = legacy_session_record_version;
        store_.write_json(name, value);
    } catch (const std::exception &e) {
        throw StorageError(e.what());
    }
}

void SessionStore::accept(const AcceptedCommand &accepted) {
    const auto id = accepted.index.str();
    validate_command_id(id);
    try {
        store_.write_json(id + "/request.json", encode(accepted));
    } catch (const std::exception &e) {
        throw StorageError(e.what());
    }
}

void SessionStore::finish(const FinishedCommand &finished) {
    const auto id = finished.index.str();
    validate_command_id(id);
    const auto path = store_.directory() / id / "result.json";
    if (fs::exists(path)) {
        auto existing = read_finished_record(store_.directory() / id);
        if (!existing)
            throw StorageError("existing command result is invalid; refusing to overwrite");
        if (comparable(*existing) != comparable(finished))
            throw StorageError("refusing to change confirmed command result");
        return;
    }
    try {
        store_.write_json(id + "/result.json", encode(finished));
        // The same provenance a one-shot run gets: a consumer that cites this command can
        // verify the bytes it read are the bytes the command wrote.
        write_checksums(store_.directory() / id, id);
    } catch (const std::exception &e) {
        throw StorageError(e.what());
    }
}

std::optional<AcceptedCommand> SessionStore::accepted_record(const fs::path &directory) {
    return read_accepted_record(directory);
}

std::optional<FinishedCommand> SessionStore::finished_record(const fs::path &directory) {
    return read_finished_record(directory);
}

std::optional<AcceptedCommand> SessionStore::find_request(const fs::path &directory,
                                                         const RequestKey &key) {
    try {
        std::optional<AcceptedCommand> found;
        for (const auto &entry : fs::directory_iterator(directory)) {
            auto name = entry.path().filename().string();
            if (name.rfind("cmd-", 0) != 0)
                continue;
            validate_command_id(name);
            auto record = read_accepted_record(entry.path());
            if (!record)
                continue;
            if (record->index.str() != name)
                throw StorageError("command record identity mismatch");
            if (!record->request.key() || *record->request.key() != key)
                continue;
            if (found)
                throw StorageError("duplicate durable request ID");
            found = std::move(record);
        }
        return found;
    } catch (const StorageError &) {
        throw;
    } catch (const std::exception &e) {
        throw StorageError(e.what());
    }
}

std::optional<CommandReport> SessionStore::result(const CommandIndex &index) const {
    const auto directory = store_.directory() / index.str();
    auto finished = read_finished_record(directory);
    if (!finished)
        return std::nullopt; // accepted, no outcome yet
    auto accepted = read_accepted_record(directory);
    if (!accepted)
        throw StorageError("result without a durable acceptance");
    return CommandReport{*accepted, std::move(finished)};
}

std::optional<CommandReport> SessionStore::replay(const ExecutionRequest &request) const {
    if (!request.key())
        return std::nullopt; // legacy v1 has no dedup guarantee
    auto found = find_request(store_.directory(), *request.key());
    if (!found)
        return std::nullopt;
    if (!found->request.same_intent(request))
        throw SessionError("REQUEST_CONFLICT",
                           "request_id already belongs to different arguments");
    return CommandReport{*found, read_finished_record(store_.directory() / found->index.str())};
}

CommandReport SessionStore::request_status(const RequestKey &key) const {
    auto found = find_request(store_.directory(), key);
    if (!found)
        throw SessionError("REQUEST_NOT_FOUND", "no durable acceptance for request_id");
    return CommandReport{*found, read_finished_record(store_.directory() / found->index.str())};
}

CommandReport SessionStore::lookup_request(const fs::path &directory, const RequestKey &key) {
    auto found = find_request(directory, key);
    if (!found)
        throw SessionError("REQUEST_NOT_FOUND", "no durable acceptance for request_id");
    return CommandReport{*found, read_finished_record(directory / found->index.str())};
}

SessionRecovery SessionStore::recover(Json session) {
    // Scan durable intents as well as the snapshot: a crash may precede snapshot
    // publication. A command whose result was never written is recorded as
    // indeterminate - it must never be dispatched again.
    SessionRecovery recovery{std::move(session), std::nullopt};
    std::map<std::string, CommandReport> commands;
    for (const auto &entry : fs::directory_iterator(store_.directory())) {
        auto id = entry.path().filename().string();
        if (id.rfind("cmd-", 0) != 0)
            continue;
        validate_command_id(id);
        auto accepted = read_accepted_record(entry.path());
        if (!accepted)
            continue; // the command directory exists but acceptance never landed
        if (accepted->index.str() != id)
            throw StorageError("command record identity mismatch");
        auto finished = read_finished_record(entry.path());
        if (!finished) {
            finished = indeterminate_record(*accepted);
            store_.write_json(id + "/result.json", encode(*finished));
        }
        commands.emplace(id, CommandReport{*accepted, std::move(finished)});
    }
    if (recovery.session.contains("current") && recovery.session.at("current").is_object()) {
        auto id = recovery.session.at("current").at("command_id").get<std::string>();
        validate_command_id(id);
        auto found = commands.find(id);
        if (found != commands.end())
            recovery.last = found->second;
    }
    recovery.session["current"] = nullptr;
    return recovery;
}

void SessionStore::close(Json session) {
    snapshot(session);
    write("result.json", session);
    store_.state(RunState::finished);
}
} // namespace sandbox
