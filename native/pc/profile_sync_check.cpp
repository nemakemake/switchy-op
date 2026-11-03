#include "../core/profile_sync.hpp"
#include "../core/net.hpp"
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc != 5 || !sw::net::startup()) return 2;
    sw::recover_profile(argv[4]);
    sw::ProfileSource source{"127.0.0.1", argv[2], std::uint16_t(std::atoi(argv[1]))};
    std::string error;
    std::vector<std::string> names;
    if (!sw::list_profiles(source, names, error)) { std::puts(error.c_str()); return 1; }
    for (const auto& name : names) std::printf("PROFILE %s\n", name.c_str());
    auto json = sw::download_profile(source, argv[3], error);
    sw::Profile profile;
    if (!json || !sw::install_profile(*json, argv[4], profile, error)) {
        std::puts(error.c_str()); return 1;
    }
    std::printf("APPLIED %s %s %s\n", profile.forward.axis.c_str(), profile.turn.axis.c_str(), profile.deadman.c_str());
    return 0;
}
