#include "sandbox/core/control.hpp"

#include "sandbox/core/binary.hpp"

namespace sandbox {
namespace {
struct Errors {
    [[noreturn]] static void fail(std::string code, std::string message) {
        throw ControlError(std::move(code), std::move(message));
    }
};
using Reader = binary::Reader<Errors>;

int as_int(std::uint64_t value) {
    return static_cast<int>(value);
}
} // namespace

const char *control_kind_name(ControlKind kind) noexcept {
    switch (kind) {
    case ControlKind::init_created:
        return "init_created";
    case ControlKind::init_reaped:
        return "init_reaped";
    case ControlKind::bootstrap_error:
        return "bootstrap_error";
    case ControlKind::prepare:
        return "prepare";
    case ControlKind::exec:
        return "exec";
    case ControlKind::stop:
        return "stop";
    case ControlKind::cancel:
        return "cancel";
    case ControlKind::stdin_handle:
        return "stdin_handle";
    case ControlKind::stdout_handle:
        return "stdout_handle";
    case ControlKind::stderr_handle:
        return "stderr_handle";
    case ControlKind::command:
        return "command";
    case ControlKind::env_ready:
        return "env_ready";
    case ControlKind::exec_observed:
        return "exec_observed";
    case ControlKind::start_failed:
        return "start_failed";
    case ControlKind::main_exit:
        return "main_exit";
    case ControlKind::init_done:
        return "init_done";
    case ControlKind::init_error:
        return "init_error";
    case ControlKind::command_done:
        return "command_done";
    case ControlKind::import_tree:
        return "import_tree";
    case ControlKind::export_tree:
        return "export_tree";
    case ControlKind::transferred:
        return "transferred";
    }
    return "unknown";
}

const char *command_stop_name(CommandStop stop) noexcept {
    switch (stop) {
    case CommandStop::main_exit:
        return "main_exited";
    case CommandStop::cancelled:
        return "cancelled";
    case CommandStop::timeout:
        return "timeout";
    case CommandStop::session_closed:
        return "session_closed";
    case CommandStop::startup_failure:
        return "startup_failure";
    case CommandStop::supervision_failure:
        return "host_error";
    }
    return "unknown";
}

FdContract fd_contract(ControlKind kind) noexcept {
    switch (kind) {
    case ControlKind::import_tree:
    case ControlKind::export_tree:
    case ControlKind::transferred:
        return FdContract{0, "none"};
    case ControlKind::init_created:
        return {1, "init pidfd"};
    case ControlKind::stdin_handle:
        return {1, "target stdin write end"};
    case ControlKind::stdout_handle:
        return {1, "target stdout read end"};
    case ControlKind::stderr_handle:
        return {1, "target stderr read end"};
    default:
        return {0, "none"};
    }
}

void validate_fd_count(ControlKind kind, std::size_t count) {
    const auto contract = fd_contract(kind);
    if (count == contract.required)
        return;
    throw ControlError("FD_CONTRACT", std::string(control_kind_name(kind)) + " requires " +
                                          std::to_string(contract.required) +
                                          " descriptor(s) but received " + std::to_string(count));
}

ControlKind kind_of(const ControlMessage &message) noexcept {
    return std::visit(
        [](const auto &value) -> ControlKind {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, InitCreated>)
                return ControlKind::init_created;
            else if constexpr (std::is_same_v<T, InitReaped>)
                return ControlKind::init_reaped;
            else if constexpr (std::is_same_v<T, BootstrapError>)
                return ControlKind::bootstrap_error;
            else if constexpr (std::is_same_v<T, Prepare>)
                return ControlKind::prepare;
            else if constexpr (std::is_same_v<T, Exec>)
                return ControlKind::exec;
            else if constexpr (std::is_same_v<T, Stop>)
                return ControlKind::stop;
            else if constexpr (std::is_same_v<T, CancelCommand>)
                return ControlKind::cancel;
            else if constexpr (std::is_same_v<T, StdinHandle>)
                return ControlKind::stdin_handle;
            else if constexpr (std::is_same_v<T, StdoutHandle>)
                return ControlKind::stdout_handle;
            else if constexpr (std::is_same_v<T, StderrHandle>)
                return ControlKind::stderr_handle;
            else if constexpr (std::is_same_v<T, Command>)
                return ControlKind::command;
            else if constexpr (std::is_same_v<T, EnvReady>)
                return ControlKind::env_ready;
            else if constexpr (std::is_same_v<T, ExecObserved>)
                return ControlKind::exec_observed;
            else if constexpr (std::is_same_v<T, StartFailed>)
                return ControlKind::start_failed;
            else if constexpr (std::is_same_v<T, MainExit>)
                return ControlKind::main_exit;
            else if constexpr (std::is_same_v<T, InitDone>)
                return ControlKind::init_done;
            else if constexpr (std::is_same_v<T, CommandDone>)
                return ControlKind::command_done;
            else if constexpr (std::is_same_v<T, ImportTree>)
                return ControlKind::import_tree;
            else if constexpr (std::is_same_v<T, ExportTree>)
                return ControlKind::export_tree;
            else if constexpr (std::is_same_v<T, Transferred>)
                return ControlKind::transferred;
            else
                return ControlKind::init_error;
        },
        message);
}

std::string encode(const ControlMessage &message) {
    binary::Writer payload;
    std::visit(
        [&payload](const auto &value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, InitCreated>) {
                payload.varint(static_cast<std::uint64_t>(value.pid));
            } else if constexpr (std::is_same_v<T, InitReaped>) {
                payload.varint(static_cast<std::uint64_t>(value.wait_status));
            } else if constexpr (std::is_same_v<T, BootstrapError> || std::is_same_v<T, InitError>) {
                payload.text(value.message);
            } else if constexpr (std::is_same_v<T, StdinHandle> || std::is_same_v<T, StdoutHandle> ||
                                 std::is_same_v<T, StderrHandle>) {
                payload.varint(value.command.number());
            } else if constexpr (std::is_same_v<T, Command>) {
                payload.varint(value.command.number());
                payload.text(value.config_blob);
            } else if constexpr (std::is_same_v<T, StartFailed>) {
                payload.varint(static_cast<std::uint64_t>(value.stage));
                payload.varint(static_cast<std::uint64_t>(value.error));
            } else if constexpr (std::is_same_v<T, MainExit>) {
                payload.optional(value.exit_code, [&](int code) {
                    payload.varint(static_cast<std::uint64_t>(code));
                });
                payload.optional(value.signal,
                                 [&](int signal) { payload.varint(static_cast<std::uint64_t>(signal)); });
            } else if constexpr (std::is_same_v<T, CommandDone>) {
                payload.varint(value.command.number());
                payload.optional(value.exit_code, [&](int code) {
                    payload.varint(static_cast<std::uint64_t>(code));
                });
                payload.optional(value.signal, [&](int signal) {
                    payload.varint(static_cast<std::uint64_t>(signal));
                });
                payload.optional(value.exec_errno, [&](int error) {
                    payload.varint(static_cast<std::uint64_t>(error));
                });
                payload.u8(static_cast<std::uint8_t>(value.stop));
                payload.varint(static_cast<std::uint64_t>(value.elapsed.count()));
            } else if constexpr (std::is_same_v<T, ImportTree>) {
                payload.text(value.name);
            } else if constexpr (std::is_same_v<T, ExportTree>) {
                payload.text(value.tag);
            } else if constexpr (std::is_same_v<T, Transferred>) {
                payload.u8(value.ok ? 1 : 0);
                payload.text(value.error);
                payload.varint(value.bytes);
                payload.varint(value.entries);
            }
        },
        message);

    binary::Writer frame;
    frame.data.append(control_magic);
    frame.u8(static_cast<std::uint8_t>(kind_of(message)));
    frame.varint(payload.data.size());
    frame.data.append(payload.data);
    if (frame.data.size() > max_control_packet)
        throw ControlError("PACKET_TOO_LARGE", "control packet exceeds 16KiB");
    return std::move(frame.data);
}

ControlMessage decode(std::string_view packet) {
    if (packet.size() > max_control_packet)
        throw ControlError("PACKET_TOO_LARGE", "control packet exceeds 16KiB");
    if (packet.size() < control_magic.size() + 2 ||
        packet.substr(0, control_magic.size()) != control_magic)
        throw ControlError("BAD_PACKET", "bad control framing");
    Reader frame(packet.substr(control_magic.size()));
    const auto kind = static_cast<ControlKind>(frame.u8());
    auto length = frame.varint();
    if (length != frame.remaining())
        throw ControlError("BAD_PACKET", "payload length does not match packet");
    auto body = packet.substr(packet.size() - static_cast<std::size_t>(length),
                              static_cast<std::size_t>(length));
    Reader reader(body);
    ControlMessage message = InitDone{};
    switch (kind) {
    case ControlKind::init_created:
        message = InitCreated{as_int(reader.varint())};
        break;
    case ControlKind::init_reaped:
        message = InitReaped{as_int(reader.varint())};
        break;
    case ControlKind::bootstrap_error:
        message = BootstrapError{reader.text()};
        break;
    case ControlKind::prepare:
        message = Prepare{};
        break;
    case ControlKind::exec:
        message = Exec{};
        break;
    case ControlKind::stop:
        message = Stop{};
        break;
    case ControlKind::cancel:
        message = CancelCommand{};
        break;
    case ControlKind::stdin_handle: {
        auto index = CommandIndex::from_number(static_cast<std::uint32_t>(reader.varint()));
        if (!index)
            throw ControlError("BAD_PACKET", "invalid command index");
        message = StdinHandle{*index};
        break;
    }
    case ControlKind::stdout_handle: {
        auto index = CommandIndex::from_number(static_cast<std::uint32_t>(reader.varint()));
        if (!index)
            throw ControlError("BAD_PACKET", "invalid command index");
        message = StdoutHandle{*index};
        break;
    }
    case ControlKind::stderr_handle: {
        auto index = CommandIndex::from_number(static_cast<std::uint32_t>(reader.varint()));
        if (!index)
            throw ControlError("BAD_PACKET", "invalid command index");
        message = StderrHandle{*index};
        break;
    }
    case ControlKind::command: {
        auto index = CommandIndex::from_number(static_cast<std::uint32_t>(reader.varint()));
        if (!index)
            throw ControlError("BAD_PACKET", "invalid command index");
        message = Command{*index, reader.text()};
        break;
    }
    case ControlKind::env_ready:
        message = EnvReady{};
        break;
    case ControlKind::exec_observed:
        message = ExecObserved{};
        break;
    case ControlKind::start_failed:
        message = StartFailed{as_int(reader.varint()), as_int(reader.varint())};
        break;
    case ControlKind::main_exit: {
        MainExit exit;
        exit.exit_code = reader.optional<int>([&] { return as_int(reader.varint()); });
        exit.signal = reader.optional<int>([&] { return as_int(reader.varint()); });
        message = std::move(exit);
        break;
    }
    case ControlKind::init_done:
        message = InitDone{};
        break;
    case ControlKind::init_error:
        message = InitError{reader.text()};
        break;
    case ControlKind::command_done: {
        auto index = CommandIndex::from_number(static_cast<std::uint32_t>(reader.varint()));
        if (!index)
            throw ControlError("BAD_PACKET", "invalid command index");
        auto code = reader.optional<int>([&] { return as_int(reader.varint()); });
        auto signal = reader.optional<int>([&] { return as_int(reader.varint()); });
        auto exec_errno = reader.optional<int>([&] { return as_int(reader.varint()); });
        const auto stop = reader.u8();
        if (stop > static_cast<std::uint8_t>(CommandStop::supervision_failure))
            throw ControlError("BAD_PACKET", "unknown command stop reason");
        auto elapsed = std::chrono::milliseconds(static_cast<std::int64_t>(reader.varint()));
        message =
            CommandDone{*index, code, signal, exec_errno, static_cast<CommandStop>(stop), elapsed};
        break;
    }
    case ControlKind::import_tree:
        message = ImportTree{reader.text()};
        break;
    case ControlKind::export_tree:
        message = ExportTree{reader.text()};
        break;
    case ControlKind::transferred: {
        const auto ok = reader.u8();
        if (ok > 1)
            throw ControlError("BAD_PACKET", "invalid transfer flag");
        auto error = reader.text();
        auto bytes = reader.varint();
        auto entries = reader.varint();
        if (entries > 0xFFFFFFFFULL)
            throw ControlError("BAD_PACKET", "entry count out of range");
        message = Transferred{ok == 1, std::move(error), bytes,
                              static_cast<std::uint32_t>(entries)};
        break;
    }
    default:
        throw ControlError("BAD_PACKET", "unknown control kind");
    }
    reader.require_end();
    return message;
}

} // namespace sandbox
