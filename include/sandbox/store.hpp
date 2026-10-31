#pragma once
#include "sandbox/linux.hpp"
#include "sandbox/report.hpp"
#include <functional>
#include <memory>
namespace sandbox {
class ExecuteRequest;
struct WriteStdinRequest;
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
    void serve(const std::function<Json(const Json &)> &handler,
               const std::function<Json(const ExecuteRequest &)> &execute_handler = {},
               const std::function<Json(const WriteStdinRequest &)> &stdin_handler = {});
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
