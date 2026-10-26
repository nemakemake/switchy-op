// Controller input to drive commands and action requests, mirroring src/switchy_op/bindings.py.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "json.hpp"

namespace sw {

struct AxisBinding {
    std::string axis;
    bool invert = false;
    double deadzone = 0.1;
    double expo = 0.0;
    double scale = 1.0;
};

struct ActionBinding {
    std::string name;
    std::string button;               // "y", or a chord such as "l+a"
    std::string trigger = "press";    // press | hold | hold_for
    std::string op = "set";           // set | start
    std::optional<bool> value;
    std::string mirror;               // robot state field to flip, for switches
    double seconds = 1.0;
    bool confirm = false;
};

struct Profile {
    std::string host = "127.0.0.1";
    std::uint16_t port = 7000;
    std::string key = "dev-key";
    AxisBinding forward;
    AxisBinding turn;
    std::string deadman;
    std::vector<ActionBinding> actions;
};

// Reads the JSON produced by `switchy-op export-profile`. On failure returns nullopt with `error`.
std::optional<Profile> profile_from_json(const Json& json, std::string& error);

// Generic names: axes left_x, left_y, right_x, right_y (-1..1, stick up is negative) and
// buttons a b x y l r zl zr plus minus l3 r3 up down left right.
struct InputState {
    std::map<std::string, double> axes;
    std::set<std::string> buttons;
};

struct Request {
    std::string name;
    std::string op;
    std::optional<bool> value;
    bool confirmed = false;
};

struct Output {
    double vx = 0;
    double wz = 0;
    bool deadman = false;
    std::vector<std::string> held;
    std::vector<Request> requests;
};

double shape(double raw, const AxisBinding& binding);

class Bindings {
public:
    explicit Bindings(Profile profile) : profile_(std::move(profile)) {}
    const Profile& profile() const { return profile_; }

    // `state` is the robot's latest state object, used by switches that mirror a state field.
    Output update(const InputState& input, double now, const Json& state);

private:
    Profile profile_;
    std::map<std::string, bool> was_down_;
    std::map<std::string, double> down_since_;
    std::set<std::string> fired_;
};

}  // namespace sw
