"""Input sources that produce the generic InputState used by the bindings engine."""

from collections.abc import Sequence
from typing import Any

from switchy_op.bindings import InputState

_IDLE = InputState()


class ScriptedInput:
    """Plays a timeline of (start_seconds, state); the latest entry that has started applies."""

    def __init__(self, timeline: Sequence[tuple[float, InputState]]) -> None:
        self._timeline = sorted(timeline, key=lambda entry: entry[0])

    def at(self, t: float) -> InputState:
        current = _IDLE
        for start, state in self._timeline:
            if start > t:
                break
            current = state
        return current


# Xbox-style layout as SDL reports it for most gamepads. A Nintendo controller labels A/B and X/Y
# the other way round, so adjust PAD_BUTTONS if the buttons feel swapped.
PAD_AXES = {0: "left_x", 1: "left_y", 2: "right_x", 3: "right_y"}
PAD_TRIGGERS = {4: "zl", 5: "zr"}
PAD_BUTTONS = {
    0: "a", 1: "b", 2: "x", 3: "y", 4: "l", 5: "r", 6: "minus", 7: "plus", 8: "l3", 9: "r3",
}  # fmt: skip

KEYBOARD_BUTTONS = {
    "space": "zr", "l": "y", "h": "b", "x": "x", "m": "minus", "e": "plus",
}  # fmt: skip


class PygameInput:
    """Reads the first gamepad, or the keyboard when none is connected, and draws a small HUD.

    Keyboard: W/S forward and back, A/D turn, Space is the dead-man switch, L light, H horn,
    X arm (hold), M emergency stop, Esc quits.
    """

    def __init__(self) -> None:
        import pygame

        self._pg: Any = pygame
        pygame.init()
        pygame.joystick.init()
        pygame.display.set_caption("switchy-op")
        self._screen = pygame.display.set_mode((720, 360))
        self._font = pygame.font.SysFont("consolas", 18)
        self._pad = pygame.joystick.Joystick(0) if pygame.joystick.get_count() else None
        if self._pad is not None:
            self._pad.init()
        self.source = self._pad.get_name() if self._pad is not None else "keyboard"

    def poll(self) -> InputState | None:
        """Return the current input, or None when the window was closed or Esc was pressed."""
        pg = self._pg
        for event in pg.event.get():
            if event.type == pg.QUIT or (event.type == pg.KEYDOWN and event.key == pg.K_ESCAPE):
                return None
        return self._read_pad() if self._pad is not None else self._read_keyboard()

    def _read_pad(self) -> InputState:
        pad: Any = self._pad
        axes = {name: float(pad.get_axis(i)) for i, name in PAD_AXES.items()}
        count = pad.get_numbuttons()
        buttons = {name for i, name in PAD_BUTTONS.items() if i < count and pad.get_button(i)}
        buttons |= {name for i, name in PAD_TRIGGERS.items() if pad.get_axis(i) > 0.0}
        return InputState(axes, frozenset(buttons))

    def _read_keyboard(self) -> InputState:
        pg = self._pg
        keys = pg.key.get_pressed()

        def axis(negative: int, positive: int) -> float:
            return float(keys[positive]) - float(keys[negative])

        axes = {"left_y": axis(pg.K_w, pg.K_s), "right_x": axis(pg.K_a, pg.K_d)}
        buttons = {name for key, name in KEYBOARD_BUTTONS.items() if keys[pg.key.key_code(key)]}
        return InputState(axes, frozenset(buttons))

    def draw(self, lines: Sequence[str]) -> None:
        self._screen.fill((16, 18, 24))
        for row, text in enumerate(lines):
            surface = self._font.render(text, True, (220, 225, 235))
            self._screen.blit(surface, (16, 12 + row * 24))
        self._pg.display.flip()

    def close(self) -> None:
        self._pg.quit()
