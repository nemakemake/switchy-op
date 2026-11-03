#include "../core/quick_start.hpp"
#include <cstdio>
#include <fstream>
#include <sstream>

int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
        if (!ok) ++failures;
    };
    const std::string path = "build/quick-start-test.json";
    std::ifstream input("profiles/gamepad.json");
    std::stringstream content; content << input.rdbuf();
    auto json = sw::json_parse(content.str());
    if (!json) return 2;
    std::string error;
    sw::LastEndpoint endpoint{"Lab robot", "192.168.1.42", "secret-key", 7010};
    check(sw::remember_session(path, endpoint, *json, error), "save successful session");
    auto last = sw::load_last_session(path);
    check(last && last->endpoint.host == endpoint.host && last->endpoint.port == 7010 &&
          last->endpoint.key == endpoint.key, "restore exact address, port and key");
    check(last && sw::json_dump(last->profile_json) == sw::json_dump(*json), "restore full control profile snapshot");
    // A downloaded profile may change independently; the last used snapshot is stable.
    auto changed = *json;
    changed["drive"]["turn"]["scale"] = sw::Json::of(.25);
    last = sw::load_last_session(path);
    check(last && last->profile.turn.scale != .25, "unused profile does not replace last session");
    endpoint.host = "192.168.1.55";
    check(sw::remember_session(path, endpoint, changed, error), "update existing session on next successful connection");
    last = sw::load_last_session(path);
    check(last && last->endpoint.host == endpoint.host && last->profile.turn.scale == .25,
          "updated address and profile restored together");
    auto bad = endpoint; bad.port = 99999;
    check(!sw::remember_session(path, bad, changed, error), "invalid address rejected");
    check(!sw::remember_session(path, endpoint, sw::Json::object(), error), "invalid profile rejected");
    last = sw::load_last_session(path);
    check(last && last->endpoint.host == endpoint.host && last->profile.turn.scale == .25,
          "failed update preserves last successful session");
    check(std::rename(path.c_str(), (path + ".rollback").c_str()) == 0, "simulate interrupted SD replacement");
    last = sw::load_last_session(path);
    check(last && last->endpoint.host == endpoint.host, "recover interrupted SD replacement on startup");
    { std::ofstream broken(path); broken << "{broken"; }
    check(!sw::load_last_session(path), "corrupt state falls back to connection manager");
    { std::ofstream future(path); future << "{\"version\":999}"; }
    check(!sw::load_last_session(path), "unknown state version ignored");
    { std::ofstream huge(path); huge << std::string(65537, 'x'); }
    check(!sw::load_last_session(path), "oversize state ignored");
    check(sw::remember_session(path, endpoint, changed, error), "replace broken settings after a successful connection");
    return failures ? 1 : 0;
}
