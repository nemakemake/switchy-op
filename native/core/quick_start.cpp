#include "quick_start.hpp"
#include "profile_sync.hpp"
#include <cmath>
#include <fstream>

namespace sw {
namespace {
std::optional<LastSession> decode(const Json& root) {
    if (root.number_or("version", 0) != 1) return std::nullopt;
    const Json* endpoint = root.find("endpoint");
    const Json* json = root.find("profile");
    if (!endpoint || !json) return std::nullopt;
    double port = endpoint->number_or("port", 0);
    LastSession session;
    session.endpoint = {endpoint->string_or("name", ""), endpoint->string_or("host", ""),
                        endpoint->string_or("key", ""), 0};
    const auto& e = session.endpoint;
    if (e.name.empty() || e.name.size() > 120 || e.host.empty() || e.host.size() > 120 ||
        e.host.find_first_of(" \t\r\n/:") != std::string::npos || e.key.empty() ||
        e.key.size() > 120 || !std::isfinite(port) || port < 1 || port > 65535 ||
        std::floor(port) != port) return std::nullopt;
    std::string error;
    auto profile = profile_from_json(*json, error);
    if (!profile) return std::nullopt;
    session.endpoint.port = int(port);
    session.profile = std::move(*profile);
    session.profile_json = *json;
    return session;
}
} // namespace
std::optional<LastSession> load_last_session(const std::string& path) {
    recover_profile(path);
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    // Bounded read: malformed settings must not consume arbitrary memory at startup.
    std::string text(64 * 1024 + 1, '\0');
    file.read(text.data(), std::streamsize(text.size()));
    text.resize(std::size_t(file.gcount()));
    if (file.bad() || text.size() > 64 * 1024) return std::nullopt;
    auto root = json_parse(text);
    return root ? decode(*root) : std::nullopt;
}
bool remember_session(const std::string& path, const LastEndpoint& e,
                      const Json& profile, std::string& error) {
    Json root = Json::object(), endpoint = Json::object();
    root["version"] = Json::of(1);
    endpoint["name"] = Json::of(e.name); endpoint["host"] = Json::of(e.host);
    endpoint["port"] = Json::of(e.port); endpoint["key"] = Json::of(e.key);
    root["endpoint"] = std::move(endpoint); root["profile"] = profile;
    if (json_dump(root).size() + 1 > 64 * 1024 || !decode(root)) {
        error = "Invalid session. Previous quick start kept."; return false;
    }
    return save_json_file(root, path, error);
}
} // namespace sw
