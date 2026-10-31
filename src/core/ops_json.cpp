#include "sandbox/core/ops_json.hpp"

#include <array>
#include <cstdint>

namespace sandbox {
namespace {
using Json = nlohmann::json;

// Field sets are closed: a typo such as "cwd2" must be reported, not ignored.
bool only_keys(const Json &object, std::initializer_list<const char *> allowed,
               std::string &error, const char *where) {
    for (auto it = object.begin(); it != object.end(); ++it) {
        bool known = false;
        for (const char *key : allowed)
            if (it.key() == key)
                known = true;
        if (!known) {
            error = std::string(where) + ": unknown field " + it.key();
            return false;
        }
    }
    return true;
}

// JSON literals such as 1500 parse as signed integers, so both spellings count.
bool nonnegative(const Json &value, std::uint64_t &out) {
    if (value.is_number_unsigned()) {
        out = value.get<std::uint64_t>();
        return true;
    }
    if (value.is_number_integer()) {
        const auto signed_value = value.get<std::int64_t>();
        if (signed_value >= 0) {
            out = static_cast<std::uint64_t>(signed_value);
            return true;
        }
    }
    return false;
}

std::optional<CommandIndex> parse_command(const Json &value, std::string &error) {
    if (!value.is_string()) {
        error = "command_id must be a string";
        return std::nullopt;
    }
    auto parsed = CommandIndex::parse(value.get<std::string>());
    if (!parsed)
        error = "invalid command_id";
    return parsed;
}

bool optional_command(const Json &object, std::optional<CommandIndex> &out, std::string &error) {
    if (!object.contains("command_id"))
        return true;
    auto parsed = parse_command(object.at("command_id"), error);
    if (!parsed)
        return false;
    out = parsed;
    return true;
}
} // namespace

std::optional<SessionRequest> session_request_from_json(const Json &input, std::string &error) {
    if (!input.is_object()) {
        error = "request must be an object";
        return std::nullopt;
    }
    // protocol_version is envelope metadata from the wire layer, not part of the
    // business request; it is accepted and dropped here so callers do not have to
    // strip it before every call.
    Json request = input;
    request.erase("protocol_version");
    if (!request.contains("op") || !request.at("op").is_string()) {
        error = "missing op";
        return std::nullopt;
    }
    const auto op = request.at("op").get<std::string>();
    try {
        if (op == "execute") {
            if (!only_keys(request, {"op", "request_id", "argv", "cwd", "env", "wall_ms"}, error,
                           "execute"))
                return std::nullopt;
            if (!request.contains("argv") || !request.at("argv").is_array()) {
                error = "execute: argv must be an array";
                return std::nullopt;
            }
            std::optional<std::filesystem::path> cwd;
            std::optional<std::map<std::string, std::string>> env;
            std::optional<std::chrono::milliseconds> timeout;
            std::optional<RequestKey> key;
            if (request.contains("cwd")) {
                if (!request.at("cwd").is_string()) {
                    error = "execute: cwd must be a string";
                    return std::nullopt;
                }
                cwd = std::filesystem::path(request.at("cwd").get<std::string>());
            }
            if (request.contains("env")) {
                if (!request.at("env").is_object()) {
                    error = "execute: env must be an object";
                    return std::nullopt;
                }
                std::map<std::string, std::string> parsed;
                for (const auto &entry : request.at("env").items()) {
                    if (!entry.value().is_string()) {
                        error = "execute: env values must be strings";
                        return std::nullopt;
                    }
                    parsed.emplace(entry.key(), entry.value().get<std::string>());
                }
                env = std::move(parsed);
            }
            if (request.contains("wall_ms")) {
                std::uint64_t milliseconds = 0;
                if (!nonnegative(request.at("wall_ms"), milliseconds)) {
                    error = "execute: wall_ms must be a nonnegative integer";
                    return std::nullopt;
                }
                timeout = std::chrono::milliseconds(static_cast<std::int64_t>(milliseconds));
            }
            if (request.contains("request_id")) {
                if (!request.at("request_id").is_string()) {
                    error = "execute: request_id must be a string";
                    return std::nullopt;
                }
                key = RequestKey::parse(request.at("request_id").get<std::string>());
                if (!key) {
                    error = "execute: invalid request_id";
                    return std::nullopt;
                }
            }
            auto argv = request.at("argv").get<std::vector<std::string>>();
            return SessionRequest::create(
                Execute{ExecutionRequest::create(std::move(argv), std::move(cwd), std::move(env),
                                                 timeout, std::move(key))});
        }
        if (op == "status" || op == "close") {
            if (!only_keys(request, {"op"}, error, op.c_str()))
                return std::nullopt;
            return SessionRequest::create(op == "status" ? SessionOperation{Status{}}
                                                         : SessionOperation{Close{}});
        }
        if (op == "cancel") {
            if (!only_keys(request, {"op", "command_id"}, error, "cancel"))
                return std::nullopt;
            std::optional<CommandIndex> index;
            if (!optional_command(request, index, error))
                return std::nullopt;
            return SessionRequest::create(Cancel{index});
        }
        if (op == "stdin") {
            if (!only_keys(request, {"op", "command_id", "bytes"}, error, "stdin"))
                return std::nullopt;
            if (!request.contains("command_id")) {
                error = "missing command_id";
                return std::nullopt;
            }
            auto index = parse_command(request.at("command_id"), error);
            if (!index)
                return std::nullopt;
            if (!request.contains("bytes") || !request.at("bytes").is_array()) {
                error = "stdin: bytes must be an array of 0..255";
                return std::nullopt;
            }
            std::string bytes;
            for (const auto &item : request.at("bytes")) {
                std::uint64_t octet = 0;
                if (!nonnegative(item, octet) || octet > 255) {
                    error = "stdin: bytes must be an array of 0..255";
                    return std::nullopt;
                }
                bytes.push_back(static_cast<char>(octet));
            }
            return SessionRequest::create(StdinWrite{*index, std::move(bytes)});
        }
        if (op == "stdin-close") {
            if (!only_keys(request, {"op", "command_id"}, error, "stdin-close"))
                return std::nullopt;
            if (!request.contains("command_id")) {
                error = "missing command_id";
                return std::nullopt;
            }
            auto index = parse_command(request.at("command_id"), error);
            if (!index)
                return std::nullopt;
            return SessionRequest::create(StdinClose{*index});
        }
        if (op == "output") {
            if (!only_keys(request, {"op", "command_id", "stream", "offset"}, error, "output"))
                return std::nullopt;
            if (!request.contains("command_id")) {
                error = "missing command_id";
                return std::nullopt;
            }
            auto index = parse_command(request.at("command_id"), error);
            if (!index)
                return std::nullopt;
            Stream stream = Stream::standard_output;
            const auto name = request.value("stream", std::string("stdout"));
            if (name == "stderr")
                stream = Stream::standard_error;
            else if (name != "stdout") {
                error = "output: stream must be stdout or stderr";
                return std::nullopt;
            }
            std::uint64_t offset = 0;
            if (request.contains("offset")) {
                if (!nonnegative(request.at("offset"), offset)) {
                    error = "output: offset must be a nonnegative integer";
                    return std::nullopt;
                }
            }
            return SessionRequest::create(ReadOutput{*index, stream, offset});
        }
        if (op == "result") {
            if (!only_keys(request, {"op", "command_id"}, error, "result"))
                return std::nullopt;
            if (!request.contains("command_id")) {
                error = "missing command_id";
                return std::nullopt;
            }
            auto index = parse_command(request.at("command_id"), error);
            if (!index)
                return std::nullopt;
            return SessionRequest::create(ReadResult{*index});
        }
        if (op == "request-status") {
            if (!only_keys(request, {"op", "request_id"}, error, "request-status"))
                return std::nullopt;
            if (!request.contains("request_id") || !request.at("request_id").is_string()) {
                error = "request-status: request_id must be a string";
                return std::nullopt;
            }
            auto key = RequestKey::parse(request.at("request_id").get<std::string>());
            if (!key) {
                error = "request-status: invalid request_id";
                return std::nullopt;
            }
            return SessionRequest::create(RequestStatusQuery{*key});
        }
        if (op == "files") {
            if (!only_keys(request, {"op", "request"}, error, "files"))
                return std::nullopt;
            if (!request.contains("request") || !request.at("request").is_object()) {
                error = "files: request must be an object";
                return std::nullopt;
            }
            const auto &inner = request.at("request");
            const auto action = inner.value("op", std::string{});
            if (action == "snapshot") {
                if (!only_keys(inner, {"op", "source", "name"}, error, "files.snapshot"))
                    return std::nullopt;
                if (!inner.contains("source") || !inner.contains("name")) {
                    error = "files.snapshot: source and name are required";
                    return std::nullopt;
                }
                return SessionRequest::create(Workspace{WorkspaceSnapshot{
                    inner.at("source").get<std::string>(), inner.at("name").get<std::string>()}});
            }
            if (action == "manifest") {
                if (!only_keys(inner, {"op"}, error, "files.manifest"))
                    return std::nullopt;
                return SessionRequest::create(Workspace{WorkspaceManifest{}});
            }
            if (action == "export") {
                if (!only_keys(inner, {"op", "destination"}, error, "files.export"))
                    return std::nullopt;
                if (!inner.contains("destination")) {
                    error = "files.export: destination is required";
                    return std::nullopt;
                }
                return SessionRequest::create(
                    Workspace{WorkspaceExport{inner.at("destination").get<std::string>()}});
            }
            error = "files: unknown operation";
            return std::nullopt;
        }
        error = "unknown session operation: " + op;
        return std::nullopt;
    } catch (const RequestError &failure) {
        error = failure.what();
        return std::nullopt;
    } catch (const std::exception &failure) {
        error = failure.what();
        return std::nullopt;
    }
}

} // namespace sandbox
