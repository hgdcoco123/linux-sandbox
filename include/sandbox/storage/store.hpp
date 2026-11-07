#pragma once
#include "sandbox/core/ops.hpp"
#include "sandbox/platform/linux.hpp"
#include "sandbox/model/report.hpp"
#include <functional>
#include <memory>
namespace sandbox {
// Hash the files a run (or a session command) leaves behind, so a consumer can prove the
// bytes it cites are the bytes this run wrote. Absent files are skipped; the result is
// {schema_version, run_id, algorithm:"sha256", files:{name:{sha256,size}}}.
Json write_checksums(const fs::path &directory, const std::string &run_id);

class RunStore {
  public:
    static std::unique_ptr<RunStore> create(const fs::path &root, const RunConfig &);
    const std::string &id() const {
        return id_;
    }
    const fs::path &directory() const {
        return directory_;
    }
    int directory_fd() const {
        return directory_fd_.get();
    }
    void state(RunState, const Json &extra = Json::object());
    void report(const RunReport &);
    void owner_update(const Json &);
    void write_json(const std::string &name, const Json &);
    // Hashes what the run left on disk so a consumer can prove the bytes it cites are
    // the bytes this run wrote. Call after the last log flush, or internal.log will differ.
    void checksums();
    static Json inspect(const fs::path &root, const std::string &id);
    static Json cleanup(const fs::path &root);
    static fs::path verified_run(const fs::path &root, const std::string &id);

  private:
    RunStore(fs::path directory, std::string id, UniqueFd lock, UniqueFd dir);
    fs::path directory_;
    std::string id_;
    UniqueFd lock_, directory_fd_;
    Json owner_;
};
class ControlServer {
  public:
    explicit ControlServer(const fs::path &path);
    ~ControlServer();
    int fd() const {
        return fd_.get();
    }
    bool accept_cancel();
    // Binary session traffic is decoded once into a typed request, so the handler
    // never sees JSON. JSON traffic is still accepted from clients that speak it:
    // it is decoded through the same typed front door, unless a protocol of its own
    // supplies `json_handler` (the batch control channel does).
    void serve(const std::function<Json(const SessionRequest &)> &handler,
               const std::function<Json(const Json &)> &json_handler = {});
    static Json request(const fs::path &, const std::string &, const Json &);
    static Json cancel(const fs::path &root, const std::string &id);

  private:
    fs::path path_;
    UniqueFd fd_;
    struct PendingClient {
        UniqueFd fd;
        std::uint64_t deadline;
    };
    std::vector<PendingClient> pending_;
};
} // namespace sandbox
