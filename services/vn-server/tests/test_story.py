"""Host tests for the VN content server (stdlib only, no FastAPI needed).

Run from the vn-server directory:

    python -m unittest discover -s tests -v
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from renpy2vn import Unsupported, convert, parse_statements  # noqa: E402
from vn_server.story import Story  # noqa: E402


class StoryGraphTests(unittest.TestCase):
    def setUp(self) -> None:
        self.story = Story(ROOT / "content" / "demo" / "story.json")

    def test_every_target_resolves(self) -> None:
        ids = set(self.story.node_ids())
        self.assertIn(self.story.start, ids)
        for node_id in self.story.node_ids():
            node = self.story.get(node_id)
            if node.next:
                self.assertIn(node.next, ids, f"{node_id}.next -> {node.next}")
            if node.kind == "choice":
                self.assertTrue(node.options, f"{node_id} has no options")
                for option in node.options:
                    self.assertIn(str(option.get("next", "")), ids, f"{node_id} option")

    def test_kinds_and_text(self) -> None:
        for node_id in self.story.node_ids():
            node = self.story.get(node_id)
            self.assertIn(node.kind, {"say", "scene", "sprite", "choice", "end"})
            if node.kind in ("say", "end"):
                self.assertTrue(node.text, f"{node_id} has empty text")
            for value in (node.who, node.text, node.bg):
                self.assertNotIn("\n", value, f"{node_id} text must be single line")

    def test_reaches_an_ending(self) -> None:
        node = self.story.get(self.story.start)
        for _ in range(50):
            if node.kind == "end":
                return
            target = node.next or (node.options[0]["next"] if node.kind == "choice" else "")
            self.assertTrue(target, f"dead end at {node.id}")
            node = self.story.get(target)
        self.fail("graph did not reach an end node within 50 steps")

    def test_wire_format(self) -> None:
        wire = self.story.to_wire(self.story.get(self.story.start))
        self.assertTrue(wire.startswith(f"NODE {self.story.start}"))
        self.assertIn("KIND ", wire)
        self.assertTrue(wire.endswith("\n\n"))


class ConverterTests(unittest.TestCase):
    def test_demo_source_converts(self) -> None:
        source = (ROOT / "content" / "demo" / "source" / "game.rpy").read_text(encoding="utf-8")
        document = convert(parse_statements(source.splitlines()), "t")
        kinds = {node["kind"] for node in document["nodes"].values()}
        self.assertIn("choice", kinds)
        choice = next(n for n in document["nodes"].values() if n["kind"] == "choice")
        self.assertEqual(len(choice["options"]), 2)
        self.assertEqual(document["start"], "start")

    def test_unsupported_is_reported(self) -> None:
        with self.assertRaises(Unsupported):
            convert(parse_statements(["label start:", "    jump nowhere"]), "t")


class SceneTests(unittest.TestCase):
    def test_strip_sizes(self) -> None:
        from vn_server import scenes

        self.assertEqual(len(scenes.strip("bg_hall", 0, 16)), scenes.WIDTH * 16 * 2)
        frame = b"".join(
            scenes.strip("bg_hall", y, 16) for y in range(0, scenes.HEIGHT, 16)
        )
        self.assertEqual(len(frame), scenes.WIDTH * scenes.HEIGHT * 2)

    def test_strip_rejects_out_of_range(self) -> None:
        from vn_server import scenes

        with self.assertRaises(scenes.SceneError):
            scenes.strip("bg_hall", scenes.HEIGHT - 4, 16)
        with self.assertRaises(scenes.SceneError):
            scenes.strip("bg_hall", 0, 0)

    def test_sprite_composites_when_available(self) -> None:
        from vn_server import scenes

        if not getattr(scenes, "_HAVE_PIL", False) or scenes._find_asset("sprites", "hero") is None:
            self.skipTest("Pillow or the sample sprite is not available")
        plain = scenes.strip("bg_watch", 0, scenes.HEIGHT)
        with_sprite = scenes.strip("bg_watch", 0, scenes.HEIGHT, "hero")
        self.assertEqual(len(plain), len(with_sprite))
        self.assertNotEqual(plain, with_sprite)


if __name__ == "__main__":
    unittest.main()
