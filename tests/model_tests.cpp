#include "sandbox/config.hpp"
#include "sandbox/report.hpp"
#include <iostream>
#include <stdexcept>
using namespace sandbox;
void require(bool c, const char *message) {
    if (!c)
        throw std::runtime_error(message);
}
template <class F> void rejects(F f) {
    bool threw = false;
    try {
        f();
    } catch (const std::exception &) {
        threw = true;
    }
    require(threw, "expected rejection");
}
int main() {
    try {
        Json j = {{"schema_version", 1},
                  {"argv", {"/bin/true"}},
                  {"cwd", "/"},
                  {"mode", "process"},
                  {"network", "host"}};
        auto c = RunConfig::parse(j, fs::current_path());
        c.validate(true);
        rejects([&] { c.validate(false); });
        auto unknown = j;
        unknown["typo"] = 1;
        rejects([&] { RunConfig::parse(unknown, "."); });
        auto negative = j;
        negative["limits"] = {{"wall_ms", -1}};
        rejects([&] { RunConfig::parse(negative, "."); });
        auto wrong = j;
        wrong["limits"] = {{"wall_ms", 1.2}};
        rejects([&] { RunConfig::parse(wrong, "."); });
        auto zero = j;
        zero["limits"] = {{"wall_ms", 0}};
        rejects([&] {
            auto v = RunConfig::parse(zero, ".");
            v.validate(true);
        });
        auto round = RunConfig::parse(c.to_json(), fs::current_path());
        require(round.argv == c.argv, "config roundtrip");
        auto sampled = j;
        sampled["metrics"] = {{"interval_ms", 500}, {"history_limit", 8}};
        auto sampling = RunConfig::parse(sampled, fs::current_path());
        sampling.validate(true);
        require(sampling.to_json()["metrics"]["interval_ms"] == 500, "sampling config roundtrip");
        for (auto bad_interval : {0, 49, 10001}) {
            auto bad = j;
            bad["metrics"] = {{"interval_ms", bad_interval}};
            rejects([&] {
                auto config = RunConfig::parse(bad, fs::current_path());
                config.validate(true);
            });
        }
        for (auto bad_history : {0, 1025}) {
            auto bad = j;
            bad["metrics"] = {{"history_limit", bad_history}};
            rejects([&] {
                auto config = RunConfig::parse(bad, fs::current_path());
                config.validate(true);
            });
        }
        auto typo = j;
        typo["metrics"] = {{"intervall_ms", 500}};
        rejects([&] { RunConfig::parse(typo, fs::current_path()); });
        StateMachine state;
        rejects([&] { state.transition(RunState::running); });
        for (auto s : {RunState::preparing, RunState::starting, RunState::running,
                       RunState::stopping, RunState::finalizing, RunState::finished})
            state.transition(s);
        rejects([&] { state.transition(RunState::running); });
        RunReport report;
        require(report.json()["exit_code"].is_null(), "missing exit must be null");
        report.exit_code = 0;
        report.conclude();
        require(report.summary == "success" && report.cli_code() == 0, "success");
        report.stop_reason = "timeout";
        report.conclude();
        require(report.summary == "timeout" && report.cli_code() == 124, "timeout");
        report.exec_error = "ENOENT";
        report.conclude();
        require(report.summary == "startup_failure", "startup precedence");
        report.errors.push_back("disk full");
        report.conclude();
        require(report.summary == "host_error", "host precedence");
        report.id = "quote\"slash\\newline\n中文";
        auto serialized = report.json().dump();
        require(Json::parse(serialized)["run_id"] == report.id, "JSON escaping and UTF8");
        std::cout << "model tests passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
