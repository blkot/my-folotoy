"""FastAPI application exposing a visual-novel node graph to the device.

Endpoints
    GET /v1/health              server + story status
    GET /v1/game                title, version, start node
    GET /v1/node/{id}           compact wire text (default) or JSON (?fmt=json)

The device only ever asks for one node at a time and follows the node graph
itself, so no session state lives here.
"""

from __future__ import annotations

import os
from pathlib import Path

from fastapi import FastAPI, HTTPException, Query
from fastapi.responses import PlainTextResponse, Response

from . import scenes
from .story import Story, StoryError

CONTENT_ROOT = Path(
    os.environ.get("VN_CONTENT_ROOT", Path(__file__).resolve().parent.parent / "content")
)
GAME = os.environ.get("VN_GAME", "demo")


def create_app() -> FastAPI:
    application = FastAPI(title="FoloToy AI Passport VN server", version="0.1.0")

    story: Story | None = None
    load_error: str | None = None
    try:
        story = Story(CONTENT_ROOT / GAME / "story.json")
    except StoryError as error:
        load_error = str(error)

    def require_story() -> Story:
        if story is None:
            raise HTTPException(status_code=503, detail=load_error or "story unavailable")
        return story

    @application.get("/v1/health")
    def health() -> dict:
        current = require_story()
        return {"ok": True, "game": GAME, "nodes": len(current.node_ids()), **current.info()}

    @application.get("/v1/game")
    def game() -> dict:
        current = require_story()
        return {"game": GAME, **current.info()}

    @application.get("/v1/node/{node_id}")
    def node(node_id: str, fmt: str = Query("text", pattern="^(text|json)$")):
        current = require_story()
        try:
            requested = current.get(node_id)
        except KeyError:
            raise HTTPException(status_code=404, detail=f"unknown node '{node_id}'") from None
        if fmt == "json":
            return current.to_json(requested)
        return PlainTextResponse(current.to_wire(requested), media_type="text/plain; charset=utf-8")

    @application.get("/v1/scene/{scene_id}")
    def scene_info(scene_id: str) -> dict:
        return scenes.info(scene_id)

    @application.get("/v1/scene/{scene_id}/strip")
    def scene_strip(
        scene_id: str,
        y: int = Query(0, ge=0),
        h: int = Query(16, ge=1, le=64),
        sprite: str = Query(""),
        sx: int | None = Query(None),
        sy: int | None = Query(None),
    ) -> Response:
        try:
            data = scenes.strip(scene_id, y, h, sprite, sx, sy)
        except scenes.SceneError as error:
            raise HTTPException(status_code=400, detail=str(error)) from None
        return Response(content=data, media_type="application/octet-stream")

    return application


app = create_app()
