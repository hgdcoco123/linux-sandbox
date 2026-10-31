#include "sandbox/batch.hpp"
#include "sandbox/interactive.hpp"
#include "sandbox/service.hpp"
#include "sandbox/session_protocol.hpp"
#include <fstream>
#include <iostream>
#include <set>
#include <unistd.h>
namespace {
void help() {
    std::cout
        << "Linux Sandbox 0.5\n"
           "  sandbox session-open --config FILE [--state-root DIR] [--allow-unsafe]\n"
           "  sandbox session-request RUN_ID --request FILE [--state-root DIR]\n"
           "  sandbox files --request FILE\n"
           "  sandbox batch --config FILE [--state-root DIR] [--allow-unsafe]\n"
           "  sandbox run --config FILE [--state-root DIR] [--launcher PATH] [--allow-unsafe]\n"
           "  sandbox validate --config FILE [--state-root DIR] [--allow-unsafe]\n"
           "  sandbox doctor [--cgroup-root DIR] [--state-root DIR]\n"
           "  sandbox inspect RUN_ID [--state-root DIR]\n"
           "  sandbox cancel RUN_ID [--state-root DIR]\n"
           "  sandbox cleanup [--state-root DIR]\n"
           "Default isolated mode fails closed. Diagnostic process mode requires explicit "
           "--allow-unsafe.\n";
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
        bool unsafe = false;
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--allow-unsafe") {
                if (unsafe)
                    throw std::runtime_error("duplicate flag");
                unsafe = true;
                continue;
            }
            if (arg.rfind("--", 0) != 0) {
                if (!id.empty())
                    throw std::runtime_error("unexpected positional argument");
                id = arg;
                continue;
            }
            if (arg != "--config" && arg != "--state-root" && arg != "--launcher" &&
                arg != "--cgroup-root" && arg != "--request")
                throw std::runtime_error("unknown option " + arg);
            if (i + 1 == argc || options.contains(arg))
                throw std::runtime_error("missing/duplicate option " + arg);
            options[arg] = argv[++i];
        }
        const std::set<std::string> commands{
            "run",     "validate",     "doctor",          "inspect", "cancel",
            "cleanup", "session-open", "session-request", "files",   "batch"};
        if (!commands.contains(command))
            throw std::runtime_error("unknown command");
        bool needs_id = command == "inspect" || command == "cancel" || command == "session-request";
        if (needs_id != !id.empty())
            throw std::runtime_error("unexpected or missing run ID");
        if ((command == "run" || command == "validate") && !options.contains("--config"))
            throw std::runtime_error("--config required");
        auto self = sandbox::fs::canonical("/proc/self/exe");
        auto state = options.contains("--state-root")
                         ? sandbox::fs::path(options["--state-root"])
                         : sandbox::fs::path("/tmp/sbx-" + std::to_string(::getuid()));
        auto launcher = options.contains("--launcher")
                            ? sandbox::fs::absolute(options["--launcher"])
                            : self.parent_path() / "sandbox-launcher";
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
            auto report = app.run(options.at("--config"), unsafe);
            output = report.json();
            code = report.cli_code();
        } else if (command == "validate")
            output = app.validate(options.at("--config"), unsafe);
        else if (command == "doctor")
            output = app.doctor(options.contains("--cgroup-root")
                                    ? sandbox::fs::path(options["--cgroup-root"])
                                    : sandbox::fs::path{});
        else if (command == "inspect")
            output = app.inspect(id);
        else if (command == "cancel")
            output = app.cancel(id);
        else
            output = app.cleanup();
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
