#pragma once
#include <string>
#include <vector>

namespace tui {
inline const std::vector<std::string>& banner() {
    static const std::vector<std::string> lines = {
        R"(                      _ __       __                  )",
        R"(         ______      (_) /______/ /_  __  __      ____  ____ )",
        R"(        / ___/ | /| / / / __/ ___/ __ \/ / / /____/ __ \/ __ \)",
        R"(       (__  )| |/ |/ / / /_/ /__/ / / / /_/ /____/ /_/ / /_/ /)",
        R"(      /____/ |__/|__/_/\__/\___/_/ /_/\__, /     \____/ .___/ )",
        R"(                                   /____/          /_/      )",
        "             switchy-op 0.5.1  //  ROBOT TELEOP TERMINAL",
        "",
    };
    return lines;
}
} // namespace tui
