#pragma once
#include <switch.h>
#include "quick_start.hpp"

namespace quick_start {
constexpr const char* path = "sdmc:/switch/switchy-op/last-session.json";
enum class Choice { Resume, Manager, Exit };

template<class Screen>
Choice choose(Screen& screen, PadState& pad, const sw::LastSession& session) {
    bool released = false;
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 held = padGetButtons(&pad), down = padGetButtonsDown(&pad);
        if (!released) { released = held == 0; down = 0; }
        if (down & HidNpadButton_Plus) return Choice::Exit;
        if (down & HidNpadButton_B) return Choice::Manager;
        if (down & HidNpadButton_A) return Choice::Resume;
        screen.show({"switchy-op | Quick start", "", "Last successful connection:",
            session.endpoint.name,
            session.endpoint.host + ":" + std::to_string(session.endpoint.port), "",
            "Saved control profile:",
            "Forward: " + session.profile.forward.axis + " | turn: " + session.profile.turn.axis,
            "Deadman: " + (session.profile.deadman.empty() ? std::string("NONE") : session.profile.deadman),
            "Actions: " + std::to_string(session.profile.actions.size()), "",
            "A: quick start with saved profile and address",
            "B: choose connection / download another profile", "Plus: exit", "",
            "Release all buttons after connecting, then press deadman to drive."});
    }
    return Choice::Exit;
}
} // namespace quick_start
