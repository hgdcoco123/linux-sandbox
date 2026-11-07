#include "sandbox/model/config.hpp"
#include "sandbox/model/report.hpp"
#include <filesystem>
#include <fstream>
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
// Shipped profiles are user-facing fixtures: a config schema change that breaks
// one of them must fail the build, not the user's first run.
int validate_profiles(const fs::path &directory) {
    int failures = 0;
    for (const auto &entry : fs::directory_iterator(directory)) {
        if (entry.path().extension() != ".json")
            continue;
        Json document;
        {
            std::ifstream in(entry.path());
            in >> document;
        }
        if (document.contains("tasks"))
            continue; // batch manifest, not a single run config
        try {
            // Schema only: the profiles point at host paths that exist on the
            // deployment machine, not here, so validate() would report missing
            // directories rather than config drift.
            auto config = RunConfig::load(entry.path());
            (void)config;
        } catch (const std::exception &error) {
            std::printf("FAIL profile %s: %s\n", entry.path().filename().string().c_str(),
                        error.what());
            ++failures;
        }
    }
    return failures;
}
int main(int argc, char **argv) {
    if (argc > 1) {
        int profile_failures = validate_profiles(argv[1]);
        if (profile_failures != 0) {
            std::printf("%d profile(s) failed to load\n", profile_failures);
            return 1;
        }
        std::printf("profiles loaded: %s\n", argv[1]);
        return 0;
    }
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
        // A near miss names the field it probably meant, instead of only rejecting the typo.
        auto near = j;
        near["limit"] = Json::object();
        bool suggested = false;
        try {
            RunConfig::parse(near, ".");
        } catch (const std::exception &error) {
            suggested = std::string(error.what()).find("did you mean 'limits'") != std::string::npos;
        }
        require(suggested, "an unknown field suggests the closest legal name");
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
        // The workspace boundary is configuration, and bad policies are refused
        // rather than silently clamped.
        auto with_workspace = j;
        with_workspace["workspace"] = {{"max_bytes", 1048576},
                                       {"max_entries", 128},
                                       {"max_depth", 8},
                                       {"input", "snapshot_copy"},
                                       {"keep_on_close", true}};
        auto parsed_workspace = RunConfig::parse(with_workspace, fs::current_path());
        parsed_workspace.validate(true);
        require(parsed_workspace.workspace.max_bytes == 1048576, "workspace max_bytes");
        require(parsed_workspace.workspace.input == InputMode::snapshot_copy, "workspace input mode");
        require(parsed_workspace.workspace.keep_on_close, "workspace retention");
        {
            auto round = RunConfig::parse(parsed_workspace.to_json(), fs::current_path());
            require(round.workspace.max_entries == 128, "workspace round-trips");
        }
        {
            auto typo = with_workspace;
            typo["workspace"]["max_size"] = 1;
            rejects([&] { RunConfig::parse(typo, fs::current_path()); });
        }
        {
            auto bad_mode = with_workspace;
            bad_mode["workspace"]["input"] = "copy";
            rejects([&] { RunConfig::parse(bad_mode, fs::current_path()); });
        }
        {
            auto zero = with_workspace;
            zero["workspace"]["max_bytes"] = 0;
            rejects([&] {
                auto config = RunConfig::parse(zero, fs::current_path());
                config.validate(true);
            });
        }
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
        RunReport dirty_cleanup;
        dirty_cleanup.exit_code = 0;
        dirty_cleanup.cleanup_errors.push_back("cgroup still populated");
        dirty_cleanup.conclude();
        require(dirty_cleanup.summary == "success",
                "a cleanup failure must not rewrite the target result");
        dirty_cleanup.verdict = "cleanup_incomplete";
        require(dirty_cleanup.json()["verdict"] == "cleanup_incomplete",
                "the verdict is reported alongside the summary");
        require(!dirty_cleanup.cleanup_errors.empty(), "the cleanup failure stays reported");
        require(dirty_cleanup.cli_code() == 125,
                "a dirty cleanup keeps the documented 125 exit status");
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
