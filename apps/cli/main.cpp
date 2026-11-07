#include "sandbox/runtime/batch.hpp"
#include "sandbox/runtime/interactive.hpp"
#include "sandbox/storage/artifacts.hpp"
#include "sandbox/application/service.hpp"
#include "sandbox/runtime/session_protocol.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>
namespace {
void help() {
    std::cout
        << "Linux Sandbox 0.5\n"
           "  sandbox run --config FILE [--state-root DIR] [--launcher PATH] [--allow-unsafe]\n"
           "  sandbox run [--timeout MS] [--memory BYTES] [--input DIR] [--output DIR] [--quiet] \\\n"
           "              [--runtime DIR] [--cgroup-root DIR] [--process] [--summary] -- COMMAND...\n"
           "  sandbox validate --config FILE [--state-root DIR] [--allow-unsafe]\n"
           "  sandbox session-open --config FILE [--state-root DIR] [--allow-unsafe]\n"
           "  sandbox session-request RUN_ID --request FILE [--state-root DIR]\n"
           "  sandbox files --request FILE\n"
           "  sandbox batch --config FILE [--state-root DIR] [--allow-unsafe]\n"
           "  sandbox doctor [--cgroup-root DIR] [--state-root DIR]\n"
           "  sandbox inspect RUN_ID [--state-root DIR]\n"
           "  sandbox cancel RUN_ID [--state-root DIR]\n"
           "  sandbox cleanup [--state-root DIR]\n"
           "\n"
           "Default isolated mode fails closed. Diagnostic process mode requires explicit\n"
           "--allow-unsafe (or --process, which implies it).\n"
           "\n"
           "Sandbox exit codes: 0 success, 10 nonzero exit, 11 signaled, 12 startup failure,\n"
           "124 timeout, 125 cleanup incomplete, 130 cancelled, 2 usage/error.\n"
           "--quiet hides the live log on stderr; internal.log keeps it either way.\n"
           "--summary prints the result as six human lines instead of the full report.\n";
}
void command_help(const std::string &command) {
    const std::map<std::string, std::string> text{
        {"run", "sandbox run --config FILE | -- [options] -- COMMAND...\n"
                "  Runs one command and prints the report (or six human lines with --summary).\n"
                "  Quick form flags: --timeout MS, --memory BYTES[K|M|G], --input DIR, --output DIR,\n"
                "  --runtime DIR, --cgroup-root DIR, --process, --quiet, --summary.\n"
                "  Example: sandbox run --process --summary -- /bin/sh -c 'exit 3'"},
        {"validate", "sandbox validate --config FILE\n"
                     "  Parses and validates a config without running anything.\n"
                     "  Example: sandbox validate --config profiles/process-demo.json --allow-unsafe"},
        {"doctor", "sandbox doctor [--cgroup-root DIR]\n"
                   "  Reports which startup modes work here and where isolation fails."},
        {"inspect", "sandbox inspect RUN_ID\n  Prints the stored state and report of one run."},
        {"cancel", "sandbox cancel RUN_ID\n  Asks a running supervisor to stop; the run still reports."},
        {"cleanup", "sandbox cleanup\n"
                    "  Recovers resources of interrupted runs (cgroup, sockets, sessions).\n"
                    "  It never deletes a run directory - use prune for that."},
        {"prune", "sandbox prune [--keep N] [--older-than 7d] [--dry-run]\n"
                  "  Deletes finished run directories. The newest N are kept; --older-than also\n"
                  "  requires age (units: m/h/d). Running or unfinished runs are never removed.\n"
                  "  Example: sandbox prune --keep 20 --older-than 7d --dry-run"},
        {"batch", "sandbox batch --config FILE\n  Runs the tasks of a batch config (see profiles/)."},
        {"session-open", "sandbox session-open --config FILE\n"
                         "  Starts a persistent session and prints its run id and control socket."},
        {"session-request", "sandbox session-request RUN_ID --request FILE\n"
                            "  Sends one control request (run a command, status, cancel, close)."},
        {"files", "sandbox files --request FILE\n"
                  "  Snapshot, manifest or export a directory through the descriptor-based copier."},
    };
    const auto found = text.find(command);
    if (found == text.end()) {
        help();
        return;
    }
    std::cout << found->second << "\n";
}

std::uint64_t duration_ms(const std::string &text) {
    if (text.size() < 2)
        throw std::runtime_error("--older-than needs a number and a unit (m/h/d), e.g. 7d");
    const char unit = text.back();
    const std::uint64_t scale = unit == 'm' ? 60 * 1000ULL : unit == 'h' ? 60 * 60 * 1000ULL : unit == 'd' ? 24 * 60 * 60 * 1000ULL : 0;
    if (!scale)
        throw std::runtime_error("--older-than needs a number and a unit (m/h/d), e.g. 7d");
    std::uint64_t value = 0;
    for (std::size_t i = 0; i + 1 < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9')
            throw std::runtime_error("--older-than needs a number and a unit (m/h/d), e.g. 7d");
        if (value > (UINT64_MAX / scale - 9) / 10)
            throw std::runtime_error("--older-than is out of range");
        value = value * 10 + static_cast<std::uint64_t>(text[i] - '0');
    }
    return value * scale;
}

std::uint64_t bytes(const std::string &text) {
    std::size_t end = text.size();
    std::uint64_t scale = 1;
    if (end && (text.back() == 'K' || text.back() == 'M' || text.back() == 'G')) {
        scale = text.back() == 'K' ? 1024ULL : text.back() == 'M' ? 1024ULL * 1024 : 1024ULL * 1024 * 1024;
        --end;
    }
    if (!end)
        throw std::runtime_error("--memory needs a number, optionally with K/M/G");
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < end; ++i) {
        if (text[i] < '0' || text[i] > '9')
            throw std::runtime_error("--memory needs a number, optionally with K/M/G");
        if (value > (UINT64_MAX - 9) / 10)
            throw std::runtime_error("--memory is out of range");
        value = value * 10 + static_cast<std::uint64_t>(text[i] - '0');
    }
    if (value && value > UINT64_MAX / scale)
        throw std::runtime_error("--memory is out of range");
    return value * scale;
}
// One-shot convenience: a config file is still the supported interface, but typing twenty
// lines of JSON to run one command is a poor first experience, so this synthesises the same
// config and keeps every rule (unknown fields, limits, mounts) in the one validator.
sandbox::Json quick_config(const std::map<std::string, std::string> &options,
                           const std::vector<std::string> &argv, bool process_mode) {
    if (argv.empty())
        throw std::runtime_error("nothing to run after --");
    sandbox::Json config = {{"schema_version", 1},
                            {"mode", process_mode ? "process" : "isolated"},
                            {"network", process_mode ? "host" : "none"},
                            {"cwd", process_mode ? "/tmp" : "/work"},
                            {"argv", argv}};
    sandbox::Json limits = sandbox::Json::object();
    if (options.contains("--timeout"))
        limits["wall_ms"] = bytes(options.at("--timeout"));
    if (options.contains("--memory"))
        limits["memory_bytes"] = bytes(options.at("--memory"));
    if (!limits.empty())
        config["limits"] = limits;
    if (options.contains("--runtime"))
        config["runtime_root"] = sandbox::fs::absolute(options.at("--runtime")).string();
    if (options.contains("--cgroup-root"))
        config["cgroup_root"] = sandbox::fs::absolute(options.at("--cgroup-root")).string();
    sandbox::Json mounts = sandbox::Json::array();
    if (options.contains("--input"))
        mounts.push_back({{"source", sandbox::fs::absolute(options.at("--input")).string()},
                          {"target", "/input"},
                          {"readonly", true}});
    if (options.contains("--output"))
        mounts.push_back({{"source", sandbox::fs::absolute(options.at("--output")).string()},
                          {"target", "/output"},
                          {"readonly", false}});
    if (!mounts.empty())
        config["mounts"] = mounts;
    return config;
}
void print_summary(const sandbox::Json &report, int code) {
    const auto text = [](const sandbox::Json &value, const char *key, const std::string &fallback) {
        return value.contains(key) && value[key].is_string() ? value[key].get<std::string>() : fallback;
    };
    const auto number = [](const sandbox::Json &value, const char *key) {
        return value.contains(key) && value[key].is_number() ? value[key].get<std::uint64_t>() : 0ULL;
    };
    const auto stream = [&](const char *name) {
        const auto &stream = report.contains(name) ? report[name] : sandbox::Json::object();
        return std::to_string(number(stream, "saved_bytes")) + " bytes";
    };
    std::cout << "verdict:    " << text(report, "verdict", "?") << " (" << text(report, "summary", "?")
              << ")\n";
    std::cout << "exit code:  "
              << (report.contains("exit_code") && report["exit_code"].is_number()
                      ? std::to_string(report["exit_code"].get<int>())
                      : std::string("none"))
              << "\n";
    std::cout << "output:     stdout " << stream("stdout") << ", stderr " << stream("stderr") << "\n";
    const auto &workspace = report.contains("workspace") && report["workspace"].is_object()
                                ? report["workspace"]
                                : sandbox::Json::object();
    if (workspace.contains("export")) {
        const auto &exported = workspace["export"];
        std::cout << "artifacts:  " << (exported.value("ok", false) ? "delivered" : "NOT delivered")
                  << " (" << exported.value("entries", 0) << " entries, "
                  << exported.value("bytes", 0) << " bytes)";
        if (exported.contains("manifest") && exported["manifest"].is_string())
            std::cout << ", manifest " << exported["manifest"].get<std::string>();
        std::cout << "\n";
    }
    const auto &timing = report.contains("timing") ? report["timing"] : sandbox::Json::object();
    std::cout << "time:       " << number(timing, "total_ms") << " ms total ("
              << number(timing, "startup_ms") << " startup, " << number(timing, "execution_ms")
              << " execution)\n";
    std::cout << "run:        " << text(report, "run_id", "?") << "  (inspect with: sandbox inspect "
              << text(report, "run_id", "?") << ")\n";
    if (report.contains("errors") && !report["errors"].empty())
        std::cout << "errors:     " << report["errors"].size() << " reported; see the run report\n";
    std::cout << "exit:       " << code << " (0 means success)\n";
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc == 1 || std::string(argv[1]) == "--help") {
            help();
            return 0;
        }
        std::string command = argv[1], id;
        std::map<std::string, std::string> options;
        std::vector<std::string> passthrough;
        bool unsafe = false, process_mode = false, summary = false, quiet = false;
        bool want_help = false, dry_run = false;
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--") {
                for (++i; i < argc; ++i)
                    passthrough.emplace_back(argv[i]);
                break;
            }
            if (arg == "--help") {
                want_help = true;
                continue;
            }
            if (arg == "--dry-run") {
                dry_run = true;
                continue;
            }
            if (arg == "--allow-unsafe") {
                if (unsafe)
                    throw std::runtime_error("duplicate flag");
                unsafe = true;
                continue;
            }
            if (arg == "--process") {
                process_mode = true;
                unsafe = true;
                continue;
            }
            if (arg == "--quiet") {
                quiet = true;
                continue;
            }
            if (arg == "--summary") {
                summary = true;
                continue;
            }
            if (arg.rfind("--", 0) != 0) {
                if (!id.empty())
                    throw std::runtime_error("unexpected positional argument");
                id = arg;
                continue;
            }
            if (arg != "--config" && arg != "--state-root" && arg != "--launcher" &&
                arg != "--cgroup-root" && arg != "--request" && arg != "--timeout" &&
                arg != "--memory" && arg != "--input" && arg != "--output" && arg != "--runtime" &&
                arg != "--keep" && arg != "--older-than")
                throw std::runtime_error("unknown option " + arg);
            if (i + 1 == argc || options.contains(arg))
                throw std::runtime_error("missing/duplicate option " + arg);
            options[arg] = argv[++i];
        }
        const std::set<std::string> commands{
            "run",     "validate",     "doctor",          "inspect", "cancel",
            "cleanup", "session-open", "session-request", "files",   "batch",
            "prune",   "help"};
        if (!commands.contains(command))
            throw std::runtime_error("unknown command");
        if (command == "help") {
            command_help(id);
            return 0;
        }
        if (want_help) {
            command_help(command);
            return 0;
        }
        bool needs_id = command == "inspect" || command == "cancel" || command == "session-request";
        if (needs_id != !id.empty())
            throw std::runtime_error("unexpected or missing run ID");
        if (!passthrough.empty() && command != "run")
            throw std::runtime_error("-- COMMAND... is only supported by run");
        if ((command == "run" || command == "validate") && !options.contains("--config") &&
            (command != "run" || passthrough.empty()))
            throw std::runtime_error("--config required (or: run -- COMMAND...)");
        if (options.contains("--config") && !passthrough.empty())
            throw std::runtime_error("use either --config or -- COMMAND..., not both");
        auto self = sandbox::fs::canonical("/proc/self/exe");
        auto state = options.contains("--state-root")
                         ? sandbox::fs::path(options["--state-root"])
                         : sandbox::fs::path("/tmp/sbx-" + std::to_string(::getuid()));
        auto launcher = options.contains("--launcher")
                            ? sandbox::fs::absolute(options["--launcher"])
                            : self.parent_path() / "sandbox-launcher";
        std::string config_path = options.contains("--config") ? options.at("--config") : std::string();
        std::string synthetic;
        if (!passthrough.empty()) {
            // A temporary file keeps one validator: the same rules reject the same mistakes
            // whether the config came from the user or from the convenience flags.
            synthetic = (sandbox::fs::temp_directory_path() /
                         ("sbx-quick-" + std::to_string(::getpid()) + ".json"))
                            .string();
            std::ofstream out(synthetic, std::ios::trunc);
            out << quick_config(options, passthrough, process_mode).dump(2);
            out.close();
            if (!out)
                throw std::runtime_error("cannot write the generated config");
            config_path = synthetic;
        }
        sandbox::SandboxService app(state, launcher);
        sandbox::Json output;
        int code = 0;
        if (command == "batch") {
            if (!options.contains("--config"))
                throw std::runtime_error("--config required");
            auto path = sandbox::fs::absolute(options.at("--config"));
            auto spec = sandbox::BatchSpec::parse(sandbox::Json::parse(sandbox::read_file(path)),
                                                  path.parent_path(), unsafe);
            output = sandbox::run_batch(std::move(spec), state, self, launcher);
            code = output.at("summary") == "success" ? 0 : 10;
        } else if (command == "session-open") {
            if (!options.contains("--config"))
                throw std::runtime_error("--config required");
            auto config = sandbox::RunConfig::load(options.at("--config"));
            config.persistent = true;
            config.validate(unsafe);
            output = sandbox::serve_session(config, state, launcher);
            if (!output.at("errors").empty())
                code = 125;
        } else if (command == "session-request" || command == "files") {
            if (!options.contains("--request"))
                throw std::runtime_error("--request required");
            std::ifstream in(options.at("--request"));
            sandbox::Json request;
            in >> request;
            output = command == "files" ? sandbox::file_request(request, sandbox::WorkspacePolicy{})
                                        : sandbox::ControlServer::request(state, id, request);
        } else if (command == "run") {
            auto report = app.run(config_path, unsafe, quiet);
            if (!synthetic.empty())
                std::remove(synthetic.c_str());
            if (summary) {
                print_summary(report.json(), report.cli_code());
                return report.cli_code();
            }
            output = report.json();
            code = report.cli_code();
        } else if (command == "validate")
            output = app.validate(config_path, unsafe);
        else if (command == "doctor")
            output = app.doctor(options.contains("--cgroup-root")
                                    ? sandbox::fs::path(options["--cgroup-root"])
                                    : sandbox::fs::path{});
        else if (command == "prune") {
            std::size_t keep = 0;
            if (options.contains("--keep")) {
                try {
                    keep = static_cast<std::size_t>(std::stoull(options.at("--keep")));
                } catch (const std::exception &) {
                    throw std::runtime_error("--keep needs a nonnegative integer");
                }
            }
            const auto older = options.contains("--older-than") ? duration_ms(options.at("--older-than")) : 0ULL;
            output = app.prune(keep, older, dry_run);
        }
        else if (command == "inspect")
            output = app.inspect(id);
        else if (command == "cancel")
            output = app.cancel(id);
        else
            output = app.cleanup();
        if (!synthetic.empty())
            std::remove(synthetic.c_str());
        std::cout << output.dump(2, ' ', false, sandbox::Json::error_handler_t::replace) << '\n';
        return code;
    } catch (const sandbox::SessionError &e) {
        std::cerr << e.json().dump() << '\n';
        return 2;
    } catch (const std::exception &e) {
        std::cerr << "sandbox: " << e.what() << '\n';
        return 2;
    }
}
