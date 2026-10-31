#include "sandbox/workspace_service.hpp"
#include "sandbox/core/ops_json.hpp"
#include "sandbox/interactive.hpp"
#include <variant>

namespace sandbox {
Json WorkspaceService::handle(const Json &request) {
    auto publish = [&](const Json &request) {
        auto value = file_request(request, policy_);
        // Keep complete manifests on disk; bound the control response independently.
        store_.write("artifacts.json", value);
        auto count = value.at("entries").size();
        if (count > 24) {
            auto entries = value.at("entries");
            value["entries"] = Json::array();
            for (std::size_t i = 0; i < 24; ++i)
                value["entries"].push_back(entries[i]);
        }
        value["entry_count"] = count;
        value["manifest_truncated"] = count > 24;
        while (value.dump().size() > 12000 && !value["entries"].empty()) {
            value["entries"].erase(value["entries"].end() - 1);
            value["manifest_truncated"] = true;
        }
        return value;
    };
    auto req = request;
    auto action = req.at("op").get<std::string>();
    auto workspace = workspace_;
    if (action == "snapshot") {
        require_keys(req, {"op", "source", "name"}, "session snapshot");
        // Upload a complete, new named directory; never overwrite live files.
        auto name = req.at("name").get<std::string>();
        if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos ||
            name.find('\\') != std::string::npos || name.find('\0') != std::string::npos)
            throw std::runtime_error("upload name must be a single directory component");
        return publish({{"op", "snapshot"},
                        {"source", req.at("source")},
                        {"destination", (workspace / name).string()}});
    }
    if (action == "manifest") {
        require_keys(req, {"op"}, "session manifest");
        return publish({{"op", action}, {"source", workspace.string()}});
    }
    if (action == "export") {
        require_keys(req, {"op", "destination"}, "session export");
        return publish({{"op", action},
                        {"source", workspace.string()},
                        {"destination", req.at("destination")}});
    }
    throw SessionError("INVALID_REQUEST", "unknown session file operation");
}
Json WorkspaceService::handle(const Workspace &operation) {
    Json request;
    std::visit(
        [&request](const auto &value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, WorkspaceSnapshot>) {
                request = {{"op", "snapshot"}, {"source", value.source}, {"name", value.name}};
            } else if constexpr (std::is_same_v<T, WorkspaceManifest>) {
                request = {{"op", "manifest"}};
            } else {
                request = {{"op", "export"}, {"destination", value.destination}};
            }
        },
        operation.operation);
    return handle(request);
}
} // namespace sandbox
