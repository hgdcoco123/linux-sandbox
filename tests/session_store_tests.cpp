#include "sandbox/core/record_json.hpp"
#include "sandbox/session_store.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <unistd.h>

using namespace sandbox;

void require(bool value) {
    if (!value)
        throw std::runtime_error("store assertion");
}

using Clock = std::chrono::system_clock;

ExecutionRequest intent_of(const std::vector<std::string> &argv, const std::string &key) {
    auto parsed = RequestKey::parse(key);
    if (!parsed)
        throw std::runtime_error("bad test key");
    return ExecutionRequest::create(argv, std::nullopt, std::nullopt, std::nullopt, parsed);
}

FinishedCommand finished_of(const std::string &id, ExecutionOutcome outcome,
                            std::uint64_t stdout_saved = 0) {
    FinishedCommand finished{*CommandIndex::parse(id), ExecutionResult{}, Clock::now()};
    finished.result.outcome = std::move(outcome);
    finished.result.out.saved_bytes = stdout_saved;
    finished.result.out.eof = true;
    finished.result.out.flushed = true;
    finished.result.err.eof = true;
    finished.result.err.flushed = true;
    finished.result.cleanup.descendants_gone = true;
    finished.result.timing.execution = std::chrono::milliseconds(3);
    return finished;
}

// A command record written by a build that predates the typed records: the stored
// JSON is the session's own command JSON, not a core record.
void write_legacy_records(const fs::path &directory) {
    fs::create_directory(directory);
    Json request = {{"schema_version", 2},
                    {"request",
                     {{"op", "execute"}, {"argv", {"/bin/true"}}, {"request_id", "retry-3"}}},
                    {"command",
                     {{"command_id", "cmd-3"},
                      {"argv", {"/bin/true"}},
                      {"cwd", "/tmp"},
                      {"state", "running"}}}};
    Json result = {{"schema_version", 2},
                   {"command_id", "cmd-3"},
                   {"state", "finished"},
                   {"exit_code", 0},
                   {"signal", nullptr},
                   {"stop_reason", "main_exited"},
                   {"execution_ms", 5},
                   {"stdout", {{"saved_bytes", 11}, {"discarded_bytes", 0}, {"eof", true},
                               {"flushed", true}}},
                   {"stderr", {{"saved_bytes", 0}, {"discarded_bytes", 0}, {"eof", true},
                               {"flushed", true}}}};
    std::ofstream(directory / "request.json") << request.dump();
    std::ofstream(directory / "result.json") << result.dump();
}

int main() {
    char pattern[] = "/tmp/sbx-store-XXXXXX";
    auto temp = ::mkdtemp(pattern);
    if (!temp)
        return 1;
    fs::path root = temp;
    try {
        RunConfig config;
        config.mode = "process";
        auto run = RunStore::create(root, config);
        SessionStore store(*run);
        fs::create_directory(run->directory() / "cmd-1");
        auto intent = intent_of({"/bin/true"}, "retry-1");
        store.accept(AcceptedCommand{*CommandIndex::parse("cmd-1"), intent, Clock::now(),
                                     fs::path("/tmp")});
        // Crash before dispatch or snapshot: no speculative re-execution.
        require(store.replay(intent, nullptr)->at("state") == "uncertain");
        Json active = {{"command_id", "cmd-1"}, {"state", "running"}};
        require(store.replay(intent, active)->at("command_id") == "cmd-1");
        auto conflict = intent_of({"/bin/false"}, "retry-1");
        bool rejected = false;
        try {
            store.replay(conflict, active);
        } catch (const SessionError &e) {
            rejected = e.code == "REQUEST_CONFLICT";
        }
        require(rejected);
        // A request that was never accepted has no durable answer.
        require(!store.replay(intent_of({"/bin/true"}, "never-seen"), nullptr).has_value());
        auto recovered = store.recover({{"session_state", "idle"}, {"current", nullptr}});
        require(store.result(*CommandIndex::parse("cmd-1")).at("state") == "interrupted");
        auto first = store.result(*CommandIndex::parse("cmd-1"));
        store.recover(recovered);
        require(first == store.result(*CommandIndex::parse("cmd-1")));
        fs::create_directory(run->directory() / "cmd-2");
        auto second = intent_of({"/bin/true"}, "retry-2");
        auto second_index = *CommandIndex::parse("cmd-2");
        store.accept(AcceptedCommand{second_index, second, Clock::now(), fs::path("/tmp")});
        store.finish(finished_of("cmd-2", ExecutionOutcome(Exited{7})));
        // Terminal precedes snapshot: recovery must keep the confirmed exit code.
        store.recover({{"current", Json{{"command_id", "cmd-2"}}}});
        require(store.result(second_index).at("exit_code") == 7);
        require(store.replay(second, nullptr)->at("state") == "finished");
        require(SessionStore::lookup_request(run->directory(), *RequestKey::parse("retry-2"))
                    .at("exit_code") == 7);
        // A confirmed result is immutable, even when the new facts look plausible.
        rejected = false;
        try {
            store.finish(finished_of("cmd-2", ExecutionOutcome(Exited{0})));
        } catch (const StorageError &) {
            rejected = true;
        }
        require(rejected);
        // The same facts twice is not a change.
        store.finish(finished_of("cmd-2", ExecutionOutcome(Exited{7})));
        // A record written by the previous storage version stays readable.
        write_legacy_records(run->directory() / "cmd-3");
        auto legacy = SessionStore::lookup_request(run->directory(), *RequestKey::parse("retry-3"));
        require(legacy.at("state") == "finished" && legacy.at("exit_code") == 0);
        require(legacy.at("cwd") == "/tmp");
        require(legacy.at("stdout").at("stored") == 11);
        // Startup failures keep the exec errno; a cancelled command keeps its cause.
        fs::create_directory(run->directory() / "cmd-4");
        store.accept(AcceptedCommand{*CommandIndex::parse("cmd-4"),
                                     intent_of({"/nonexistent"}, "retry-4"), Clock::now(),
                                     fs::path("/tmp")});
        store.finish(finished_of("cmd-4", ExecutionOutcome(StartupFailed{"exec failed", 2})));
        require(store.result(*CommandIndex::parse("cmd-4")).at("exec_errno") == 2);
        run->write_json("cmd-2/result.json",
                        {{"command_id", "cmd-2"}, {"state", "finished"}, {"schema_version", 99}});
        rejected = false;
        try {
            store.recover({{"current", Json{{"command_id", "cmd-2"}}}});
        } catch (const StorageError &) {
            rejected = true;
        }
        require(rejected);
        run.reset();
        fs::remove_all(root);
        std::cout << "durable intent, deduplication and recovery tests passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        fs::remove_all(root);
        return 1;
    }
}
