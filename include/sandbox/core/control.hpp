#pragma once
// Typed messages on the launcher/init control channel.
//
// Every message declares how many file descriptors it must carry, so the FD
// contract is data rather than four `kind == "..."` comparisons buried in the
// socket layer. A message with the wrong number of descriptors is rejected
// before its payload is interpreted.
#include "sandbox/core/ids.hpp"
#include "sandbox/core/result.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>

namespace sandbox {

class ControlError : public std::runtime_error {
  public:
    ControlError(std::string code, std::string message)
        : std::runtime_error(std::move(message)), code(std::move(code)) {}
    std::string code;
};

enum class ControlKind : std::uint8_t {
    // launcher -> supervisor
    init_created = 1,
    init_reaped = 2,
    bootstrap_error = 3,
    // supervisor -> init
    prepare = 4,
    exec = 5,
    stop = 6,
    cancel = 7,
    stdin_handle = 8,
    stdout_handle = 9,
    stderr_handle = 10,
    command = 11,
    // init -> supervisor
    env_ready = 12,
    exec_observed = 13,
    start_failed = 14,
    main_exit = 15,
    init_done = 16,
    init_error = 17,
    command_done = 18,
    // Workspace transfer between the run directory and the sandbox-owned tmpfs.
    // Both directions are explicit requests: the host cannot read a tmpfs mounted
    // inside the sandbox's namespace, so the sandbox has to move its own data.
    import_tree = 19,
    export_tree = 20,
    transferred = 21,
};

// Why a session command ended, as reported by the pid-1 process.
enum class CommandStop : std::uint8_t {
    main_exit = 0,
    cancelled = 1,
    timeout = 2,
    session_closed = 3,
    startup_failure = 4,
    supervision_failure = 5,
};
const char *command_stop_name(CommandStop) noexcept;

const char *control_kind_name(ControlKind) noexcept;

// How many SCM_RIGHTS descriptors a message must carry, and what they are for.
struct FdContract {
    std::uint8_t required = 0;
    const char *role = "none";
};

FdContract fd_contract(ControlKind) noexcept;

// Throws ControlError on a count mismatch. `count` is what actually arrived.
void validate_fd_count(ControlKind, std::size_t count);

struct InitCreated {
    int pid = 0;
};
struct InitReaped {
    int wait_status = 0;
};
struct BootstrapError {
    std::string message;
};
struct Prepare {};
struct Exec {};
struct Stop {};
struct CancelCommand {};
struct StdinHandle {
    CommandIndex command;
};
struct StdoutHandle {
    CommandIndex command;
};
struct StderrHandle {
    CommandIndex command;
};
// The effective run configuration is the configuration boundary: it travels as
// an opaque blob the control layer never interprets.
struct Command {
    CommandIndex command;
    std::string config_blob;
};
struct EnvReady {};
struct ExecObserved {};
struct StartFailed {
    int stage = 0;
    int error = 0;
};
struct MainExit {
    std::optional<int> exit_code;
    std::optional<int> signal;
};
struct InitDone {};
struct InitError {
    std::string message;
};
// Facts about one finished session command. The stop reason is a fact, not a
// decision: the controller still decides the outcome.
struct CommandDone {
    CommandIndex command;
    std::optional<int> exit_code{};
    std::optional<int> signal{};
    std::optional<int> exec_errno{};
    CommandStop stop = CommandStop::main_exit;
    std::chrono::milliseconds elapsed{0};
};

// Move one uploaded tree into the sandbox workspace. `name` is a single path
// component: the destination is always /output/<name>.
struct ImportTree {
    std::string name;
};
// Copy the whole sandbox workspace out to /transfer/<tag>. The tag names a
// destination that must not already exist, so a failed export never overwrites a
// previous good copy.
struct ExportTree {
    std::string tag;
};
// The answer to either request. `ok` is about the copy, not about the run: a
// failed export is reported here and must not be mistaken for a failed target.
struct Transferred {
    bool ok = false;
    std::string error;
    std::uint64_t bytes = 0;
    std::uint32_t entries = 0;
};

using ControlMessage = std::variant<InitCreated, InitReaped, BootstrapError, Prepare, Exec, Stop,
                                    CancelCommand, StdinHandle, StdoutHandle, StderrHandle, Command, EnvReady,
                                    ExecObserved, StartFailed, MainExit, InitDone, InitError,
                                    CommandDone, ImportTree, ExportTree, Transferred>;

[[nodiscard]] ControlKind kind_of(const ControlMessage &) noexcept;

inline constexpr std::size_t max_control_packet = 16384;
inline constexpr std::string_view control_magic = "CTL1";

[[nodiscard]] std::string encode(const ControlMessage &);
// Throws ControlError on framing, tag or field problems. FD count is the
// channel's business: it calls validate_fd_count() with what recvmsg returned.
[[nodiscard]] ControlMessage decode(std::string_view packet);

} // namespace sandbox
