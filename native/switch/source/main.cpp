// switchy-op for Nintendo Switch (libnx): reads the controllers, drives a robot through a
// switchy-op gateway and shows the robot's state on the console screen.
//
// The protocol, key handling and button bindings live in ../../core and are the same code that is
// checked against the Python gateway on a PC (native/check_pc.sh). This file is the thin
// platform layer: input, screen, sockets start-up and the main loop.
//
// Plus quits. Everything else is mapped by the profile file:
//   sdmc:/switch/switchy-op/profile.json   (made with `switchy-op export-profile`)
#include <switch.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "bindings.hpp"
#include "client.hpp"
#include "json.hpp"
#include "connections.hpp"
#include "video_screen.hpp"
#include "profile_menu.hpp"
#include "quick_start_menu.hpp"
#include "tui_brand.hpp"

namespace {

constexpr const char* kProfilePath = "sdmc:/switch/switchy-op/profile.json";
constexpr double kReconnectEveryS = 3.0;
constexpr double kStickMax = 32767.0;

struct ButtonName {
    u64 mask;
    const char* name;
};

// Plus is reserved for quitting, so it is not offered to the profile.
constexpr ButtonName kButtons[] = {
    {HidNpadButton_A, "a"},         {HidNpadButton_B, "b"},        {HidNpadButton_X, "x"},
    {HidNpadButton_Y, "y"},         {HidNpadButton_L, "l"},        {HidNpadButton_R, "r"},
    {HidNpadButton_ZL, "zl"},       {HidNpadButton_ZR, "zr"},      {HidNpadButton_Minus, "minus"},
    {HidNpadButton_StickL, "l3"},   {HidNpadButton_StickR, "r3"},  {HidNpadButton_Up, "up"},
    {HidNpadButton_Down, "down"},   {HidNpadButton_Left, "left"},  {HidNpadButton_Right, "right"},
};

double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string read_file(const char* path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// A fixed block of text lines; each line is padded so old text never lingers on screen.
class Screen {
public:
    void show(const std::vector<std::string>& lines) {
        const auto& header = tui::banner();
        for (std::size_t row = 0; row < 44; ++row) {
            const char* text = row < header.size() ? header[row].c_str()
                : row - header.size() < lines.size() ? lines[row - header.size()].c_str() : "";
            std::printf("\x1b[%zu;1H\x1b[%sm%-79.79s\x1b[0m", row + 1,
                        row < 6 ? "36;1" : "37", text);
        }
        consoleUpdate(nullptr);
    }
};

sw::InputState read_input(PadState& pad) {
    sw::InputState input;
    u64 held = padGetButtons(&pad);
    for (const ButtonName& button : kButtons)
        if (held & button.mask) input.buttons.insert(button.name);

    HidAnalogStickState left = padGetStickPos(&pad, 0);
    HidAnalogStickState right = padGetStickPos(&pad, 1);
    // The Switch reports stick up as positive y; the profile convention is up = -1.
    input.axes["left_x"] = left.x / kStickMax;
    input.axes["left_y"] = -left.y / kStickMax;
    input.axes["right_x"] = right.x / kStickMax;
    input.axes["right_y"] = -right.y / kStickMax;
    return input;
}

std::string fmt(const char* format, ...) {
    char buffer[160];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof buffer, format, args);
    va_end(args);
    return buffer;
}

}  // namespace

int main(int, char**) {
    consoleInit(nullptr);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    Screen screen;
    screen.show({"Starting controller and network ..."});
    bool sockets_ready = R_SUCCEEDED(socketInitializeDefault());
    std::deque<std::string> notices;
    auto note = [&](const std::string& text) {
        notices.push_back(text);
        while (notices.size() > 6) notices.pop_front();
    };

    sw::recover_profile(kProfilePath);
    auto json = sw::json_parse(read_file(kProfilePath));
    std::string error;
    auto profile = json ? sw::profile_from_json(*json, error) : std::nullopt;
    std::optional<connections::Entry> startup_endpoint;
    if (sockets_ready) {
        auto last = sw::load_last_session(quick_start::path);
        if (last) {
            auto choice = quick_start::choose(screen, pad, *last);
            if (choice == quick_start::Choice::Exit) {
                socketExit(); consoleExit(nullptr); return 0;
            }
            if (choice == quick_start::Choice::Resume) {
                profile = last->profile;
                json = last->profile_json;
                const auto& e = last->endpoint;
                startup_endpoint = connections::Entry{e.name, e.host, e.key, e.port};
            }
        }
    }

    if (!profile || !sockets_ready) {
        std::vector<std::string> lines = {"switchy-op", ""};
        if (!sockets_ready) lines.push_back("The network could not be started (is Wi-Fi on?).");
        if (!profile) {
            lines.push_back(std::string("Profile problem: ") + (json ? error : "file missing or invalid"));
            lines.push_back(std::string("Expected: ") + kProfilePath);
        }
        lines.push_back("");
        lines.push_back("Press Y to download a profile from PC over Wi-Fi, + to exit.");
        while (appletMainLoop()) {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
            if (sockets_ready && (padGetButtonsDown(&pad) & HidNpadButton_Y)) {
                sw::Profile imported;
                if (profile_menu::load(screen, pad, imported, kProfilePath)) {
                    profile = std::move(imported);
                    json = sw::json_parse(read_file(kProfilePath));
                    break;
                }
            }
            screen.show(lines);
        }
        if (!profile || !sockets_ready) {
            if (sockets_ready) socketExit();
            consoleExit(nullptr);
            return 0;
        }
    }

    while (appletMainLoop()) {
    auto endpoint = startup_endpoint;
    startup_endpoint.reset();
    if (!endpoint) endpoint = connections::choose(screen, pad, *profile, [&] {
        bool loaded = profile_menu::load(screen, pad, *profile, kProfilePath);
        if (loaded) json = sw::json_parse(read_file(kProfilePath));
        return loaded;
    });
    if (!endpoint) break;
    auto video_config = sw::json_parse(read_file("sdmc:/switch/switchy-op/video.json"));
    bool video_enabled = !video_config || video_config->bool_or("enabled", true);
    bool require_video = !video_config || video_config->bool_or("require_video", true);
    std::string video_url = video_config ? video_config->string_or("url", "") : "";
    if (video_url.empty()) video_url = "rtsp://" + endpoint->host + ":8554/robot";
    std::unique_ptr<sw::VideoReceiver> video;
    std::unique_ptr<VideoScreen> video_screen;
    sw::VideoFrame video_frame;
    if (video_enabled) {
        video_screen = std::make_unique<VideoScreen>();
        if (video_screen->ready()) video = std::make_unique<sw::VideoReceiver>(video_url);
        else video_screen.reset();
    }
    notices.clear();
    std::map<std::uint32_t, std::string> action_names;
    sw::ClientConfig config;
    config.host = endpoint->host;
    config.port = endpoint->port;
    config.key = endpoint->key;
    config.name = "switch";

    sw::ClientEvents events;
    events.on_ack = [&](std::uint32_t id, const std::string& status, const std::string& reason) {
        note(action_names[id] + ": " + status + (reason.empty() ? "" : " (" + reason + ")"));
    };
    events.on_result = [&](std::uint32_t id, const std::string& status, const std::string& reason) {
        note(action_names[id] + " finished: " + status + (reason.empty() ? "" : " (" + reason + ")"));
    };
    events.on_log = [&](const std::string& text) { note(text); };

    sw::Client client(config, events);
    sw::Bindings bindings(*profile);
    double next_attempt = 0;
    std::string status = "not connected";
    bool quit = false;
    bool input_ready = false;
    bool session_recorded = false;
    sw::VideoDriveGuard video_guard;

    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtons(&pad) & HidNpadButton_Plus) { quit = true; break; }
        const u64 manager_chord = HidNpadButton_L | HidNpadButton_R;
        if ((padGetButtons(&pad) & manager_chord) == manager_chord) break;
        if (padGetButtons(&pad) == 0) input_ready = true;
        double now = now_seconds();

        if (!client.connected() && now >= next_attempt) {
            status = "connecting to " + config.host + ":" + std::to_string(config.port) + " ...";
            if (!video_screen) screen.show({"switchy-op", "", status});
            std::string problem;
            if (client.connect(problem)) {
                status = "connected";
                if (!session_recorded && json) {
                    std::string save_error;
                    if (!sw::remember_session(quick_start::path,
                            {endpoint->name, endpoint->host, endpoint->key, endpoint->port}, *json, save_error))
                        note("Quick start not saved: " + save_error);
                    session_recorded = true;
                }
            } else {
                status = problem;
                next_attempt = now_seconds() + kReconnectEveryS;
            }
        }

        // connect() may block: sample the camera and controller again afterwards.
        padUpdate(&pad);
        if (video) video->snapshot(video_frame);
        now = now_seconds();
        bool video_live = video && sw::video_is_live(video_frame, now);
        sw::Output displayed;
        if (!client.connected()) video_guard.update(video_enabled && require_video, false, false);
        if (client.connected()) {
            sw::Output out = bindings.update(input_ready ? read_input(pad) : sw::InputState{}, now, client.state());
            bool blocked = video_guard.update(video_enabled && require_video, video_live, out.deadman);
            client.drive(blocked ? 0 : out.vx, blocked ? 0 : out.wz,
                         !blocked && out.deadman, blocked ? std::vector<std::string>{} : out.held);
            displayed.vx = blocked ? 0 : out.vx;
            displayed.wz = blocked ? 0 : out.wz;
            displayed.deadman = !blocked && out.deadman;
            for (const sw::Request& request : out.requests) {
                std::uint32_t id = request.op == "start"
                                       ? client.start_long(request.name, request.confirmed)
                                       : client.action(request.name, "set", request.value);
                action_names[id] = request.name;
            }
            client.update();
            if (!client.connected()) status = "connection lost, retrying";
        }

        const sw::Json& state = client.state();
        std::vector<std::string> lines = {
            fmt("switchy-op   %s", client.connected() ? client.capabilities().string_or("name", "?").c_str() : "offline"),
            status,
            "",
            fmt("speed %.2f m/s   heading %.0f   battery %.0f%%", state.number_or("speed", 0),
                state.number_or("heading", 0), state.number_or("battery", 0) * 100),
            fmt("light %s   horn %s   arm %s   e-stop %s", state.bool_or("light", false) ? "on" : "off",
                state.bool_or("horn", false) ? "on" : "off", state.string_or("arm", "?").c_str(),
                state.bool_or("estop", false) ? "ACTIVE" : "off"),
            "",
        };
        for (const std::string& line : notices) lines.push_back(line);
        lines.push_back("");
        lines.push_back("Hold ZR to drive. L+R: connections. Plus exits.");
        if (video_guard.blocked(video_enabled && require_video))
            lines.push_back("DRIVE BLOCKED: live video required; release deadman to rearm.");
        if (video_screen) {
            double since_frame = video_frame.received_at > 0 ? (now - video_frame.received_at) * 1000 : -1;
            video_screen->draw(video_frame, fmt("switchy-op | %s | battery %.0f%%",
                client.connected() ? "CONNECTED" : "OFFLINE", state.number_or("battery", 0) * 100),
                {"CONTROL", fmt("Speed %.2f m/s | heading %.0f", state.number_or("speed", 0), state.number_or("heading", 0)),
                 fmt("Drive %+.0f%% | turn %+.0f%%", displayed.vx * 100, displayed.wz * 100),
                 fmt("Deadman: %s", displayed.deadman ? "ACTIVE" : "RELEASED / BLOCKED"),
                 fmt("Light %s | horn %s", state.bool_or("light", false) ? "ON" : "OFF", state.bool_or("horn", false) ? "ON" : "OFF"),
                 "ZR: drive | L+R: menu | +: exit",
                 notices.empty() ? "" : notices.back()},
                {"VIDEO / LINK", fmt("Decoded: %.1f FPS", video_live ? video_frame.fps : 0),
                 fmt("Since decoded frame: %.0f ms", since_frame),
                 fmt("Frame interval: %.0f ms", video_frame.gap_ms),
                 fmt("Reconnects: %lu", video_frame.reconnects),
                 "RTT / packet loss: unavailable", status},
                state.bool_or("estop", false), video_guard.blocked(video_enabled && require_video));
        } else screen.show(lines);
    }

    if (client.connected()) {
        client.drive(0, 0, false, {});
        client.update();
    }
    client.close();
    if (quit) break;
    }
    socketExit();
    consoleExit(nullptr);
    return 0;
}
