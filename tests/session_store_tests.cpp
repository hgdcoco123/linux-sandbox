#include "sandbox/session_store.hpp"
#include <iostream>
#include <unistd.h>
using namespace sandbox;
void require(bool value) {
    if (!value)
        throw std::runtime_error("store assertion");
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
        Json request = {{"op", "execute"}, {"argv", {"/bin/true"}}, {"request_id", "retry-1"}};
        Json command = {{"command_id", "cmd-1"}, {"state", "running"}};
        store.accept(request, command);
        // Crash before dispatch or snapshot: no speculative re-execution.
        require(store.replay(request, nullptr)->at("state") == "uncertain");
        require(store.replay(request, command)->at("command_id") == "cmd-1");
        auto conflict = request;
        conflict["argv"] = {"/bin/false"};
        bool rejected = false;
        try {
            store.replay(conflict, command);
        } catch (const SessionError &e) {
            rejected = e.code == "REQUEST_CONFLICT";
        }
        require(rejected);
        auto recovered = store.recover({{"session_state", "idle"}, {"current", nullptr}});
        require(store.result("cmd-1").at("state") == "interrupted");
        auto first = store.result("cmd-1");
        store.recover(recovered);
        require(first == store.result("cmd-1"));
        fs::create_directory(run->directory() / "cmd-2");
        command["command_id"] = "cmd-2";
        request["request_id"] = "retry-2";
        store.accept(request, command);
        auto terminal = command;
        terminal["state"] = "finished";
        terminal["exit_code"] = 7;
        store.finish(terminal);
        // Terminal precedes snapshot: recovery must keep the confirmed exit code.
        store.recover({{"current", command}});
        require(store.result("cmd-2").at("exit_code") == 7);
        require(store.replay(request, nullptr)->at("state") == "finished");
        require(SessionStore::lookup_request(run->directory(), "retry-2").at("exit_code") == 7);
        terminal["exit_code"] = 0;
        rejected = false;
        try {
            store.finish(terminal);
        } catch (const StorageError &) {
            rejected = true;
        }
        require(rejected);
        run->write_json("cmd-2/result.json",
                        {{"command_id", "cmd-2"}, {"state", "finished"}, {"schema_version", 99}});
        rejected = false;
        try {
            store.recover({{"current", command}});
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
