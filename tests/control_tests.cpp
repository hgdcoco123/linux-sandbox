// Portable tests for the typed control messages and their FD contracts.
#include "sandbox/core/control.hpp"

#include <cstdio>
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

void round_trip(const ControlMessage &message, const char *what) {
    try {
        auto bytes = encode(message);
        auto decoded = decode(bytes);
        if (kind_of(decoded) != kind_of(message)) {
            std::printf("FAIL: %s (kind changed: %s -> %s)\n", what, control_kind_name(kind_of(message)),
                        control_kind_name(kind_of(decoded)));
            ++failures;
            return;
        }
        if (encode(decoded) != bytes) {
            std::printf("FAIL: %s (bytes differ)\n", what);
            ++failures;
        }
    } catch (const std::exception &error) {
        std::printf("FAIL: %s (%s)\n", what, error.what());
        ++failures;
    }
}

template <class F> void check_control_error(F &&body, const char *what) {
    try {
        body();
        std::printf("FAIL: %s (accepted)\n", what);
        ++failures;
    } catch (const ControlError &) {
    } catch (const std::exception &error) {
        std::printf("FAIL: %s (wrong exception: %s)\n", what, error.what());
        ++failures;
    }
}

void test_round_trip() {
    round_trip(InitCreated{4321}, "init_created");
    round_trip(InitReaped{0x1f00}, "init_reaped");
    round_trip(BootstrapError{"clone3 denied"}, "bootstrap_error");
    round_trip(Prepare{}, "prepare");
    round_trip(Exec{}, "exec");
    round_trip(Stop{}, "stop");
    round_trip(Cancel{}, "cancel");
    round_trip(StdinHandle{*CommandIndex::from_number(1)}, "stdin_handle");
    round_trip(StdoutHandle{*CommandIndex::from_number(2)}, "stdout_handle");
    round_trip(StderrHandle{*CommandIndex::from_number(3)}, "stderr_handle");
    round_trip(Command{*CommandIndex::from_number(4), "{\"mode\":\"process\"}"}, "command");
    round_trip(EnvReady{}, "env_ready");
    round_trip(ExecObserved{}, "exec_observed");
    round_trip(StartFailed{2, 13}, "start_failed");
    round_trip(MainExit{0, std::nullopt}, "main_exit code");
    round_trip(MainExit{std::nullopt, 15}, "main_exit signal");
    round_trip(MainExit{std::nullopt, std::nullopt}, "main_exit neither");
    round_trip(InitDone{}, "init_done");
    round_trip(InitError{"mount failed"}, "init_error");
}

void test_decoded_values() {
    auto decoded = decode(encode(InitCreated{77}));
    const auto *created = std::get_if<InitCreated>(&decoded);
    check(created != nullptr && created->pid == 77, "pid preserved");

    decoded = decode(encode(MainExit{0, std::nullopt}));
    const auto *exit = std::get_if<MainExit>(&decoded);
    check(exit != nullptr, "main_exit decoded");
    if (exit) {
        check(exit->exit_code == 0, "exit code 0 preserved (not confused with absent)");
        check(!exit->signal.has_value(), "signal absent");
    }

    decoded = decode(encode(StartFailed{3, 2}));
    const auto *failed = std::get_if<StartFailed>(&decoded);
    check(failed != nullptr && failed->stage == 3 && failed->error == 2, "start failure facts");
}

void test_fd_contract() {
    check(fd_contract(ControlKind::init_created).required == 1, "init_created carries one fd");
    check(fd_contract(ControlKind::stdin_handle).required == 1, "stdin handle carries one fd");
    check(fd_contract(ControlKind::stdout_handle).required == 1, "stdout handle carries one fd");
    check(fd_contract(ControlKind::stderr_handle).required == 1, "stderr handle carries one fd");
    check(fd_contract(ControlKind::stop).required == 0, "stop carries no fd");
    check(fd_contract(ControlKind::main_exit).required == 0, "main_exit carries no fd");
    check(std::string(fd_contract(ControlKind::init_created).role) == "init pidfd", "fd role named");

    validate_fd_count(ControlKind::init_created, 1);
    validate_fd_count(ControlKind::stop, 0);
    check_control_error([] { validate_fd_count(ControlKind::init_created, 0); },
                        "missing descriptor rejected");
    check_control_error([] { validate_fd_count(ControlKind::init_created, 2); },
                        "extra descriptor rejected");
    check_control_error([] { validate_fd_count(ControlKind::stop, 1); },
                        "descriptor on a body-only message rejected");
}

void test_rejections() {
    check_control_error([] { decode("nope"); }, "short packet");
    check_control_error([] { decode("XXXX\x01\x00"); }, "bad magic");
    check_control_error([] {
        std::string packet = "CTL1";
        packet.push_back('\x01'); // init_created
        packet.push_back('\x05'); // claims 5 bytes
        decode(packet);
    }, "length beyond packet");
    check_control_error([] {
        std::string packet = "CTL1";
        packet.push_back('\x7f'); // unknown kind
        packet.push_back('\x00');
        decode(packet);
    }, "unknown kind");
    check_control_error([] {
        auto bytes = encode(InitDone{});
        bytes.push_back('\x00');
        decode(bytes);
    }, "trailing bytes");

    // A body-less message must not accept a payload.
    check_control_error([] {
        std::string packet = "CTL1";
        packet.push_back(static_cast<char>(ControlKind::exec));
        packet.push_back('\x01');
        packet.push_back('\x00');
        decode(packet);
    }, "unexpected payload on a body-less message");
}
} // namespace

int main() {
    test_round_trip();
    test_decoded_values();
    test_fd_contract();
    test_rejections();
    if (failures != 0) {
        std::printf("%d control check(s) failed\n", failures);
        return 1;
    }
    std::printf("typed control messages and fd contracts passed\n");
    return 0;
}
