#pragma once

#include <switch.h>
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <optional>
#include <string>
#include <vector>
#include "json.hpp"
#include "bindings.hpp"

namespace connections {
constexpr const char* path = "sdmc:/switch/switchy-op/connections.json";
struct Entry {
    std::string name, host, key;
    int port = 7000;
};

inline bool valid(const Entry& e) {
    return !e.name.empty() && e.name.size() <= 120 && !e.host.empty() &&
        e.host.size() <= 120 && e.host.find_first_of(" \t\r\n/:") == std::string::npos &&
        e.port > 0 && e.port <= 65535 && !e.key.empty() && e.key.size() <= 120;
}

inline bool save(const std::vector<Entry>& entries, std::size_t selected) {
    sw::Json root = sw::Json::object(), list = sw::Json::array();
    for (const auto& e : entries) {
        sw::Json item = sw::Json::object();
        item["name"] = sw::Json::of(e.name);
        item["host"] = sw::Json::of(e.host);
        item["port"] = sw::Json::of(e.port);
        item["key"] = sw::Json::of(e.key);
        list.push(item);
    }
    root["entries"] = list;
    root["selected"] = sw::Json::of(double(selected));
    const std::string temp = std::string(path) + ".tmp";
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    file << sw::json_dump(root) << '\n';
    file.close();
    if (!file) return false;
    return std::rename(temp.c_str(), path) == 0;
}

inline bool text(const char* title, std::string& value, bool secret = false,
                 bool numeric = false) {
    SwkbdConfig keyboard;
    if (R_FAILED(swkbdCreate(&keyboard, 0))) return false;
    swkbdConfigMakePresetDefault(&keyboard);
    swkbdConfigSetHeaderText(&keyboard, title);
    swkbdConfigSetInitialText(&keyboard, value.c_str());
    swkbdConfigSetStringLenMin(&keyboard, 1);
    swkbdConfigSetStringLenMax(&keyboard, numeric ? 5 : 120);
    if (numeric) swkbdConfigSetType(&keyboard, SwkbdType_NumPad);
    if (secret) swkbdConfigSetPasswordFlag(&keyboard, 1);
    char output[512] = {};
    Result result = swkbdShow(&keyboard, output, sizeof output);
    swkbdClose(&keyboard);
    if (R_FAILED(result)) return false;
    value = output;
    return true;
}

inline bool edit(Entry& e) {
    std::string port = std::to_string(e.port);
    if (!text("Connection name", e.name) || !text("IPv4 address or hostname", e.host) ||
        !text("TCP port (1-65535)", port, false, true) || !text("Gateway key", e.key, true))
        return false;
    if (port.empty() || port.find_first_not_of("0123456789") != std::string::npos || port.size() > 5)
        return false;
    e.port = std::atoi(port.c_str());
    return valid(e);
}

// No client exists while this menu is active. Controller input cannot reach the robot.
template<class Screen>
std::optional<Entry> choose(Screen& screen, PadState& pad, const sw::Profile& profile) {
    std::vector<Entry> entries;
    std::size_t selected = 0;
    std::string message;
    std::ifstream file(path, std::ios::binary);
    if (file) {
        std::stringstream buffer;
        buffer << file.rdbuf();
        auto root = sw::json_parse(buffer.str());
        const auto* list = root ? root->find("entries") : nullptr;
        if (!list || list->type != sw::Json::Type::Array) {
            message = "Cannot read connections.json. Original file kept until you save.";
        } else {
            for (const auto& item : list->items) {
                double port = item.number_or("port", 0);
                if (!std::isfinite(port) || port < 1 || port > 65535 || std::floor(port) != port)
                    continue;
                Entry e{item.string_or("name", ""), item.string_or("host", ""),
                        item.string_or("key", ""), int(port)};
                if (valid(e) && entries.size() < 32) entries.push_back(e);
            }
            double index = root->number_or("selected", 0);
            if (index >= 0 && index < entries.size()) selected = std::size_t(index);
        }
    } else {
        entries.push_back({"Profile gateway", profile.host, profile.key, profile.port});
        message = "Imported profile.json. A connects and saves this entry.";
    }
    bool confirm = false;
    // Consume the chord that opened the manager.
    bool released = false;
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 held = padGetButtons(&pad), down = padGetButtonsDown(&pad);
        if (!released) { released = held == 0; down = 0; }
        if (down & HidNpadButton_Plus) return std::nullopt;
        if (confirm) {
            if (down & HidNpadButton_B) confirm = false;
            if (down & HidNpadButton_A) {
                auto updated = entries;
                updated.erase(updated.begin() + selected);
                std::size_t next = updated.empty() ? 0 : std::min(selected, updated.size() - 1);
                if (save(updated, next)) {
                    entries = std::move(updated); selected = next; message = "Deleted.";
                } else message = "Save failed. Check SD card; entry kept.";
                confirm = false;
            }
        } else {
            if (!entries.empty()) {
                if (down & (HidNpadButton_Up | HidNpadButton_StickLUp))
                    selected = (selected + entries.size() - 1) % entries.size();
                if (down & (HidNpadButton_Down | HidNpadButton_StickLDown))
                    selected = (selected + 1) % entries.size();
                if (down & HidNpadButton_Y) confirm = true;
                if (down & HidNpadButton_A) {
                    if (save(entries, selected)) return entries[selected];
                    message = "Save failed. Check SD card before connecting.";
                }
            }
            bool add = down & HidNpadButton_X;
            bool modify = (down & HidNpadButton_R) && !entries.empty();
            if (add || modify) {
                if (add && entries.size() >= 32) message = "Maximum 32 connections.";
                else {
                    Entry e = modify ? entries[selected] : Entry{"New gateway", "", profile.key, 7000};
                    if (edit(e)) {
                        auto updated = entries;
                        std::size_t next = selected;
                        if (modify) updated[selected] = e;
                        else { updated.push_back(e); next = updated.size() - 1; }
                        if (save(updated, next)) {
                            entries = std::move(updated); selected = next; message = "Saved.";
                        } else message = "Save failed. Changes not applied.";
                    } else message = "Cancelled or invalid input. Changes not applied.";
                    released = false;
                }
            }
        }
        std::vector<std::string> lines = {"switchy-op | Connections", "",
            "D-pad / left stick: select     A: connect", "X: add    R: edit    Y: delete    Plus: exit", ""};
        if (entries.empty()) lines.push_back("No connections. Press X to add one.");
        std::size_t first = selected > 7 ? selected - 7 : 0;
        for (std::size_t i = first; i < entries.size() && i < first + 12; ++i) {
            lines.push_back(std::string(i == selected ? "> " : "  ") + entries[i].name);
            lines.push_back("    " + entries[i].host + ":" + std::to_string(entries[i].port));
        }
        lines.push_back("");
        lines.push_back(message);
        if (confirm) {
            lines.push_back("Delete " + entries[selected].name + "?");
            lines.push_back("A: DELETE permanently     B: cancel");
        }
        screen.show(lines);
    }
    return std::nullopt;
}
} // namespace connections
