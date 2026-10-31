#include "sandbox/config.hpp"
#include "sandbox/sha256.hpp"
#include <iostream>
#include <stdexcept>
using namespace sandbox;
void require(bool v) {
    if (!v)
        throw std::runtime_error("session model assertion failed");
}
int main() {
    try {
        Sha256 empty;
        require(empty.hex() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        Sha256 abc;
        abc.update("a");
        abc.update("bc");
        require(abc.hex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        Sha256 million;
        for (int i = 0; i < 1000; ++i)
            million.update(std::string(1000, 'a'));
        require(million.hex() ==
                "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
        Json j = {{"schema_version", 1},
                  {"mode", "process"},
                  {"network", "host"},
                  {"argv", {"/bin/true"}},
                  {"session", {{"enabled", true}, {"total_ms", 1000}, {"idle_ms", 500}}}};
        auto c = RunConfig::parse(j, fs::current_path());
        c.validate(true);
        require(c.persistent && c.session_ms == 1000);
        require(RunConfig::parse(c.to_json(), fs::current_path()).to_json() == c.to_json());
        for (auto bad : {Json(-1), Json(0), Json(1001), Json(1.5), Json("5")}) {
            auto invalid = j;
            invalid["session"]["idle_ms"] = bad;
            bool rejected = false;
            try {
                auto value = RunConfig::parse(invalid, fs::current_path());
                value.validate(true);
            } catch (...) {
                rejected = true;
            }
            require(rejected);
        }
        std::cout << "session configuration and SHA-256 tests passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
