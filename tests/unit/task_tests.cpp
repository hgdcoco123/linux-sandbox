#include "sandbox/model/command_model.hpp"
#include "sandbox/model/task.hpp"
#include <iostream>
#include <type_traits>
using namespace sandbox;
void require(bool value) {
    if (!value)
        throw std::runtime_error("task assertion failed");
}
template <class F> void rejects(F function) {
    try {
        function();
    } catch (const std::exception &) {
        return;
    }
    throw std::runtime_error("expected rejection");
}
int main() {
    try {
        static_assert(!std::is_convertible_v<RequestId, CommandId>);
        Json wire = {
            {"op", "execute"}, {"argv", {"/bin/true"}}, {"request_id", "build-1"}, {"wall_ms", 50}};
        auto request = ExecuteRequest::parse(wire);
        require(request.json() == wire && request.timeout->count() == 50);
        wire["wall_ms"] = 0;
        rejects([&] { ExecuteRequest::parse(wire); });
        wire["wall_ms"] = 1.5;
        rejects([&] { ExecuteRequest::parse(wire); });
        rejects([] { RequestId::parse("../bad"); });
        rejects([] { CommandId::parse("cmd-x"); });
        RunConfig config;
        config.argv = {"/bin/true"};
        BatchSpec batch{2, {300, 30}, {}};
        for (unsigned n : {100, 250, 50}) {
            config.limits.memory_bytes = n;
            config.limits.pids = 10;
            batch.tasks.push_back({std::to_string(n), config});
        }
        Scheduler scheduler(batch);
        require(scheduler.admit() == 0);
        require(!scheduler.admit()); // FIFO head cannot fit; do not bypass it with the small task
        scheduler.running(0);
        scheduler.finishing(0);
        require(scheduler.resources().used().memory == 100); // hold reservation during finishing
        scheduler.complete(0, TaskState::completed);
        require(scheduler.admit() == 1 && scheduler.admit() == 2);
        require(!scheduler.admit());
        scheduler.complete(1, TaskState::failed);
        scheduler.complete(2, TaskState::cancelled);
        require(scheduler.done() && scheduler.resources().active() == 0);
        rejects([&] { scheduler.complete(2, TaskState::cancelled); });
        Scheduler cancelled(batch);
        require(cancelled.cancel_queued(0));
        require(!cancelled.cancel_queued(0));
        require(cancelled.admit() == 1);
        std::cout
            << "typed commands, FIFO, admission, finishing and cancellation contracts passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
