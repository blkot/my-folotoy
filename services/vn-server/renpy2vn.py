#!/usr/bin/env python3
"""Minimal Ren'Py -> runtime node graph converter.

Supports only the linear + single-choice subset needed for milestone 1:

    label <name>:
        scene <bg>
        "<text>"                 -> say, no speaker
        <speaker> "<text>"       -> say with speaker
        menu:
            "<option>":
                jump <label>
        jump <label>
        return                   -> end node

Anything outside this subset raises ``Unsupported`` instead of being dropped
silently. Each label's first statement must produce a node (``say``/``scene``/
``menu``) so entry ids stay deterministic.

Usage:
    python renpy2vn.py content/demo/source/game.rpy content/demo/story.json \
        --title "展厅的黄昏"
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

QUOTED = re.compile(r'^(?:(?P<speaker>[A-Za-z_][\w.]*)\s+)?"(?P<text>.*)"\s*$')
LABEL = re.compile(r"^label\s+(?P<name>[A-Za-z_][\w.]*)\s*:\s*$")
MENU = re.compile(r"^menu\s*:\s*$")
OPTION = re.compile(r'^"(?P<text>.*)"\s*:\s*$')


class Unsupported(Exception):
    """Raised for Ren'Py constructs this converter does not implement."""


def entry_id(label: str) -> str:
    return "start" if label == "start" else f"{label}_1"


def node_id(label: str, index: int) -> str:
    return "start" if (label == "start" and index == 0) else f"{label}_{index + 1}"


def parse_statements(lines: list[str]) -> dict[str, list[tuple[str, dict]]]:
    """Return {label: [statement, ...]} where statement is (type, payload)."""
    labels: dict[str, list[tuple[str, dict]]] = {}
    current: str | None = None
    index = 0
    while index < len(lines):
        raw = lines[index]
        stripped = raw.split("#", 1)[0].rstrip()
        index += 1
        if not stripped.strip():
            continue

        match = LABEL.match(stripped.strip())
        if match:
            current = match.group("name")
            labels[current] = []
            continue
        if current is None:
            raise Unsupported(f"statement before any label: {stripped.strip()}")

        text = stripped.strip()
        if MENU.match(text):
            options: list[dict] = []
            current_option: dict | None = None
            while index < len(lines) and lines[index].strip():
                body = lines[index].split("#", 1)[0].strip()
                index += 1
                option = OPTION.match(body)
                if option:
                    current_option = {"text": option.group("text")}
                    options.append(current_option)
                    continue
                if body.startswith("jump "):
                    if current_option is None:
                        raise Unsupported(f"jump outside a menu option: {body}")
                    current_option["next_target"] = body.split(None, 1)[1].strip()
                    continue
                raise Unsupported(f"unsupported menu body: {body}")
            for option in options:
                if "next_target" not in option:
                    raise Unsupported(f"menu option without jump: {option['text']}")
            if not options:
                raise Unsupported("menu without options")
            labels[current].append(("menu", {"options": options}))
            continue
        if text == "return":
            labels[current].append(("return", {}))
            continue
        if text.startswith("scene "):
            labels[current].append(("scene", {"bg": text.split(None, 1)[1].strip()}))
            continue
        if text.startswith("show "):
            labels[current].append(("sprite", {"sprite": text.split(None, 1)[1].strip()}))
            continue
        if text == "hide" or text.startswith("hide "):
            labels[current].append(("sprite", {"sprite": ""}))
            continue
        if text.startswith("jump "):
            labels[current].append(("jump", {"target": text.split(None, 1)[1].strip()}))
            continue
        match = QUOTED.match(text)
        if match:
            labels[current].append(
                ("say", {"who": match.group("speaker") or "", "text": match.group("text")})
            )
            continue
        raise Unsupported(f"unsupported statement: {text}")
    return labels


def convert(labels: dict[str, list[tuple[str, dict]]], title: str) -> dict:
    for label, statements in labels.items():
        if not statements:
            raise Unsupported(f"label '{label}' is empty")
        if statements[0][0] not in ("say", "scene", "sprite", "menu"):
            raise Unsupported(f"label '{label}' must start with say/scene/show/menu")

    nodes: dict[str, dict] = {}
    pending_jump: list[tuple[str, str]] = []  # (source node id, target label)

    for label, statements in labels.items():
        node_index = 0
        previous: str | None = None
        for kind, payload in statements:
            if kind == "jump":
                if previous is None:
                    raise Unsupported(f"label '{label}' starts with jump")
                pending_jump.append((previous, payload["target"]))
                previous = None
                continue
            if kind == "return":
                nid = node_id(label, node_index)
                node_index += 1
                nodes[nid] = {"kind": "end", "text": "—— 完 ——"}
                if previous is not None:
                    nodes[previous]["next"] = nid
                previous = None
                continue

            nid = node_id(label, node_index)
            node_index += 1
            if previous is not None:
                nodes[previous]["next"] = nid
            if kind == "say":
                node = {"kind": "say", "text": payload["text"]}
                if payload["who"]:
                    node["who"] = payload["who"]
            elif kind == "scene":
                node = {"kind": "scene", "bg": payload["bg"]}
            elif kind == "sprite":
                node = {"kind": "sprite"}
                if payload["sprite"]:
                    node["sprite"] = payload["sprite"]
            else:  # menu
                node = {
                    "kind": "choice",
                    "options": [
                        {"text": option["text"], "target": option["next_target"]}
                        for option in payload["options"]
                    ],
                }
            nodes[nid] = node
            previous = nid

        if (
            previous is not None
            and nodes[previous]["kind"] in ("say", "scene", "sprite")
            and not nodes[previous].get("next")
        ):
            # A label that just runs off the end behaves like `return`.
            nid = node_id(label, node_index)
            nodes[nid] = {"kind": "end", "text": "—— 完 ——"}
            nodes[previous]["next"] = nid

    for source, target in pending_jump:
        if target not in labels:
            raise Unsupported(f"jump to unknown label '{target}'")
        nodes[source]["next"] = entry_id(target)

    for node in nodes.values():
        if node["kind"] == "choice":
            resolved = []
            for option in node["options"]:
                target = option["target"]
                if target not in labels:
                    raise Unsupported(f"choice target '{target}' is not a label")
                resolved.append({"text": option["text"], "next": entry_id(target)})
            node["options"] = resolved

    return {"title": title, "version": 1, "start": entry_id("start"), "nodes": nodes}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--title", default="Untitled")
    args = parser.parse_args(argv)

    try:
        labels = parse_statements(args.source.read_text(encoding="utf-8").splitlines())
        document = convert(labels, args.title)
    except Unsupported as error:
        print(f"unsupported: {error}", file=sys.stderr)
        return 2
    except OSError as error:
        print(f"cannot read {args.source}: {error}", file=sys.stderr)
        return 1

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(document, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(f"wrote {len(document['nodes'])} nodes to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
