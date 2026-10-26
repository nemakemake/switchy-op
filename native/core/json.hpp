// A small JSON value type: enough for the protocol messages and profile files.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sw {

struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string text;
    std::vector<Json> items;
    std::map<std::string, Json> fields;

    Json() = default;
    static Json object() { Json j; j.type = Type::Object; return j; }
    static Json array() { Json j; j.type = Type::Array; return j; }
    static Json of(bool v) { Json j; j.type = Type::Bool; j.boolean = v; return j; }
    static Json of(double v) { Json j; j.type = Type::Number; j.number = v; return j; }
    static Json of(int v) { return of(double(v)); }
    static Json of(const std::string& v) { Json j; j.type = Type::String; j.text = v; return j; }
    static Json of(const char* v) { return of(std::string(v)); }

    bool is_object() const { return type == Type::Object; }

    // Object access; operator[] turns a null value into an object and creates the field.
    Json& operator[](const std::string& key);
    const Json* find(const std::string& key) const;
    void push(Json value);

    std::string string_or(const std::string& key, const std::string& fallback) const;
    double number_or(const std::string& key, double fallback) const;
    bool bool_or(const std::string& key, bool fallback) const;
};

std::optional<Json> json_parse(std::string_view text);

// Compact output with object keys in sorted order.
std::string json_dump(const Json& value);

}  // namespace sw
