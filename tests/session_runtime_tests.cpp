#include "sandbox/command_execution.hpp"
#include "sandbox/protocol.hpp"
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
        for (auto kind : {"STDIN", "STDOUT", "STDERR"}) {
            sender.send(kind, Json::object(), null.get());
            auto received = receiver.receive();
            require(received && received->fd && received->body.at("payload").is_object());
        }
        bool rejected = false;
        try {
            sender.send("STDIN", Json(nullptr), null.get());
        } catch (...) {
            rejected = true;
        }
        require(rejected);
        sender.send("NEXT");
        require(receiver.receive()->body.at("kind") == "NEXT");
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
        std::cout << "session protocol and rollback tests passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
