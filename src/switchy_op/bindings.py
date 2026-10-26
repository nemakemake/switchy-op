"""Turns controller input into drive commands and action requests, driven by a profile file.

The engine only knows generic names: axes left_x, left_y, right_x, right_y (-1..1, stick up is
negative like on most gamepads) and buttons a, b, x, y, l, r, zl, zr, plus, minus, l3, r3,
up, down, left, right. A platform layer maps its real controller onto them.
"""

from collections.abc import Mapping
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Literal

import yaml
from pydantic import BaseModel, ConfigDict, Field, model_validator


class _Strict(BaseModel):
    model_config = ConfigDict(extra="forbid")


class RobotTarget(_Strict):
    host: str = "127.0.0.1"
    port: int = Field(7000, ge=1, le=65535)
    key: str = "dev-key"


class AxisBinding(_Strict):
    axis: str
    invert: bool = False
    deadzone: float = Field(0.1, ge=0, lt=1)
    expo: float = Field(0.0, ge=0, le=1)
    scale: float = Field(1.0, gt=0, le=1)


class DriveBinding(_Strict):
    forward: AxisBinding
    turn: AxisBinding
    deadman: str | None = None


class ActionBinding(_Strict):
    """trigger: press fires once, hold is active while held (sent in the stream),
    hold_for fires once after the button was held for `seconds`.

    For switches, `mirror` names the robot state field to flip, otherwise `value` is sent.
    """

    button: str
    trigger: Literal["press", "hold", "hold_for"] = "press"
    op: Literal["set", "start"] = "set"
    value: bool | None = None
    mirror: str | None = None
    seconds: float = Field(1.0, gt=0)
    confirm: bool = False

    @model_validator(mode="after")
    def _consistent(self) -> ActionBinding:
        if self.trigger == "hold" and (self.mirror or self.value is not None):
            raise ValueError("a hold action carries no value")
        if (
            self.op == "set"
            and self.trigger != "hold"
            and self.mirror is None
            and self.value is None
        ):
            raise ValueError("a set action needs `mirror` or `value`")
        return self


class Profile(_Strict):
    robot: RobotTarget = Field(default_factory=RobotTarget)
    drive: DriveBinding
    actions: dict[str, ActionBinding] = {}


def load_profile(path: str | Path) -> Profile:
    return Profile.model_validate(yaml.safe_load(Path(path).read_text(encoding="utf-8")))


@dataclass(frozen=True, slots=True)
class InputState:
    axes: Mapping[str, float] = field(default_factory=dict)
    buttons: frozenset[str] = frozenset()


@dataclass(frozen=True, slots=True)
class Request:
    name: str
    op: str
    value: bool | None = None
    params: dict[str, Any] | None = None


@dataclass(frozen=True, slots=True)
class Output:
    vx: float
    wz: float
    deadman: bool
    held: frozenset[str]
    requests: tuple[Request, ...]


def shape(raw: float, binding: AxisBinding) -> float:
    """Dead zone, rescale so output starts from zero, then blend in a cubic for fine control."""
    value = -raw if binding.invert else raw
    magnitude = abs(value)
    if magnitude <= binding.deadzone:
        return 0.0
    magnitude = min(1.0, (magnitude - binding.deadzone) / (1.0 - binding.deadzone))
    magnitude = (1.0 - binding.expo) * magnitude + binding.expo * magnitude**3
    return magnitude * binding.scale * (1.0 if value > 0 else -1.0)


def _pressed(button: str, inp: InputState) -> bool:
    return all(part in inp.buttons for part in button.split("+"))


class Bindings:
    def __init__(self, profile: Profile) -> None:
        self._profile = profile
        self._was_down: dict[str, bool] = {}
        self._down_since: dict[str, float] = {}
        self._fired: set[str] = set()

    def update(self, inp: InputState, now: float, state: Mapping[str, Any]) -> Output:
        drive = self._profile.drive
        deadman = True if drive.deadman is None else _pressed(drive.deadman, inp)
        vx = shape(inp.axes.get(drive.forward.axis, 0.0), drive.forward)
        wz = shape(inp.axes.get(drive.turn.axis, 0.0), drive.turn)

        held: set[str] = set()
        requests: list[Request] = []
        for name, binding in self._profile.actions.items():
            down = _pressed(binding.button, inp)
            was_down = self._was_down.get(name, False)
            self._was_down[name] = down
            if binding.trigger == "hold":
                if down:
                    held.add(name)
            elif binding.trigger == "press":
                if down and not was_down:
                    requests.append(self._request(name, binding, state))
            elif not down:
                self._down_since.pop(name, None)
                self._fired.discard(name)
            else:
                started = self._down_since.setdefault(name, now)
                if now - started >= binding.seconds and name not in self._fired:
                    self._fired.add(name)
                    requests.append(self._request(name, binding, state))
        return Output(vx, wz, deadman, frozenset(held), tuple(requests))

    @staticmethod
    def _request(name: str, binding: ActionBinding, state: Mapping[str, Any]) -> Request:
        params = {"confirmed": True} if binding.confirm else None
        if binding.op == "start":
            return Request(name, "start", None, params)
        if binding.mirror is not None:
            return Request(name, "set", not bool(state.get(binding.mirror)), params)
        return Request(name, "set", binding.value, params)
