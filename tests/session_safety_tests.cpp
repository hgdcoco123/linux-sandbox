#include "sandbox/session_safety.hpp"
#include <iostream>
using namespace sandbox;
void require(bool ok) {
    if (!ok)
        throw std::runtime_error("session safety assertion failed");
}
int main() {
    try {
        Json before = {{"cpu.stat", {{"usage_usec", 10}}}};
        require(session_counter_delta(before, Json{{"cpu.stat", {{"usage_usec", 16}}}},
                                      "usage_usec") == 6);
        for (auto invalid :
             {Json(nullptr), Json::object(), Json{{"cpu.stat", nullptr}},
              Json{{"cpu.stat", Json::array()}}, Json{{"cpu.stat", {{"usage_usec", nullptr}}}},
              Json{{"cpu.stat", {{"usage_usec", -1}}}}, Json{{"cpu.stat", {{"usage_usec", 1.5}}}},
              Json{{"cpu.stat", {{"usage_usec", 9}}}}})
            require(session_counter_delta(before, invalid, "usage_usec").is_null());
        Json running = {{"command_id", "cmd-1"}, {"state", "running"}};
        Json finished = {{"command_id", "cmd-1"},
                         {"state", "finished"},
                         {"exit_code", 7},
                         {"extra", "preserve"}};
        require(recover_command(running, finished) == finished);
        require(recover_command(running, recover_command(running, finished)) == finished);
        auto interrupted = recover_command(running, std::nullopt);
        require(interrupted["state"] == "interrupted" && interrupted["exit_code"].is_null());
        require(recover_command(running, interrupted) == interrupted);
        bool rejected = false;
        try {
            recover_command(running, Json{{"command_id", "cmd-2"}, {"state", "finished"}});
        } catch (...) {
            rejected = true;
        }
        require(rejected);
        std::cout << "session safety tests passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
