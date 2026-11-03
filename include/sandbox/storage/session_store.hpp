#pragma once
#include "sandbox/core/ops.hpp"
#include "sandbox/core/request.hpp"
#include "sandbox/runtime/session_protocol.hpp"
#include "sandbox/storage/store.hpp"
namespace sandbox {
class StorageError : public SessionError {
  public:
    explicit StorageError(const std::string &message)
        : SessionError("STORAGE_FAILURE", message) {}
};
// What recovery learned about a session. `last` is the durable answer for the
// command the snapshot still called current, so the caller can render it in
// whatever vocabulary it speaks; storage does not know that vocabulary.
struct SessionRecovery {
    Json session;
    std::optional<CommandReport> last;
};
// Durable per-command storage.
//
// The interface speaks business types; JSON exists only at the file boundary and
// is produced by core/record_json. Two record shapes are on disk, deliberately:
//
//   request.json   encode(AcceptedCommand)   - the intent, written before dispatch
//   result.json    encode(FinishedCommand)   - the terminal facts, written once
//
// Replies are not built here: a report goes to the wire layer, which knows how
// the protocol spells it. That keeps the storage format and the protocol free to
// version independently.
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
    // nullopt when no command carries this request key.
    std::optional<CommandReport> replay(const ExecutionRequest &) const;
    // Throws REQUEST_NOT_FOUND when nothing carries the key.
    CommandReport request_status(const RequestKey &) const;
    // nullopt while the command has no durable result yet.
    std::optional<CommandReport> result(const CommandIndex &) const;
    SessionRecovery recover(Json session);
    void close(Json session);
    static Json read(const fs::path &path);
    static CommandReport lookup_request(const fs::path &directory, const RequestKey &);

  private:
    // Both return nullopt only when the file is absent. A file that exists but
    // cannot be decoded is a StorageError: it is data to report, not to skip.
    static std::optional<AcceptedCommand> accepted_record(const fs::path &directory);
    static std::optional<FinishedCommand> finished_record(const fs::path &directory);
    static std::optional<AcceptedCommand> find_request(const fs::path &, const RequestKey &);
};
} // namespace sandbox
