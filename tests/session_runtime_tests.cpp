#include "sandbox/command_execution.hpp"
#include "sandbox/protocol.hpp"
#include "sandbox/snapshot.hpp"
#include <fstream>
#include <iostream>
#include <unistd.h>
using namespace sandbox;
void require(bool ok) {
    if (!ok)
        throw std::runtime_error("session runtime assertion failed");
}
std::size_t descriptors() {
    return std::distance(fs::directory_iterator("/proc/self/fd"), fs::directory_iterator{});
}
int main() {
    try {
        auto pair = SocketPair::create();
        ControlChannel sender(std::move(pair.first)), receiver(std::move(pair.second));
        auto null = open_path("/dev/null", O_RDWR);
        for (const auto &message : {ControlMessage{StdinHandle{*CommandIndex::from_number(1)}},
                                    ControlMessage{StdoutHandle{*CommandIndex::from_number(1)}},
                                    ControlMessage{StderrHandle{*CommandIndex::from_number(1)}}}) {
            sender.send(message, null.get());
            auto received = receiver.receive();
            require(received && received->fd);
        }
        // A body-only message must not carry a descriptor: rejected at the sender.
        bool rejected = false;
        try {
            sender.send(EnvReady{}, null.get());
        } catch (...) {
            rejected = true;
        }
        require(rejected);
        sender.send(EnvReady{});
        auto next = receiver.receive();
        require(next && std::holds_alternative<EnvReady>(next->message));
        char pattern[] = "/tmp/sbx-prepare-XXXXXX";
        auto temp = ::mkdtemp(pattern);
        if (!temp)
            system_error("mkdtemp");
        fs::path root = temp;
        try {
            auto failed = root / "failed";
            fs::create_directory(failed);
            fs::create_directory(failed / "stderr.bin");
            auto count = descriptors();
            rejected = false;
            try {
                CommandExecution attempt(failed, 1024);
            } catch (...) {
                rejected = true;
            }
            require(rejected &&
                    descriptors() == count); // stdout opened, stderr failed: no FD leak.
            auto next = root / "next";
            fs::create_directory(next);
            {
                CommandExecution prepared(next, 1024);
                require(prepared.out && prepared.err && prepared.input.write);
            }
            require(descriptors() == count);
        } catch (...) {
            fs::remove_all(root);
            throw;
        }
        fs::remove_all(root);
        // The /input boundary: a snapshot is a copy taken now, not a bind.
        char snapshot_pattern[] = "/tmp/sbx-snapshot-XXXXXX";
        auto created = ::mkdtemp(snapshot_pattern);
        if (!created)
            system_error("mkdtemp");
        fs::path snapshot_root = created;
        try {
            auto host = snapshot_root / "host";
            fs::create_directory(host);
            std::ofstream(host / "data") << "original";
            auto state = snapshot_root / "state";
            fs::create_directory(state);
            RunConfig config;
            config.mode = "process";
            config.workspace.input = InputMode::snapshot_copy;
            config.mounts.push_back({host, "/input", true});
            auto copies = snapshot_inputs(config, state);
            require(copies.size() == 1 && config.mounts.size() == 1);
            require(config.mounts[0].source == copies[0]);
            require(config.mounts[0].source != host); // nothing downstream sees the host tree
            std::string text;
            std::ifstream(copies[0] / "data") >> text;
            require(text == "original");
            // A later host write must not reach the sandbox.
            std::ofstream(host / "data") << "changed";
            std::ifstream(copies[0] / "data") >> text;
            require(text == "original");
            // A symlink in the source is refused rather than followed.
            fs::create_symlink("/etc/passwd", host / "link");
            auto other_state = snapshot_root / "state2";
            fs::create_directory(other_state);
            RunConfig refused;
            refused.mode = "process";
            refused.workspace.input = InputMode::snapshot_copy;
            refused.mounts.push_back({host, "/input", true});
            bool rejected_symlink = false;
            try {
                snapshot_inputs(refused, other_state);
            } catch (const std::exception &) {
                rejected_symlink = true;
            }
            require(rejected_symlink);
            // bind_readonly is untouched: the mode is an explicit choice.
            fs::remove(host / "link");
            RunConfig bound;
            bound.mode = "process";
            bound.mounts.push_back({host, "/input", true});
            require(snapshot_inputs(bound, state).empty());
            require(bound.mounts[0].source == host);
            // The copier applies the *configured* budget through the same
            // admission policy the unit tests cover, rather than its own copy.
            auto many = snapshot_root / "many";
            fs::create_directory(many);
            for (auto name : {"a", "b", "c"})
                std::ofstream(many / name) << "x";
            auto limited_state = snapshot_root / "state3";
            fs::create_directory(limited_state);
            RunConfig limited;
            limited.mode = "process";
            limited.workspace.input = InputMode::snapshot_copy;
            limited.workspace.max_entries = 2;
            limited.mounts.push_back({many, "/input", true});
            bool refused_entries = false;
            try {
                snapshot_inputs(limited, limited_state);
            } catch (const std::exception &failure) {
                refused_entries = std::string(failure.what()).find("entry_limit") !=
                                  std::string::npos;
            }
            require(refused_entries);
            auto byte_state = snapshot_root / "state4";
            fs::create_directory(byte_state);
            RunConfig tiny;
            tiny.mode = "process";
            tiny.workspace.input = InputMode::snapshot_copy;
            tiny.workspace.max_bytes = 1;
            tiny.mounts.push_back({many, "/input", true});
            bool refused_bytes = false;
            try {
                snapshot_inputs(tiny, byte_state);
            } catch (const std::exception &failure) {
                refused_bytes = std::string(failure.what()).find("byte_limit") !=
                                std::string::npos;
            }
            require(refused_bytes);
        } catch (...) {
            fs::remove_all(snapshot_root);
            throw;
        }
        fs::remove_all(snapshot_root);
        std::cout << "session protocol and rollback tests passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
