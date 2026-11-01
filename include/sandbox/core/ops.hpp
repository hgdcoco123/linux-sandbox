#pragma once
// Typed session vocabulary.
//
// This replaces the `j["op"]`-style dispatch: a session operation is a closed
// variant, each alternative is a distinct type, and every field is either a
// validated business type or a bounded primitive. There is no JSON in this
// header and nothing here can represent "op is execute but argv is missing".
#include "sandbox/core/ids.hpp"
#include "sandbox/core/record.hpp"
#include "sandbox/core/request.hpp"
#include "sandbox/core/result.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace sandbox {

enum class Stream { standard_output, standard_error };
const char *stream_name(Stream) noexcept;

enum class SessionPhase { created, preparing, idle, running, draining, closing, closed, failed };
const char *session_phase_name(SessionPhase) noexcept;

// A single stdin write is bounded by the control message size, not by memory.
inline constexpr std::size_t max_stdin_chunk = 4096;

// ---------- requests ----------

struct Execute {
    ExecutionRequest request;
};
struct Status {};
struct Close {};
struct Cancel {
    // nullopt means "whatever is active now".
    std::optional<CommandIndex> command;
};
struct StdinWrite {
    CommandIndex command;
    std::string bytes;
};
struct StdinClose {
    CommandIndex command;
};
struct ReadOutput {
    CommandIndex command;
    Stream stream = Stream::standard_output;
    std::uint64_t offset = 0;
};
struct ReadResult {
    CommandIndex command;
};
struct RequestStatusQuery {
    RequestKey key;
};

struct WorkspaceSnapshot {
    std::string source; // host path, validated as absolute
    std::string name;   // sandbox-visible name
};
struct WorkspaceManifest {};
struct WorkspaceExport {
    std::string destination;
};
using WorkspaceOperation = std::variant<WorkspaceSnapshot, WorkspaceManifest, WorkspaceExport>;
struct Workspace {
    WorkspaceOperation operation;
};

using SessionOperation = std::variant<Execute, Status, Close, Cancel, StdinWrite, StdinClose,
                                      ReadOutput, ReadResult, RequestStatusQuery, Workspace>;

// Validated envelope around one operation. Construction is the only place that
// can reject a request, so the controller never re-checks field invariants.
class SessionRequest {
  public:
    [[nodiscard]] static SessionRequest create(SessionOperation operation);

    [[nodiscard]] const SessionOperation &operation() const noexcept { return operation_; }
    template <class T> [[nodiscard]] const T *as() const & noexcept { return std::get_if<T>(&operation_); }
    // A view into a temporary request would dangle; forbid it at compile time.
    template <class T> const T *as() const && = delete;

  private:
    explicit SessionRequest(SessionOperation operation) : operation_(std::move(operation)) {}
    SessionOperation operation_;
};

// ---------- replies ----------

struct Accepted {
    enum class Kind { cancel_requested, stdin_closed, close_requested };
    Kind kind = Kind::cancel_requested;
};

struct OutputChunk {
    std::uint64_t offset = 0;
    std::uint64_t next_offset = 0;
    std::string bytes;
    bool finished = false;
};

struct CommandStatus {
    CommandIndex command;
    bool done = false;
    // Present only once the command has a durable outcome.
    std::optional<ExecutionOutcome> outcome;
};

struct SessionStatus {
    SessionPhase state = SessionPhase::created;
    std::optional<CommandIndex> current;
    std::uint32_t completed_commands = 0;
};

struct FileEntry {
    std::string path;
    std::uint64_t size = 0;
    bool executable = false;
};

struct ManifestReply {
    std::vector<FileEntry> entries;
};

struct ResultReply {
    FinishedCommand command;
};

struct RequestStatusReply {
    RequestKey key;
    std::optional<CommandIndex> command;
};

// The durable answer for one command: the accepted intent always, the terminal
// record once one exists. Storage returns this and the wire layer decides how to
// say it - "still running", "no outcome observed" and "finished" are different
// answers built from the same two records, and the choice is a protocol question,
// not a storage one.
struct CommandReport {
    AcceptedCommand accepted;
    std::optional<FinishedCommand> finished;
};

struct ErrorReply {
    std::string code;
    std::string message;
    bool retryable = false;
};

using SessionReply = std::variant<Accepted, OutputChunk, CommandStatus, SessionStatus, ResultReply,
                                  RequestStatusReply, ManifestReply, ErrorReply>;

} // namespace sandbox
