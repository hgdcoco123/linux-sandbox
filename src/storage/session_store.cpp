#include "sandbox/session_store.hpp"
namespace sandbox {
Json SessionStore::read(const fs::path &path) {
    try {
        auto value = Json::parse(read_file(path));
        if (!value.is_object() ||
            (value.contains("schema_version") && value.at("schema_version") != 1 &&
             value.at("schema_version") != 2))
            throw std::runtime_error("unsupported or corrupt session record");
        return value;
    } catch (const std::exception &e) {
        throw StorageError(e.what());
    }
}
void SessionStore::write(const std::string &name, Json value) {
    try {
        value["schema_version"] = 2;
        store_.write_json(name, value);
    } catch (const std::exception &e) {
        throw StorageError(e.what());
    }
}
void SessionStore::accept(const Json &request, const Json &command) {
    // The durable intent precedes all descriptor/COMMAND sends. It is never removed
    // on a dispatch failure: retry must resolve this command, not start a new one.
    auto id = command.at("command_id").get<std::string>();
    validate_command_id(id);
    write(id + "/request.json", {{"request", request}, {"command", command}});
}
std::optional<Json> SessionStore::find_request(const fs::path &directory, const std::string &id) {
    validate_request_id(id);
    try {
        std::optional<Json> found;
        for (const auto &e : fs::directory_iterator(directory)) {
            auto name = e.path().filename().string();
            if (name.rfind("cmd-", 0) != 0)
                continue;
            validate_command_id(name);
            if (!fs::exists(e.path() / "request.json"))
                continue;
            auto record = read(e.path() / "request.json");
            if (record.at("command").at("command_id") != name)
                throw StorageError("command record identity mismatch");
            if (record.at("request").value("request_id", "") == id) {
                if (found)
                    throw StorageError("duplicate durable request ID");
                found = std::move(record);
            }
        }
        return found;
    } catch (const StorageError &) {
        throw;
    } catch (const std::exception &e) {
        throw StorageError(e.what());
    }
}
Json SessionStore::result(const std::string &id) const {
    validate_command_id(id);
    auto path = store_.directory() / id / "result.json";
    if (!fs::exists(path))
        return {{"state", "pending"}};
    auto value = read(path);
    try {
        return recover_command(Json{{"command_id", id}}, value);
    } catch (const std::exception &e) {
        throw StorageError(e.what());
    }
}
Json SessionStore::describe(const fs::path &directory, const Json &record, const Json &active) {
    auto command = record.at("command");
    auto id = command.at("command_id").get<std::string>();
    validate_command_id(id);
    auto path = directory / id / "result.json";
    if (fs::exists(path)) {
        try {
            return recover_command(command, read(path));
        } catch (const std::exception &e) {
            throw StorageError(e.what());
        }
    }
    if (active.is_object() && active.at("command_id") == id)
        return active;
    command["state"] = "uncertain";
    command["outcome_known"] = false;
    return command;
}
std::optional<Json> SessionStore::replay(const Json &request, const Json &active) const {
    return replay(ExecuteRequest::parse(request), active);
}
std::optional<Json> SessionStore::replay(const ExecuteRequest &request, const Json &active) const {
    if (!request.request_id)
        return std::nullopt; // legacy v1 has no dedup guarantee
    auto found = find_request(store_.directory(), request.request_id->str());
    if (!found)
        return std::nullopt;
    if (!ExecuteRequest::parse(found->at("request")).same_intent(request))
        throw SessionError("REQUEST_CONFLICT", "request_id already belongs to different arguments");
    return describe(store_.directory(), *found, active);
}
Json SessionStore::request_status(const std::string &id, const Json &active) const {
    auto found = find_request(store_.directory(), id);
    if (!found)
        throw SessionError("REQUEST_NOT_FOUND", "no durable acceptance for request_id");
    return describe(store_.directory(), *found, active);
}
Json SessionStore::lookup_request(const fs::path &directory, const std::string &id) {
    auto found = find_request(directory, id);
    if (!found)
        throw SessionError("REQUEST_NOT_FOUND", "no durable acceptance for request_id");
    return describe(directory, *found, nullptr);
}
void SessionStore::finish(const Json &command) {
    auto id = command.at("command_id").get<std::string>();
    validate_command_id(id);
    auto path = store_.directory() / id / "result.json";
    if (fs::exists(path)) {
        auto existing = recover_command(command, read(path));
        auto proposed = command;
        proposed["schema_version"] = 2;
        if (existing != proposed)
            throw StorageError("refusing to change confirmed command result");
        return;
    }
    write(id + "/result.json", command);
}
Json SessionStore::recover(Json session) {
    // Scan durable intents as well as the snapshot: a crash may precede snapshot publication.
    std::map<std::string, Json> commands;
    for (const auto &e : fs::directory_iterator(store_.directory())) {
        auto id = e.path().filename().string();
        if (id.rfind("cmd-", 0) != 0)
            continue;
        validate_command_id(id);
        if (fs::exists(e.path() / "request.json")) {
            auto record = read(e.path() / "request.json");
            if (record.at("command").at("command_id") != id)
                throw StorageError("command record identity mismatch");
            commands[id] = record.at("command");
        }
    }
    if (session.contains("current") && session.at("current").is_object()) {
        auto command = session.at("current");
        auto id = command.at("command_id").get<std::string>();
        validate_command_id(id);
        if (commands.contains(id))
            commands[id].update(command);
        else
            commands[id] = command;
    }
    for (auto &[id, command] : commands) {
        auto path = store_.directory() / id / "result.json";
        std::optional<Json> saved;
        if (fs::exists(path))
            saved = read(path);
        command = recover_command(command, saved);
        if (!saved) {
            command["outcome_known"] = false;
            write(id + "/result.json", command);
        }
        if (session.value("current", Json(nullptr)).is_object() &&
            session["current"]["command_id"] == id)
            session["last"] = command;
    }
    session["current"] = nullptr;
    return session;
}
void SessionStore::close(Json session) {
    snapshot(session);
    write("result.json", session);
    store_.state(RunState::finished);
}
} // namespace sandbox
