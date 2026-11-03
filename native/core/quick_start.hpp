#pragma once
#include "bindings.hpp"

namespace sw {
struct LastEndpoint {
    std::string name, host, key;
    int port = 7000;
};
struct LastSession {
    LastEndpoint endpoint;
    Json profile_json;
    Profile profile;
};
std::optional<LastSession> load_last_session(const std::string& path);
bool remember_session(const std::string& path, const LastEndpoint& endpoint,
                      const Json& profile, std::string& error);
} // namespace sw
