<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 视觉小说内容服务器

AI Passport 视觉小说系统的**后端**。它存放解包后的 Ren'Py 内容,转换成扁平的
节点图,再通过网络一次一个节点地下发给设备。固件里**不**保存任何剧本文本或素材。

服务器**无状态**:设备自己跟随 `next` 与选项目标,这里不记录会话。

## 目录结构

```text
vn-server/
  vn_server/app.py        FastAPI 应用(接口)
  vn_server/story.py      节点图加载与 wire/JSON 序列化
  renpy2vn.py             最简 Ren'Py -> 节点图转换器
  content/<game>/
    source/*.rpy          “解包后的 Ren'Py”源文件(人工编写)
    story.json            实际下发的节点图
  tests/test_story.py     标准库 host test(不依赖 FastAPI)
```

## 运行

```sh
python -m venv .venv
.venv/Scripts/pip install -r requirements.txt      # Windows
# .venv/bin/pip install -r requirements.txt        # Linux/macOS
.venv/Scripts/python -m uvicorn vn_server.app:app --host 0.0.0.0 --port 8080
```

绑定 `0.0.0.0` 以便同一局域网内的设备访问,并在主机防火墙上放行该端口。

## 接口

| 接口 | 用途 |
| --- | --- |
| `GET /v1/health` | 存活探测 + 剧本摘要 |
| `GET /v1/game` | 标题、版本、起始节点 |
| `GET /v1/node/{id}` | 紧凑 wire 文本(设备默认) |
| `GET /v1/node/{id}?fmt=json` | 调试用 JSON |

wire 格式:每行一个 `KEY value`,空行结束:

```text
NODE <id>
KIND say|scene|choice|end
WHO <说话人>            (可选)
TEXT <UTF-8 文本>       (可选)
BG <场景 id>            (可选)
NEXT <id>               (可选)
OPT <目标 id> <选项文案> (0..N,choice)
```

## 重新生成节点图

```sh
python renpy2vn.py content/demo/source/game.rpy content/demo/story.json --title "展厅的黄昏"
```

转换器只支持线性 + 单选分支子集(见其文档字符串);其他写法会明确报
`unsupported`,不会静默丢弃。

## 测试

```sh
python -m unittest discover -s tests -v
```
