// Drives the native client against a real Python gateway and checks what the robot did.
// Usage: pc_check [--host H] [--port P] [--key K] [--profile profile.json]
// Exit code 0 when every check passed.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <thread>

#include "../core/bindings.hpp"
#include "../core/client.hpp"
#include "../core/net.hpp"

using namespace sw;
using Clock = std::chrono::steady_clock;

namespace {

constexpr double kTickS = 1.0 / 30.0;

struct Answer {
    std::string status;
    std::string reason;
};

class Rig {
public:
    Rig(Profile profile, ClientConfig config)
        : bindings_(std::move(profile)), client_(std::move(config), events()) {}

    Client& client() { return client_; }

    bool connect() {
        for (int attempt = 0; attempt < 40; ++attempt) {
            std::string error;
            if (client_.connect(error)) return true;
            last_error_ = error;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        std::printf("cannot connect: %s\n", last_error_.c_str());
        return false;
    }

    // One frame of operator input: bindings, drive stream, action requests, network.
    void tick(const InputState& input) {
        Output out = bindings_.update(input, seconds(), client_.state());
        client_.drive(out.vx, out.wz, out.deadman, out.held);
        for (const Request& request : out.requests) {
            std::uint32_t id = request.op == "start"
                                   ? client_.start_long(request.name, request.confirmed)
                                   : client_.action(request.name, "set", request.value);
            sent_[request.name] = id;
        }
        client_.update();
        std::this_thread::sleep_for(std::chrono::duration<double>(kTickS));
    }

    void hold(double duration, const InputState& input) {
        double end = seconds() + duration;
        while (seconds() < end) tick(input);
    }

    bool wait_until(const std::function<bool()>& done, double timeout, const InputState& input = {}) {
        double end = seconds() + timeout;
        while (seconds() < end) {
            if (done()) return true;
            tick(input);
        }
        return done();
    }

    std::optional<Answer> ack(std::uint32_t id) const {
        auto it = acks_.find(id);
        return it == acks_.end() ? std::nullopt : std::optional<Answer>(it->second);
    }
    std::optional<Answer> result(std::uint32_t id) const {
        auto it = results_.find(id);
        return it == results_.end() ? std::nullopt : std::optional<Answer>(it->second);
    }
    std::uint32_t last_sent(const std::string& name) const {
        auto it = sent_.find(name);
        return it == sent_.end() ? 0 : it->second;
    }

    double speed() const { return client_.state().number_or("speed", -1); }
    double x() const { return client_.state().number_or("x", 0); }
    bool flag(const std::string& key) const { return client_.state().bool_or(key, false); }
    std::string text(const std::string& key) const { return client_.state().string_or(key, ""); }

private:
    double seconds() const {
        return std::chrono::duration<double>(Clock::now() - start_).count();
    }

    ClientEvents events() {
        ClientEvents e;
        e.on_ack = [this](std::uint32_t id, const std::string& status, const std::string& reason) {
            acks_[id] = {status, reason};
        };
        e.on_result = [this](std::uint32_t id, const std::string& status, const std::string& reason) {
            results_[id] = {status, reason};
        };
        e.on_log = [](const std::string& text) { std::printf("    [client] %s\n", text.c_str()); };
        return e;
    }

    Bindings bindings_;
    Client client_;
    Clock::time_point start_ = Clock::now();
    std::map<std::uint32_t, Answer> acks_;
    std::map<std::uint32_t, Answer> results_;
    std::map<std::string, std::uint32_t> sent_;
    std::string last_error_;
};

int failures = 0;

void check(const std::string& name, bool ok) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++failures;
}

InputState pad(std::initializer_list<const char*> buttons, double left_y = 0, double right_x = 0) {
    InputState input;
    for (const char* b : buttons) input.buttons.insert(b);
    input.axes["left_y"] = left_y;
    input.axes["right_x"] = right_x;
    return input;
}

std::string read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1", key = "dev-key", profile_path = "profiles/gamepad.json";
    int port = 7000;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string flag = argv[i], value = argv[i + 1];
        if (flag == "--host") host = value;
        else if (flag == "--port") port = std::atoi(value.c_str());
        else if (flag == "--key") key = value;
        else if (flag == "--profile") profile_path = value;
    }

    net::startup();
    auto json = json_parse(read_file(profile_path));
    std::string error;
    auto profile = json ? profile_from_json(*json, error) : std::nullopt;
    if (!profile) {
        std::printf("cannot read profile %s: %s\n", profile_path.c_str(),
                    json ? error.c_str() : "not valid JSON");
        return 2;
    }

    ClientConfig config;
    config.host = host;
    config.port = std::uint16_t(port);
    config.key = key;
    config.name = "pc-check";
    Rig rig(*profile, config);
    if (!rig.connect()) return 2;

    const Json& caps = rig.client().capabilities();
    const Json* actions = caps.find("actions");
    check("handshake accepted (the gateway verified our HMAC)", actions && actions->find("light"));

    std::printf("-- driving\n");
    rig.hold(1.0, pad({"zr"}, -1.0));
    check("drives forward while the dead-man is held", rig.speed() > 0.5 && rig.x() > 0.3);
    rig.hold(0.7, pad({}));
    check("stops when the dead-man is released", rig.speed() == 0.0);

    std::printf("-- buttons\n");
    rig.hold(0.1, pad({"y"}));
    rig.hold(0.5, pad({}));
    check("light switches on", rig.flag("light"));
    rig.hold(0.1, pad({"y"}));
    rig.hold(0.5, pad({}));
    check("same button switches it off (mirrors the robot's real state)", !rig.flag("light"));

    rig.hold(0.5, pad({"b"}));
    bool honking = rig.flag("horn");
    rig.hold(0.5, pad({}));
    check("horn sounds only while the button is held", honking && !rig.flag("horn"));

    rig.hold(1.2, pad({"x"}));
    std::uint32_t arm = rig.last_sent("arm_home");
    check("holding X for a second starts the arm", arm != 0);
    bool done = rig.wait_until([&] { return rig.result(arm).has_value(); }, 4.0);
    std::printf("    arm result: %s\n", done ? rig.result(arm)->status.c_str() : "(none)");
    check("arm action finishes successfully", done && rig.result(arm)->status == "succeeded");
    check("telemetry shows the arm at home",
          rig.wait_until([&] { return rig.text("arm") == "home"; }, 1.0));

    std::printf("-- safety interlocks\n");
    std::uint32_t on = rig.client().action("estop", "set", true);
    rig.wait_until([&] { return rig.ack(on).has_value(); }, 2.0);
    std::uint32_t blocked = rig.client().start_long("arm_home", true);
    rig.wait_until([&] { return rig.ack(blocked).has_value(); }, 2.0);
    auto refused = rig.ack(blocked);
    check("arm refused while e-stop is active",
          refused && refused->status == "rejected" && refused->reason == "estop_active");
    std::uint32_t off = rig.client().action("estop", "set", false);
    rig.wait_until([&] { return rig.ack(off).has_value(); }, 2.0);
    std::uint32_t unconfirmed = rig.client().start_long("arm_home", false);
    rig.wait_until([&] { return rig.ack(unconfirmed).has_value(); }, 2.0);
    auto unsure = rig.ack(unconfirmed);
    check("arm refused without confirmation",
          unsure && unsure->reason == "confirmation_required");

    std::printf("-- link loss\n");
    rig.hold(1.2, pad({}));
    std::uint32_t arm2 = rig.client().start_long("arm_home", true);
    rig.wait_until([&] { return rig.ack(arm2).has_value(); }, 2.0);
    rig.hold(0.8, pad({"zr"}, -1.0));
    check("robot is moving before the link is cut", rig.speed() > 0.5);
    rig.client().set_stream_paused(true);
    rig.hold(0.9, pad({"zr"}, -1.0));
    check("robot stops by itself when the stream goes quiet", rig.speed() == 0.0);
    bool cancelled = rig.wait_until([&] { return rig.result(arm2).has_value(); }, 2.0,
                                    pad({"zr"}, -1.0));
    check("running arm action is cancelled on link loss",
          cancelled && rig.result(arm2)->status == "canceled");
    rig.client().set_stream_paused(false);
    rig.hold(0.6, pad({"zr"}, -1.0));
    check("driving resumes after the link returns", rig.speed() > 0.5);
    rig.hold(0.3, pad({}));

    std::printf("%s: %d check(s) failed\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
