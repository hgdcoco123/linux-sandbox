#include "sandbox/core/request.hpp"

namespace sandbox {
namespace {
std::string_view as_view(const std::string &text) noexcept {
    return {text.data(), text.size()};
}

// Portable UTF-8 view of a path. `native()` is wide on Windows, so validation
// must not depend on it.
std::string utf8_of(const std::filesystem::path &path) {
    auto text = path.u8string();
    return std::string(reinterpret_cast<const char *>(text.data()), text.size());
}

void reject_nul(std::string_view text, const char *what) {
    if (text.find('\0') != std::string_view::npos)
        throw RequestError("INVALID_REQUEST", std::string("NUL in ") + what);
}

bool is_absolute(const std::filesystem::path &path) {
    auto text = utf8_of(path);
    return !text.empty() && text.front() == '/';
}
} // namespace

ExecutionRequest ExecutionRequest::create(std::vector<std::string> argv,
                                          std::optional<std::filesystem::path> cwd,
                                          std::optional<std::map<std::string, std::string>> env,
                                          std::optional<std::chrono::milliseconds> timeout,
                                          std::optional<RequestKey> key) {
    if (argv.empty())
        throw RequestError("INVALID_REQUEST", "argv must not be empty");
    // The target path is resolved inside the sandbox root, so it has to be an
    // absolute path in the sandbox namespace; there is no PATH lookup.
    if (!is_absolute(std::filesystem::path(argv.front())))
        throw RequestError("INVALID_REQUEST", "argv[0] must be an absolute path");
    for (const auto &arg : argv)
        reject_nul(as_view(arg), "argv");

    if (cwd) {
        if (!is_absolute(*cwd))
            throw RequestError("INVALID_REQUEST", "cwd must be an absolute path");
        reject_nul(utf8_of(*cwd), "cwd");
    }

    if (env) {
        for (const auto &[name, value] : *env) {
            if (name.empty())
                throw RequestError("INVALID_REQUEST", "empty environment variable name");
            reject_nul(as_view(name), "environment name");
            reject_nul(as_view(value), "environment value");
            if (name.find('=') != std::string::npos)
                throw RequestError("INVALID_REQUEST", "environment name must not contain '='");
        }
    }

    if (timeout) {
        if (timeout->count() <= 0)
            throw RequestError("INVALID_REQUEST", "timeout must be positive");
        if (*timeout > max_timeout)
            throw RequestError("INVALID_REQUEST", "timeout exceeds the supported maximum");
    }

    ExecutionRequest request;
    request.argv_ = std::move(argv);
    request.cwd_ = std::move(cwd);
    request.env_ = std::move(env);
    request.timeout_ = timeout;
    request.key_ = std::move(key);
    return request;
}

bool ExecutionRequest::same_intent(const ExecutionRequest &other) const noexcept {
    if (argv_ != other.argv_ || key_ != other.key_ || timeout_ != other.timeout_)
        return false;
    if (cwd_.has_value() != other.cwd_.has_value())
        return false;
    if (cwd_ && utf8_of(*cwd_) != utf8_of(*other.cwd_))
        return false;
    // Env presence carries meaning: "inherit" vs "explicitly empty".
    if (env_.has_value() != other.env_.has_value())
        return false;
    if (env_ && *env_ != *other.env_)
        return false;
    return true;
}

} // namespace sandbox
