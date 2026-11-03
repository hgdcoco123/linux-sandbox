#include "sandbox/model/task.hpp"
#include <set>
namespace sandbox {
const char *task_state_name(TaskState state) {
    switch (state) {
    case TaskState::queued:
        return "queued";
    case TaskState::starting:
        return "starting";
    case TaskState::running:
        return "running";
    case TaskState::finishing:
        return "finishing";
    case TaskState::completed:
        return "completed";
    case TaskState::cancelled:
        return "cancelled";
    case TaskState::failed:
        return "failed";
    }
    throw std::logic_error("invalid task state");
}
ResourceBudget::ResourceBudget(std::size_t slots, ResourceClaim maximum)
    : slots_(slots), maximum_(maximum) {
    if (!slots || !maximum.memory || !maximum.pids)
        throw std::invalid_argument("positive resource budget required");
}
bool ResourceBudget::fits(ResourceClaim c) const {
    return active_ < slots_ && c.memory <= maximum_.memory - used_.memory &&
           c.pids <= maximum_.pids - used_.pids;
}
void ResourceBudget::reserve(ResourceClaim c) {
    if (!fits(c))
        throw std::logic_error("resource budget exceeded");
    ++active_;
    used_.memory += c.memory;
    used_.pids += c.pids;
}
void ResourceBudget::release(ResourceClaim c) {
    if (!active_ || c.memory > used_.memory || c.pids > used_.pids)
        throw std::logic_error("invalid resource release");
    --active_;
    used_.memory -= c.memory;
    used_.pids -= c.pids;
}
BatchSpec BatchSpec::parse(const Json &j, const fs::path &base, bool unsafe) {
    require_keys(j,
                 {"schema_version", "max_parallel", "memory_budget_bytes", "pids_budget", "tasks"},
                 "batch");
    if (j.at("schema_version") != 1)
        throw std::invalid_argument("batch schema_version must be 1");
    auto positive = [&](const char *key, std::uint64_t max) {
        const auto &n = j.at(key);
        if (!n.is_number_integer() || n <= 0 || n > max)
            throw std::invalid_argument(std::string("invalid batch ") + key);
        return n.get<std::uint64_t>();
    };
    BatchSpec spec{
        static_cast<std::size_t>(positive("max_parallel", 32)),
        {positive("memory_budget_bytes", 1ULL << 50), positive("pids_budget", 1ULL << 30)},
        {}};
    const auto &tasks = j.at("tasks");
    if (!tasks.is_array() || tasks.empty() || tasks.size() > 256)
        throw std::invalid_argument("batch requires 1..256 tasks");
    std::set<std::string> ids;
    for (const auto &task : tasks) {
        require_keys(task, {"id", "config"}, "batch task");
        auto id = task.at("id").get<std::string>();
        if (id.empty() || id.size() > 32 ||
            id.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
                std::string::npos ||
            !ids.insert(id).second)
            throw std::invalid_argument("invalid or duplicate task id");
        auto config = RunConfig::parse(task.at("config"), base);
        config.validate(unsafe);
        if (config.persistent)
            throw std::invalid_argument("batch tasks are independent one-shot runs");
        if (!spec.tasks.empty() && config.mode != spec.tasks.front().config.mode)
            throw std::invalid_argument("batch tasks must share execution mode");
        if (config.limits.memory_bytes > spec.budget.memory ||
            config.limits.pids > spec.budget.pids)
            throw std::invalid_argument("task cannot fit batch resource budget");
        spec.tasks.push_back({std::move(id), std::move(config)});
    }
    return spec;
}
Scheduler::Scheduler(const BatchSpec &spec) : resources_(spec.max_parallel, spec.budget) {
    if (spec.tasks.empty() || spec.tasks.size() > 256 || spec.max_parallel > 32)
        throw std::invalid_argument("invalid scheduler batch size");
    for (std::size_t i = 0; i < spec.tasks.size(); ++i) {
        auto claim = spec.tasks[i].claim();
        if (!claim.memory || !claim.pids || claim.memory > spec.budget.memory || claim.pids > spec.budget.pids)
            throw std::invalid_argument("unschedulable task claim");
        claims_.push_back(spec.tasks[i].claim());
        states_.push_back(TaskState::queued);
        waiting_.push_back(i);
    }
}
std::optional<std::size_t> Scheduler::admit() {
    while (!waiting_.empty() && states_[waiting_.front()] != TaskState::queued)
        waiting_.pop_front();
    if (waiting_.empty() || !resources_.fits(claims_[waiting_.front()]))
        return std::nullopt;
    auto index = waiting_.front();
    waiting_.pop_front();
    resources_.reserve(claims_[index]);
    states_[index] = TaskState::starting;
    return index;
}
bool Scheduler::cancel_queued(std::size_t index) {
    if (states_.at(index) != TaskState::queued)
        return false;
    states_[index] = TaskState::cancelled;
    return true;
}
void Scheduler::running(std::size_t index) {
    if (states_.at(index) != TaskState::starting)
        throw std::logic_error("invalid task start transition");
    states_[index] = TaskState::running;
}
void Scheduler::finishing(std::size_t index) {
    if (states_.at(index) != TaskState::running && states_[index] != TaskState::starting)
        throw std::logic_error("invalid task finish transition");
    states_[index] = TaskState::finishing;
}
void Scheduler::complete(std::size_t index, TaskState terminal) {
    auto before = states_.at(index);
    if ((before != TaskState::starting && before != TaskState::running &&
         before != TaskState::finishing) ||
        (terminal != TaskState::completed && terminal != TaskState::cancelled &&
         terminal != TaskState::failed))
        throw std::logic_error("invalid task completion");
    resources_.release(claims_[index]);
    states_[index] = terminal;
}
bool Scheduler::done() const {
    for (auto state : states_)
        if (state == TaskState::queued || state == TaskState::starting ||
            state == TaskState::running || state == TaskState::finishing)
            return false;
    return true;
}
} // namespace sandbox
