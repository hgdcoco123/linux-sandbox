#pragma once
#include "sandbox/session_protocol.hpp"
#include "sandbox/command_model.hpp"
#include "sandbox/session_safety.hpp"
#include "sandbox/store.hpp"
namespace sandbox {
class StorageError : public SessionError {
  public:
    explicit StorageError(const std::string &message) : SessionError("STORAGE_FAILURE", message) {}
};
class SessionStore {
    RunStore &store_;

  public:
    explicit SessionStore(RunStore &store) : store_(store) {}
    void write(const std::string &, Json);
    void snapshot(Json value) {
        write("session.json", std::move(value));
    }
    void accept(const Json &request, const Json &command);
    std::optional<Json> replay(const Json &request, const Json &active) const;
    std::optional<Json> replay(const ExecuteRequest &request, const Json &active) const;
    Json request_status(const std::string &id, const Json &active = nullptr) const;
    Json result(const std::string &id) const;
    void finish(const Json &command);
    void close(Json session);
    Json recover(Json session);
    static Json read(const fs::path &path);
    static Json lookup_request(const fs::path &directory, const std::string &id);

  private:
    static std::optional<Json> find_request(const fs::path &, const std::string &);
    static Json describe(const fs::path &, const Json &, const Json &);
};
} // namespace sandbox
