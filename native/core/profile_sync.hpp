#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "bindings.hpp"

namespace sw {
struct ProfileSource {
    std::string host, key;
    std::uint16_t port = 7080;
};

bool list_profiles(const ProfileSource& source, std::vector<std::string>& names, std::string& error);
std::optional<Json> download_profile(const ProfileSource& source, const std::string& name,
                                     std::string& error);
// Validates again locally, preserves the previous file as .bak, and replaces via a
// temporary file in the same directory. Does not change any connection entries.
bool install_profile(const Json& json, const std::string& path, Profile& profile, std::string& error);
// Recover a profile if power was lost after moving the old file aside on Switch.
void recover_profile(const std::string& path);
bool save_json_file(const Json& json, const std::string& path, std::string& error);
} // namespace sw
