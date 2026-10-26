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
#include <sstream>
#include <string>
#include <vector>

#include "bindings.hpp"
#include "client.hpp"
#include "json.hpp"

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
        for (std::size_t row = 0; row < lines.size() && row < 44; ++row)
            std::printf("\x1b[%zu;1H%-79.79s", row + 1, lines[row].c_str());
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
    bool sockets_ready = R_SUCCEEDED(socketInitializeDefault());

    Screen screen;
    std::deque<std::string> notices;
    auto note = [&](const std::string& text) {
        notices.push_back(text);
        while (notices.size() > 6) notices.pop_front();
    };

    auto json = sw::json_parse(read_file(kProfilePath));
    std::string error;
    auto profile = json ? sw::profile_from_json(*json, error) : std::nullopt;

    if (!profile || !sockets_ready) {
        std::vector<std::string> lines = {"switchy-op", ""};
        if (!sockets_ready) lines.push_back("The network could not be started (is Wi-Fi on?).");
        if (!profile) {
            lines.push_back(std::string("Profile problem: ") + (json ? error : "file missing or invalid"));
            lines.push_back(std::string("Expected: ") + kProfilePath);
        }
        lines.push_back("");
        lines.push_back("Press + to exit.");
        while (appletMainLoop()) {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
            screen.show(lines);
        }
        if (sockets_ready) socketExit();
        consoleExit(nullptr);
        return 0;
    }

    std::map<std::uint32_t, std::string> action_names;
    sw::ClientConfig config;
    config.host = profile->host;
    config.port = profile->port;
    config.key = profile->key;
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

    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        double now = now_seconds();

        if (!client.connected() && now >= next_attempt) {
            status = "connecting to " + config.host + ":" + std::to_string(config.port) + " ...";
            screen.show({"switchy-op", "", status});
            std::string problem;
            if (client.connect(problem)) {
                status = "connected";
            } else {
                status = problem;
                next_attempt = now_seconds() + kReconnectEveryS;
            }
        }

        if (client.connected()) {
            sw::Output out = bindings.update(read_input(pad), now, client.state());
            client.drive(out.vx, out.wz, out.deadman, out.held);
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
        lines.push_back("Hold ZR to drive. Plus exits.");
        screen.show(lines);
    }

    client.close();
    socketExit();
    consoleExit(nullptr);
    return 0;
}
