#include "profile_sync.hpp"
#include "net.hpp"
#include "sha256.hpp"

#include <chrono>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#endif

namespace sw {
namespace {
constexpr std::size_t kMaxWire = 128 * 1024;
double clock_now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct Connection {
    net::Socket socket;
    ~Connection() { net::close_socket(socket); }
};
bool read_line(net::Socket socket, std::string& out, double deadline) {
    out.clear();
    char chunk[2048];
    while (clock_now() < deadline && out.size() <= kMaxWire) {
        long count = net::recv_some(socket, chunk, sizeof chunk);
        if (count > 0) {
            out.append(chunk, std::size_t(count));
            auto end = out.find('\n');
            if (end != std::string::npos) {
                if (end > kMaxWire) return false;
                out.resize(end); return true;
            }
        } else if (count == 0 || !net::would_block()) return false;
        else std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}
bool send_line(net::Socket socket, const std::string& line, double deadline) {
    std::size_t offset = 0;
    while (offset < line.size() && clock_now() < deadline) {
        long sent = net::send_some(socket, line.data() + offset, line.size() - offset);
        if (sent > 0) offset += std::size_t(sent);
        else if (sent == 0 || !net::would_block()) return false;
        else std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return offset == line.size();
}
bool equal_mac(const std::string& a, const std::string& b) {
    if (a.size() != 64 || b.size() != 64) return false;
    unsigned difference = 0;
    for (std::size_t i = 0; i < 64; ++i) difference |= unsigned(a[i] ^ b[i]);
    return difference == 0;
}
std::optional<Json> request(const ProfileSource& source, const std::string& op,
                            const std::string& name, std::string& error) {
    error = "Profile server unavailable. Check PC address, port and firewall.";
    Connection connection{net::connect_tcp(source.host, source.port)};
    if (connection.socket == net::kBad) return std::nullopt;
    double deadline = clock_now() + 5;
    std::string line;
    if (!read_line(connection.socket, line, deadline)) return std::nullopt;
    auto challenge = json_parse(line);
    std::string nonce = challenge ? challenge->string_or("nonce", "") : "";
    if (!challenge || challenge->string_or("service", "") != "switchy-profiles" ||
        challenge->number_or("version", 0) != 1 || nonce.size() != 32 ||
        nonce.find_first_not_of("0123456789abcdef") != std::string::npos) {
        error = "Not a compatible profile server."; return std::nullopt;
    }
    Json query = Json::object();
    query["op"] = Json::of(op); query["name"] = Json::of(name);
    query["mac"] = Json::of(hmac_sha256_hex(source.key, "profiles-request:" + nonce + ":" + op + ":" + name));
    error = "Download failed: wrong key, timeout or interrupted connection.";
    if (!send_line(connection.socket, json_dump(query) + "\n", deadline) ||
        !read_line(connection.socket, line, deadline)) return std::nullopt;
    auto envelope = json_parse(line);
    std::string payload = envelope ? envelope->string_or("payload", "") : "";
    if (!envelope || !equal_mac(envelope->string_or("mac", ""),
            hmac_sha256_hex(source.key, "profiles-response:" + nonce + ":" + payload))) {
        error = "Profile server signature invalid. Nothing changed."; return std::nullopt;
    }
    auto answer = json_parse(payload);
    if (!answer || !answer->is_object()) { error = "Invalid server response."; return std::nullopt; }
    if (!answer->string_or("error", "").empty()) {
        error = answer->string_or("error", ""); return std::nullopt;
    }
    error.clear();
    return answer;
}
bool rename_file(const std::string& from, const std::string& to) {
#ifdef SWITCHY_RENAME_NO_REPLACE
    // Reproduce Horizon's no-overwrite rename semantics in the PC integration tests.
    struct stat existing{};
    if (stat(to.c_str(), &existing) == 0) { errno = EEXIST; return false; }
#endif
    return std::rename(from.c_str(), to.c_str()) == 0;
}
bool replace_file(const std::string& from, const std::string& to) {
#if defined(_WIN32) && !defined(SWITCHY_RENAME_NO_REPLACE)
    return MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    if (rename_file(from, to)) return true;
    if (errno != EEXIST && errno != ENOTEMPTY) return false;
    // libnx fsdev forwards to fsFsRenameFile, which cannot overwrite an existing
    // destination. Keep the old file recoverable throughout the two renames.
    struct stat existing{};
    if (stat(to.c_str(), &existing) != 0 || !S_ISREG(existing.st_mode)) return false;
    std::string rollback = to + ".rollback";
    if (stat(rollback.c_str(), &existing) == 0 &&
        (!S_ISREG(existing.st_mode) || std::remove(rollback.c_str()) != 0)) return false;
    if (!rename_file(to, rollback)) return false;
    if (rename_file(from, to)) { std::remove(rollback.c_str()); return true; }
    rename_file(rollback, to);
    return false;
#endif
}
bool write_file(const std::string& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
    file.close();
    return bool(file);
}
} // namespace

void recover_profile(const std::string& path) {
    struct stat existing{};
    if (stat(path.c_str(), &existing) != 0 && errno == ENOENT)
        rename_file(path + ".rollback", path);
}

bool save_json_file(const Json& json, const std::string& path, std::string& error) {
    recover_profile(path);
    if (!write_file(path + ".tmp", json_dump(json) + "\n") ||
        !replace_file(path + ".tmp", path)) {
        error = "Cannot save settings. Check SD card.";
        return false;
    }
    error.clear();
    return true;
}

bool list_profiles(const ProfileSource& source, std::vector<std::string>& names, std::string& error) {
    names.clear();
    auto answer = request(source, "list", "", error);
    const Json* list = answer ? answer->find("profiles") : nullptr;
    if (!list || list->type != Json::Type::Array || list->items.size() > 32) {
        if (answer) error = "Invalid profile catalog.";
        return false;
    }
    for (const auto& item : list->items) {
        if (item.type != Json::Type::String || item.text.empty() || item.text.size() > 120 ||
            item.text.find_first_of("/\\\r\n\t") != std::string::npos) {
            error = "Invalid profile name."; names.clear(); return false;
        }
        names.push_back(item.text);
    }
    return true;
}
std::optional<Json> download_profile(const ProfileSource& source, const std::string& name,
                                     std::string& error) {
    auto answer = request(source, "get", name, error);
    const Json* json = answer ? answer->find("profile") : nullptr;
    if (!json || json_dump(*json).size() > 32 * 1024 || !profile_from_json(*json, error)) {
        if (error.empty()) error = "Downloaded profile is invalid or too large.";
        return std::nullopt;
    }
    return *json;
}
bool install_profile(const Json& json, const std::string& path, Profile& profile, std::string& error) {
    auto parsed = profile_from_json(json, error);
    std::string text = json_dump(json) + "\n";
    if (!parsed || text.size() > 32 * 1024) {
        if (error.empty()) error = "Profile exceeds 32 KiB.";
        return false;
    }
    error = "Cannot save profile. Previous profile kept; check SD card.";
    if (!write_file(path + ".tmp", text)) return false;
    std::ifstream old(path, std::ios::binary);
    struct stat existing{};
    if (!old && stat(path.c_str(), &existing) == 0) return false;
    if (old) {
        std::stringstream content;
        content << old.rdbuf();
        if (old.bad()) return false;
        old.close();
        if (!write_file(path + ".bak.tmp", content.str()) ||
            !replace_file(path + ".bak.tmp", path + ".bak")) return false;
    }
    if (!replace_file(path + ".tmp", path)) return false;
    profile = std::move(*parsed);
    error.clear();
    return true;
}
} // namespace sw
