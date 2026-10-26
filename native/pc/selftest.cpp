// Prints HMAC-SHA256 for fixed inputs; compared against Python's hmac module by check_pc.sh.
#include <cstdio>
#include <string>

#include "../core/sha256.hpp"

int main() {
    struct Case { std::string key, message; };
    const Case cases[] = {
        {"dev-key", "hello:s1:n1"}, {"dev-key", ""},
        {"k", std::string(55, 'x')}, {"k", std::string(56, 'x')}, {"k", std::string(63, 'x')},
        {"k", std::string(64, 'x')}, {"k", std::string(65, 'x')}, {"k", std::string(200, 'y')},
        {std::string(100, 'K'), "long key message"},
    };
    for (const Case& c : cases) std::printf("%s\n", sw::hmac_sha256_hex(c.key, c.message).c_str());
}
