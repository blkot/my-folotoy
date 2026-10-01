"""Load a visual-novel node graph and serialise it for the device.

The graph is a JSON document produced from unpacked Ren'Py sources by
``renpy2vn.py``. The server is intentionally **stateless**: the client follows
``next`` and option targets itself, so nothing here tracks a session.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any


class StoryError(Exception):
    """Raised when the story document is missing or malformed."""


class Node:
    """A single graph node wrapping its raw mapping."""

    __slots__ = ("id", "raw")

    def __init__(self, node_id: str, raw: dict[str, Any]) -> None:
        self.id = node_id
        self.raw = raw

    @property
    def kind(self) -> str:
        return str(self.raw.get("kind", "say"))

    @property
    def who(self) -> str:
        return str(self.raw.get("who", ""))

    @property
    def text(self) -> str:
        return str(self.raw.get("text", ""))

    @property
    def bg(self) -> str:
        return str(self.raw.get("bg", ""))

    @property
    def sprite(self) -> str:
        return str(self.raw.get("sprite", ""))

    @property
    def next(self) -> str:
        return str(self.raw.get("next", ""))

    @property
    def options(self) -> list[dict[str, Any]]:
        options = self.raw.get("options", [])
        return options if isinstance(options, list) else []


class Story:
    """Validated, read-only node graph."""

    def __init__(self, path: Path) -> None:
        try:
            document = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            raise StoryError(f"cannot load story {path}: {error}") from error
        if not isinstance(document, dict):
            raise StoryError(f"story {path} is not an object")

        self.title = str(document.get("title", "Untitled"))
        self.version = int(document.get("version", 1))
        self.start = str(document.get("start", "start"))

        nodes = document.get("nodes")
        if not isinstance(nodes, dict) or not nodes:
            raise StoryError(f"story {path} has no nodes")
        self._nodes: dict[str, dict[str, Any]] = {
            str(key): value for key, value in nodes.items()
        }
        if not isinstance(self._nodes.get(self.start), dict):
            raise StoryError(f"start node '{self.start}' is missing")

    def info(self) -> dict[str, Any]:
        return {"title": self.title, "version": self.version, "start": self.start}

    def node_ids(self) -> list[str]:
        return list(self._nodes)

    def get(self, node_id: str) -> Node:
        raw = self._nodes.get(node_id)
        if not isinstance(raw, dict):
            raise KeyError(node_id)
        return Node(node_id, raw)

    def to_json(self, node: Node) -> dict[str, Any]:
        """Debug/JSON representation (not used by the constrained device)."""
        body: dict[str, Any] = {"id": node.id, "kind": node.kind}
        if node.who:
            body["who"] = node.who
        if node.text:
            body["text"] = node.text
        if node.bg:
            body["bg"] = node.bg
        if node.next:
            body["next"] = node.next
        if node.kind == "choice":
            body["options"] = [
                {"text": str(option.get("text", "")), "next": str(option.get("next", ""))}
                for option in node.options
            ]
        return body

    def to_wire(self, node: Node) -> str:
        """Compact line protocol consumed by the ESP32 client.

        One ``KEY value`` per line, an empty line terminates the message.
        Text values are UTF-8 and never contain a raw newline.
        """
        lines = [f"NODE {node.id}", f"KIND {node.kind}"]
        if node.who:
            lines.append(f"WHO {node.who}")
        if node.text:
            lines.append(f"TEXT {node.text}")
        if node.bg:
            lines.append(f"BG {node.bg}")
        if node.sprite:
            lines.append(f"SPRITE {node.sprite}")
        if node.next:
            lines.append(f"NEXT {node.next}")
        for option in node.options:
            target = str(option.get("next", ""))
            label = str(option.get("text", ""))
            lines.append(f"OPT {target} {label}".rstrip())
        return "\n".join(lines) + "\n\n"
