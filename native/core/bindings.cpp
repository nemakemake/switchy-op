#include "bindings.hpp"

#include <algorithm>
#include <cmath>

namespace sw {
namespace {

bool read_axis(const Json* json, AxisBinding& out, std::string& error, const char* label) {
    if (!json || !json->is_object() || json->string_or("axis", "").empty()) {
        error = std::string("profile: drive.") + label + " needs an axis";
        return false;
    }
    out.axis = json->string_or("axis", "");
    out.invert = json->bool_or("invert", false);
    out.deadzone = json->number_or("deadzone", 0.1);
    out.expo = json->number_or("expo", 0.0);
    out.scale = json->number_or("scale", 1.0);
    if (out.deadzone < 0 || out.deadzone >= 1 || out.expo < 0 || out.expo > 1 || out.scale <= 0 ||
        out.scale > 1) {
        error = std::string("profile: drive.") + label + " has a value out of range";
        return false;
    }
    return true;
}

bool pressed(const std::string& button, const InputState& input) {
    std::size_t start = 0;
    while (true) {
        std::size_t plus = button.find('+', start);
        std::string part = button.substr(start, plus == std::string::npos ? plus : plus - start);
        if (!input.buttons.count(part)) return false;
        if (plus == std::string::npos) return true;
        start = plus + 1;
    }
}

}  // namespace

std::optional<Profile> profile_from_json(const Json& json, std::string& error) {
    Profile profile;
    if (const Json* robot = json.find("robot")) {
        double port = robot->number_or("port", profile.port);
        if (!robot->is_object() || port < 1 || port > 65535 || std::floor(port) != port) {
            error = "profile: invalid robot port";
            return std::nullopt;
        }
        profile.host = robot->string_or("host", profile.host);
        profile.port = std::uint16_t(port);
        profile.key = robot->string_or("key", profile.key);
    }
    const Json* drive = json.find("drive");
    if (!drive || !drive->is_object()) {
        error = "profile: missing drive section";
        return std::nullopt;
    }
    if (!read_axis(drive->find("forward"), profile.forward, error, "forward") ||
        !read_axis(drive->find("turn"), profile.turn, error, "turn"))
        return std::nullopt;
    profile.deadman = drive->string_or("deadman", "");

    if (const Json* actions = json.find("actions")) {
        for (const auto& [name, spec] : actions->fields) {
            ActionBinding action;
            action.name = name;
            action.button = spec.string_or("button", "");
            action.trigger = spec.string_or("trigger", "press");
            action.op = spec.string_or("op", "set");
            if (const Json* value = spec.find("value"); value && value->type == Json::Type::Bool)
                action.value = value->boolean;
            action.mirror = spec.string_or("mirror", "");
            action.seconds = spec.number_or("seconds", 1.0);
            action.confirm = spec.bool_or("confirm", false);
            if (action.button.empty() ||
                (action.trigger != "press" && action.trigger != "hold" &&
                 action.trigger != "hold_for") ||
                (action.op != "set" && action.op != "start")) {
                error = "profile: action '" + name + "' is incomplete or has an unknown trigger/op";
                return std::nullopt;
            }
            profile.actions.push_back(std::move(action));
        }
    }
    return profile;
}

double shape(double raw, const AxisBinding& binding) {
    double value = binding.invert ? -raw : raw;
    double magnitude = std::fabs(value);
    if (magnitude <= binding.deadzone) return 0.0;
    magnitude = std::min(1.0, (magnitude - binding.deadzone) / (1.0 - binding.deadzone));
    magnitude = (1.0 - binding.expo) * magnitude + binding.expo * magnitude * magnitude * magnitude;
    return magnitude * binding.scale * (value > 0 ? 1.0 : -1.0);
}

Output Bindings::update(const InputState& input, double now, const Json& state) {
    Output out;
    out.deadman = profile_.deadman.empty() ? true : pressed(profile_.deadman, input);
    auto axis = [&](const std::string& name) {
        auto it = input.axes.find(name);
        return it == input.axes.end() ? 0.0 : it->second;
    };
    out.vx = shape(axis(profile_.forward.axis), profile_.forward);
    out.wz = shape(axis(profile_.turn.axis), profile_.turn);

    for (const ActionBinding& binding : profile_.actions) {
        bool down = pressed(binding.button, input);
        bool was_down = was_down_[binding.name];
        was_down_[binding.name] = down;

        bool fire = false;
        if (binding.trigger == "hold") {
            if (down) out.held.push_back(binding.name);
        } else if (binding.trigger == "press") {
            fire = down && !was_down;
        } else if (!down) {
            down_since_.erase(binding.name);
            fired_.erase(binding.name);
        } else {
            double started = down_since_.emplace(binding.name, now).first->second;
            if (now - started >= binding.seconds && !fired_.count(binding.name)) {
                fired_.insert(binding.name);
                fire = true;
            }
        }
        if (!fire) continue;

        Request request;
        request.name = binding.name;
        request.op = binding.op;
        request.confirmed = binding.confirm;
        if (binding.op == "set") {
            if (!binding.mirror.empty())
                request.value = !state.bool_or(binding.mirror, false);
            else
                request.value = binding.value;
        }
        out.requests.push_back(std::move(request));
    }
    return out;
}

}  // namespace sw
