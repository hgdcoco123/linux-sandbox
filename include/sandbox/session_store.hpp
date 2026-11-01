#pragma once
#include "sandbox/core/record.hpp"
#include "sandbox/core/request.hpp"
#include "sandbox/session_protocol.hpp"
#include "sandbox/store.hpp"
namespace sandbox {
class StorageError : public SessionError {
  public:
    explicit StorageError(const std::string &message)
        : SessionError("STORAGE_FAILURE", message) {}
};
// Durable per-command storage.
//
// The interface speaks business types; JSON exists only at the file boundary and
// is produced by core/record_json. Two shapes are on disk, deliberately:
//
//   request.json   encode(AcceptedCommand)   - the intent, written before dispatch
//   result.json    encode(FinishedCommand)   - the terminal facts, written once
//
// Replies are still the session JSON the wire has always used, because the wire
// has its own version and the SDK reads `result.json` directly. Translating a
// record into that reply happens in this file and nowhere else, so the storage
// format can change without the protocol changing with it.
class SessionStore {
    RunStore &store_;

  public:
    explicit SessionStore(RunStore &store) : store_(store) {}
    void write(const std::string &, Json);
    void snapshot(Json value) { write("session.json", std::move(value)); }
    // Acceptance precedes dispatch and is never withdrawn: a retry must resolve
    // this command, not start a second one.
    void accept(const AcceptedCommand &);
    // Completion is written once. A confirmed result is never rewritten, not even
    // to a "more accurate" one.
    void finish(const FinishedCommand &);
    std::optional<Json> replay(const ExecutionRequest &, const Json &active) const;
    Json request_status(const RequestKey &, const Json &active = nullptr) const;
    Json result(const CommandIndex &) const;
    Json recover(Json session);
    void close(Json session);
    static Json read(const fs::path &path);
    static Json lookup_request(const fs::path &directory, const RequestKey &);

  private:
    // Both return nullopt only when the file is absent. A file that exists but
    // cannot be decoded is a StorageError: it is data to report, not to skip.
    static std::optional<AcceptedCommand> accepted_record(const fs::path &directory);
    static std::optional<FinishedCommand> finished_record(const fs::path &directory);
    static std::optional<AcceptedCommand> find_request(const fs::path &, const RequestKey &);
};
} // namespace sandbox
