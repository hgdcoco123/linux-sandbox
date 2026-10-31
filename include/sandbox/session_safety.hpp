#pragma once
#include "sandbox/config.hpp"
#include <optional>
namespace sandbox {
inline std::optional<std::uint64_t> session_counter(const Json &raw, const char *group,
                                                    const char *key) {
    if (!raw.is_object())
        return std::nullopt;
    auto section = raw.find(group);
    if (section == raw.end() || !section->is_object())
        return std::nullopt;
    auto value = section->find(key);
    if (value == section->end())
        return std::nullopt;
    if (value->is_number_unsigned())
        return value->get<std::uint64_t>();
    if (value->is_number_integer() && value->get<std::int64_t>() >= 0)
        return value->get<std::uint64_t>();
    return std::nullopt;
}
inline Json session_counter_delta(const Json &before, const Json &after, const char *key) {
    auto a = session_counter(before, "cpu.stat", key), b = session_counter(after, "cpu.stat", key);
    return a && b && *b >= *a ? Json(*b - *a) : Json(nullptr);
}
inline Json recover_command(const Json &snapshot, const std::optional<Json> &saved) {
    if (saved) {
        if (!saved->is_object() ||
            saved->value("command_id", "") != snapshot.at("command_id").get<std::string>() ||
            (saved->value("state", "") != "finished" && saved->value("state", "") != "interrupted"))
            throw std::runtime_error("existing command result is invalid; refusing to overwrite");
        return *saved;
    }
    auto result = snapshot;
    result["state"] = "interrupted";
    result["exit_code"] = nullptr;
    result["signal"] = nullptr;
    return result;
}
} // namespace sandbox
