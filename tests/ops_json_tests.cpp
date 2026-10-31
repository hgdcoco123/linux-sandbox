// Portable tests for the JSON -> typed session request adapter.
#include "sandbox/core/ops_json.hpp"

#include <cstdio>
#include <string>

using namespace sandbox;
using Json = nlohmann::json;

namespace {
int failures = 0;

void check(bool condition, const char *what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

SessionRequest accept(const Json &request, const char *what) {
    std::string error;
    auto parsed = session_request_from_json(request, error);
    if (!parsed) {
        std::printf("FAIL: %s was rejected: %s\n", what, error.c_str());
        ++failures;
        return SessionRequest::create(Status{});
    }
    return *parsed;
}

void reject(const Json &request, const char *what) {
    std::string error;
    if (session_request_from_json(request, error)) {
        std::printf("FAIL: %s was accepted\n", what);
        ++failures;
        return;
    }
    if (error.empty()) {
        std::printf("FAIL: %s produced no reason\n", what);
        ++failures;
    }
}

void test_execute() {
    auto minimal = accept(Json{{"op", "execute"}, {"argv", {"/bin/echo", "hi"}}}, "execute minimal");
    const auto *execute = minimal.as<Execute>();
    check(execute != nullptr, "minimal maps to Execute");
    if (execute) {
        check(execute->request.argv().size() == 2, "argv parsed");
        check(!execute->request.cwd().has_value(), "cwd stays absent");
        check(!execute->request.key().has_value(), "request key stays absent");
    }

    auto full = accept(Json{{"op", "execute"},
                            {"argv", {"/bin/sh", "-c", "echo hi"}},
                            {"cwd", "/work"},
                            {"env", {{"LANG", "C.UTF-8"}}},
                            {"wall_ms", 1500},
                            {"request_id", "req-1"}},
                       "execute full");
    const auto *rich = full.as<Execute>();
    check(rich != nullptr, "full maps to Execute");
    if (rich) {
        check(rich->request.cwd() && rich->request.cwd()->string() == "/work", "cwd parsed");
        check(rich->request.env() && rich->request.env()->at("LANG") == "C.UTF-8", "env parsed");
        check(rich->request.timeout() == std::chrono::milliseconds(1500), "timeout parsed");
        check(rich->request.key() && rich->request.key()->str() == "req-1", "request key parsed");
    }

    reject(Json{{"op", "execute"}, {"argv", {"relative"}}}, "relative executable");
    reject(Json{{"op", "execute"}}, "execute without argv");
    reject(Json{{"op", "execute"}, {"argv", {"/bin/true"}}, {"cwd2", "/work"}},
           "unknown execute field");
    reject(Json{{"op", "execute"}, {"argv", {"/bin/true"}}, {"env", "not-an-object"}},
           "env of the wrong type");
    reject(Json{{"op", "execute"}, {"argv", {"/bin/true"}}, {"request_id", "has space"}},
           "malformed request_id");
}

void test_control_ops() {
    auto status_request = accept(Json{{"op", "status"}}, "status");
    check(status_request.as<Status>() != nullptr, "status");
    auto close_request = accept(Json{{"op", "close"}}, "close");
    check(close_request.as<Close>() != nullptr, "close");

    auto cancel_request = accept(Json{{"op", "cancel"}}, "cancel all");
    const auto *cancel_all = cancel_request.as<Cancel>();
    check(cancel_all && !cancel_all->command.has_value(), "cancel without a target");
    auto cancel_one_request = accept(Json{{"op", "cancel"}, {"command_id", "cmd-7"}}, "cancel one");
    const auto *cancel_one = cancel_one_request.as<Cancel>();
    check(cancel_one && cancel_one->command && cancel_one->command->number() == 7,
          "cancel targets a command");

    auto write_request =
        accept(Json{{"op", "stdin"}, {"command_id", "cmd-1"}, {"bytes", {104, 105}}}, "stdin");
    const auto *write = write_request.as<StdinWrite>();
    check(write && write->bytes == "hi", "stdin bytes decoded as octets");
    reject(Json{{"op", "stdin"}, {"command_id", "cmd-1"}, {"bytes", {256}}}, "byte out of range");
    reject(Json{{"op", "stdin"}, {"command_id", "cmd-1"}, {"bytes", "hi"}}, "bytes not an array");
    reject(Json{{"op", "stdin"}, {"command_id", "cmd-0"}, {"bytes", {}}}, "invalid command index");

    auto output_request =
        accept(Json{{"op", "output"}, {"command_id", "cmd-2"}, {"offset", 4096}}, "output");
    const auto *output = output_request.as<ReadOutput>();
    check(output && output->stream == Stream::standard_output && output->offset == 4096,
          "output defaults to stdout with the given offset");
    auto stderr_request =
        accept(Json{{"op", "output"}, {"command_id", "cmd-2"}, {"stream", "stderr"}}, "stderr");
    const auto *errors = stderr_request.as<ReadOutput>();
    check(errors && errors->stream == Stream::standard_error, "stderr stream");
    reject(Json{{"op", "output"}, {"command_id", "cmd-2"}, {"stream", "stdin"}}, "unknown stream");

    auto status_query_request =
        accept(Json{{"op", "request-status"}, {"request_id", "abc"}}, "request-status");
    const auto *status = status_query_request.as<RequestStatusQuery>();
    check(status && status->key.str() == "abc", "request status key");
}

void test_workspace_ops() {
    auto manifest_request = accept(Json{{"op", "files"}, {"request", {{"op", "manifest"}}}},
                                  "manifest");
    const auto *manifest = manifest_request.as<Workspace>();
    check(manifest && std::holds_alternative<WorkspaceManifest>(manifest->operation),
          "manifest operation");

    auto snapshot_request =
        accept(Json{{"op", "files"},
                    {"request", {{"op", "snapshot"}, {"source", "/tmp/src"}, {"name", "snap"}}}},
               "snapshot");
    const auto *snapshot = snapshot_request.as<Workspace>();
    check(snapshot && std::holds_alternative<WorkspaceSnapshot>(snapshot->operation),
          "snapshot operation");
    reject(Json{{"op", "files"},
                {"request", {{"op", "snapshot"}, {"source", "relative"}, {"name", "snap"}}}},
           "relative snapshot source");
    reject(Json{{"op", "files"}, {"request", {{"op", "export"}}}}, "export without destination");
    reject(Json{{"op", "files"}, {"request", {{"op", "teleport"}}}}, "unknown file operation");
    reject(Json{{"op", "files"}, {"request", "nope"}}, "files without a request object");
}

void test_rejections() {
    reject(Json::array(), "a non-object request");
    reject(Json{{"argv", {"/bin/true"}}}, "a request without op");
    reject(Json{{"op", 42}}, "a numeric op");
    reject(Json{{"op", "teleport"}}, "an unknown op");
    reject(Json{{"op", "status"}, {"command_id", "cmd-1"}}, "status with a stray field");
}
} // namespace

int main() {
    test_execute();
    test_control_ops();
    test_workspace_ops();
    test_rejections();
    if (failures != 0) {
        std::printf("%d adapter check(s) failed\n", failures);
        return 1;
    }
    std::printf("json session request adapter passed\n");
    return 0;
}
