#pragma once
#include "sandbox/config.hpp"
#include <deque>
#include <optional>
namespace sandbox {
enum class TaskState { queued, starting, running, finishing, completed, cancelled, failed };
const char *task_state_name(TaskState);
struct ResourceClaim {
    std::uint64_t memory, pids;
};
class ResourceBudget {
    std::size_t slots_, active_ = 0;
    ResourceClaim maximum_, used_{0, 0};

  public:
    ResourceBudget(std::size_t slots, ResourceClaim maximum);
    bool fits(ResourceClaim) const;
    void reserve(ResourceClaim);
    void release(ResourceClaim);
    std::size_t active() const {
        return active_;
    }
    ResourceClaim used() const {
        return used_;
    }
};
struct TaskSpec {
    std::string id;
    RunConfig config;
    ResourceClaim claim() const {
        return {config.limits.memory_bytes, config.limits.pids};
    }
};
struct BatchSpec {
    std::size_t max_parallel;
    ResourceClaim budget;
    std::vector<TaskSpec> tasks;
    static BatchSpec parse(const Json &, const fs::path &base, bool allow_process);
};
class Scheduler {
    ResourceBudget resources_;
    std::vector<ResourceClaim> claims_;
    std::vector<TaskState> states_;
    std::deque<std::size_t> waiting_;

  public:
    explicit Scheduler(const BatchSpec &);
    std::optional<std::size_t> admit();
    bool cancel_queued(std::size_t);
    void running(std::size_t);
    void finishing(std::size_t);
    void complete(std::size_t, TaskState terminal);
    TaskState state(std::size_t index) const {
        return states_.at(index);
    }
    bool done() const;
    const ResourceBudget &resources() const {
        return resources_;
    }
};
} // namespace sandbox
