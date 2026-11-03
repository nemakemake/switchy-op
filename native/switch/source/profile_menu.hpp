#pragma once
#include "connections.hpp"
#include "profile_sync.hpp"

namespace profile_menu {
constexpr const char* source_path = "sdmc:/switch/switchy-op/profile-source.json";

template<class Screen>
bool load(Screen& screen, PadState& pad, sw::Profile& profile, const char* destination) {
    sw::ProfileSource source{profile.host, profile.key, 7080};
    std::ifstream saved(source_path, std::ios::binary);
    std::stringstream content; content << saved.rdbuf();
    auto settings = sw::json_parse(content.str());
    if (settings) {
        source.host = settings->string_or("host", source.host);
        double port = settings->number_or("port", 7080);
        if (port >= 1 && port <= 65535 && std::floor(port) == port) source.port = std::uint16_t(port);
    }
    std::string port = std::to_string(source.port);
    if (!connections::text("PC IPv4 address / hostname", source.host) ||
        !connections::text("Profile server port", port, false, true) ||
        !connections::text("Profile download key (--key on PC)", source.key, true)) return false;
    if (port.empty() || port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos ||
        std::atoi(port.c_str()) < 1 || std::atoi(port.c_str()) > 65535) return false;
    source.port = std::uint16_t(std::atoi(port.c_str()));
    // Remember the address, never the download key. Failure here does not prevent downloading.
    sw::Json preferences = sw::Json::object();
    preferences["host"] = sw::Json::of(source.host);
    preferences["port"] = sw::Json::of(int(source.port));
    std::ofstream output(source_path, std::ios::binary | std::ios::trunc);
    output << sw::json_dump(preferences); output.close();

    std::vector<std::string> names;
    std::string message;
    auto refresh = [&] {
        screen.show({"Profiles over Wi-Fi", "", "Loading catalog from " + source.host + ":" + port,
                     "Robot disconnected. Network operations may take a few seconds."});
        if (sw::list_profiles(source, names, message) && names.empty())
            message = "No valid profiles. Check the directory on the PC.";
    };
    refresh();
    std::size_t selected = 0;
    std::optional<sw::Json> pending;
    std::optional<sw::Profile> preview;
    bool released = false;
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 held = padGetButtons(&pad), down = padGetButtonsDown(&pad);
        if (!released) { released = held == 0; down = 0; }
        if (down & (HidNpadButton_B | HidNpadButton_Plus)) {
            if (pending) { pending.reset(); preview.reset(); released = false; }
            else return false;
        }
        if (pending && preview) {
            if (down & HidNpadButton_A) {
                if (sw::install_profile(*pending, destination, profile, message)) return true;
                pending.reset(); preview.reset(); released = false;
            }
        } else {
            if (down & HidNpadButton_X) { refresh(); selected = 0; released = false; }
            if (!names.empty()) {
                if (down & (HidNpadButton_Up | HidNpadButton_StickLUp)) selected = (selected + names.size() - 1) % names.size();
                if (down & (HidNpadButton_Down | HidNpadButton_StickLDown)) selected = (selected + 1) % names.size();
                if (down & HidNpadButton_A) {
                    screen.show({"Profiles over Wi-Fi", "", "Downloading " + names[selected] + " ..."});
                    pending = sw::download_profile(source, names[selected], message);
                    if (pending) preview = sw::profile_from_json(*pending, message);
                    released = false;
                }
            }
        }
        std::vector<std::string> lines = {"switchy-op | Profiles over Wi-Fi", "",
            source.host + ":" + port, ""};
        if (pending && preview) {
            lines.push_back("Apply " + names[selected] + "?");
            lines.push_back("Forward axis: " + preview->forward.axis);
            lines.push_back("Turn axis: " + preview->turn.axis);
            lines.push_back("Deadman: " + (preview->deadman.empty() ? std::string("NONE") : preview->deadman));
            lines.push_back("Actions: " + std::to_string(preview->actions.size()));
            lines.push_back("Current profile will be backed up as profile.json.bak.");
            lines.push_back("Connection entries will be kept. Robot stays disconnected.");
            lines.push_back("A: save and apply    B: cancel");
        } else {
            lines.push_back("D-pad: select    A: download    X: refresh    B: back");
            std::size_t first = selected > 15 ? selected - 15 : 0;
            for (std::size_t i = first; i < names.size() && i < first + 24; ++i)
                lines.push_back(std::string(i == selected ? "> " : "  ") + names[i]);
        }
        lines.push_back(""); lines.push_back(message);
        screen.show(lines);
    }
    return false;
}
} // namespace profile_menu
