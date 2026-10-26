#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace sw {

Json& Json::operator[](const std::string& key) {
    if (type == Type::Null) type = Type::Object;
    return fields[key];
}

const Json* Json::find(const std::string& key) const {
    if (type != Type::Object) return nullptr;
    auto it = fields.find(key);
    return it == fields.end() ? nullptr : &it->second;
}

void Json::push(Json value) {
    if (type == Type::Null) type = Type::Array;
    items.push_back(std::move(value));
}

std::string Json::string_or(const std::string& key, const std::string& fallback) const {
    const Json* v = find(key);
    return v && v->type == Type::String ? v->text : fallback;
}

double Json::number_or(const std::string& key, double fallback) const {
    const Json* v = find(key);
    return v && v->type == Type::Number ? v->number : fallback;
}

bool Json::bool_or(const std::string& key, bool fallback) const {
    const Json* v = find(key);
    return v && v->type == Type::Bool ? v->boolean : fallback;
}

namespace {

constexpr int kMaxDepth = 32;

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    std::optional<Json> run() {
        auto value = parse_value(0);
        skip_space();
        if (!value || pos_ != text_.size()) return std::nullopt;
        return value;
    }

private:
    void skip_space() {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' ||
                                       text_[pos_] == '\n' || text_[pos_] == '\r'))
            ++pos_;
    }

    bool consume(char c) {
        if (pos_ < text_.size() && text_[pos_] == c) { ++pos_; return true; }
        return false;
    }

    bool literal(std::string_view word) {
        if (text_.substr(pos_, word.size()) != word) return false;
        pos_ += word.size();
        return true;
    }

    std::optional<Json> parse_value(int depth) {
        if (depth > kMaxDepth) return std::nullopt;
        skip_space();
        if (pos_ >= text_.size()) return std::nullopt;
        char c = text_[pos_];
        if (c == '{') return parse_object(depth);
        if (c == '[') return parse_array(depth);
        if (c == '"') {
            auto s = parse_string();
            if (!s) return std::nullopt;
            return Json::of(*s);
        }
        if (literal("true")) return Json::of(true);
        if (literal("false")) return Json::of(false);
        if (literal("null")) return Json();
        return parse_number();
    }

    std::optional<Json> parse_number() {
        std::size_t start = pos_;
        if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
        while (pos_ < text_.size() && ((text_[pos_] >= '0' && text_[pos_] <= '9') ||
                                       text_[pos_] == '.' || text_[pos_] == 'e' ||
                                       text_[pos_] == 'E' || text_[pos_] == '+' ||
                                       text_[pos_] == '-'))
            ++pos_;
        if (pos_ == start) return std::nullopt;
        std::string chunk(text_.substr(start, pos_ - start));
        char* end = nullptr;
        double value = std::strtod(chunk.c_str(), &end);
        if (end != chunk.c_str() + chunk.size() || !std::isfinite(value)) return std::nullopt;
        return Json::of(value);
    }

    static void append_utf8(std::string& out, unsigned code) {
        if (code < 0x80) {
            out.push_back(char(code));
        } else if (code < 0x800) {
            out.push_back(char(0xC0 | (code >> 6)));
            out.push_back(char(0x80 | (code & 0x3F)));
        } else {
            out.push_back(char(0xE0 | (code >> 12)));
            out.push_back(char(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(char(0x80 | (code & 0x3F)));
        }
    }

    std::optional<std::string> parse_string() {
        if (!consume('"')) return std::nullopt;
        std::string out;
        while (pos_ < text_.size()) {
            char c = text_[pos_++];
            if (c == '"') return out;
            if (c != '\\') { out.push_back(c); continue; }
            if (pos_ >= text_.size()) return std::nullopt;
            char esc = text_[pos_++];
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (pos_ + 4 > text_.size()) return std::nullopt;
                    std::string hex(text_.substr(pos_, 4));
                    char* end = nullptr;
                    unsigned code = unsigned(std::strtoul(hex.c_str(), &end, 16));
                    if (end != hex.c_str() + 4) return std::nullopt;
                    pos_ += 4;
                    append_utf8(out, code);
                    break;
                }
                default: return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::optional<Json> parse_array(int depth) {
        consume('[');
        Json result = Json::array();
        skip_space();
        if (consume(']')) return result;
        while (true) {
            auto item = parse_value(depth + 1);
            if (!item) return std::nullopt;
            result.items.push_back(std::move(*item));
            skip_space();
            if (consume(']')) return result;
            if (!consume(',')) return std::nullopt;
        }
    }

    std::optional<Json> parse_object(int depth) {
        consume('{');
        Json result = Json::object();
        skip_space();
        if (consume('}')) return result;
        while (true) {
            skip_space();
            auto key = parse_string();
            if (!key) return std::nullopt;
            skip_space();
            if (!consume(':')) return std::nullopt;
            auto value = parse_value(depth + 1);
            if (!value) return std::nullopt;
            if (result.fields.count(*key)) return std::nullopt;  // duplicate keys are errors
            result.fields.emplace(std::move(*key), std::move(*value));
            skip_space();
            if (consume('}')) return result;
            if (!consume(',')) return std::nullopt;
        }
    }

    std::string_view text_;
    std::size_t pos_ = 0;
};

void dump_string(std::string& out, const std::string& s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(char(c));
                }
        }
    }
    out.push_back('"');
}

void dump(std::string& out, const Json& v) {
    switch (v.type) {
        case Json::Type::Null: out += "null"; break;
        case Json::Type::Bool: out += v.boolean ? "true" : "false"; break;
        case Json::Type::Number: {
            char buf[40];
            if (v.number == std::floor(v.number) && std::fabs(v.number) < 1e15)
                std::snprintf(buf, sizeof buf, "%.0f", v.number);
            else
                std::snprintf(buf, sizeof buf, "%.10g", v.number);
            out += buf;
            break;
        }
        case Json::Type::String: dump_string(out, v.text); break;
        case Json::Type::Array: {
            out.push_back('[');
            bool first = true;
            for (const Json& item : v.items) {
                if (!first) out.push_back(',');
                first = false;
                dump(out, item);
            }
            out.push_back(']');
            break;
        }
        case Json::Type::Object: {
            out.push_back('{');
            bool first = true;
            for (const auto& [key, item] : v.fields) {
                if (!first) out.push_back(',');
                first = false;
                dump_string(out, key);
                out.push_back(':');
                dump(out, item);
            }
            out.push_back('}');
            break;
        }
    }
}

}  // namespace

std::optional<Json> json_parse(std::string_view text) { return Parser(text).run(); }

std::string json_dump(const Json& value) {
    std::string out;
    dump(out, value);
    return out;
}

}  // namespace sw
