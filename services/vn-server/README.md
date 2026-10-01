<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# VN content server

The server half of the AI Passport visual-novel system. It stores unpacked
Ren'Py content, converts it into a flat node graph, and serves one node at a
time to the device over HTTP. The firmware holds **no** story text or assets.

The server is stateless: the device follows `next` and option targets itself,
so nothing here tracks a session.

## Layout

```text
vn-server/
  vn_server/app.py        FastAPI application (endpoints)
  vn_server/story.py      node graph loading + wire/JSON serialisation
  renpy2vn.py             minimal Ren'Py -> node graph converter
  content/<game>/
    source/*.rpy          the "unpacked Ren'Py" source (human-authored)
    story.json            converted node graph actually served
  tests/test_story.py     stdlib host tests (no FastAPI required)
```

## Run

```sh
python -m venv .venv
.venv/Scripts/pip install -r requirements.txt      # Windows
# .venv/bin/pip install -r requirements.txt        # Linux/macOS
.venv/Scripts/python -m uvicorn vn_server.app:app --host 0.0.0.0 --port 8080
```

Bind to `0.0.0.0` so the device on the same LAN can reach it, and allow the
port through the host firewall.

## Endpoints

| Endpoint | Purpose |
| --- | --- |
| `GET /v1/health` | liveness + story summary |
| `GET /v1/game` | title, version, start node |
| `GET /v1/node/{id}` | compact wire text (device default) |
| `GET /v1/node/{id}?fmt=json` | JSON form for debugging |

Wire format, one `KEY value` per line, blank line terminates:

```text
NODE <id>
KIND say|scene|choice|end
WHO <speaker>          (optional)
TEXT <utf-8 text>      (optional)
BG <scene id>          (optional)
NEXT <id>              (optional)
OPT <next id> <label>  (0..N, choice)
```

## Rebuild the node graph

```sh
python renpy2vn.py content/demo/source/game.rpy content/demo/story.json --title "Demo"
```

The converter only supports the linear + single-choice subset (see its
docstring); anything else is reported as `unsupported` rather than dropped.

## Tests

```sh
python -m unittest discover -s tests -v
```
