# 「本护照」UI 参考图

社区固件 **FoloToy AI Passport「本护照」**（`ai-passport-5`，作者 york_Z）的界面截图。
用于对照 `docs/usage-monitor-design.md` 里的设计规范。

## 来源

`page1`~`page4` 由设备 **串口截屏协议** 抓取（把我们逆向出的协议用在了原固件上）：

```
电脑 → 设备:  "FAP_SCREENSHOT_V1\n"
设备 → 电脑:  "FAP_SCREENSHOT_V1 240 320 RGB565LE 153600\n" + 153600 字节 RGB565 小端
```

所以这些是**设备帧缓冲的原始像素**（240×320，16 色索引），不是渲染或翻拍。

`cover-official-hd.webp` 来自社区平台的作品封面，是第 1 页的 **900×1200 高清渲染图**（3.75×），
带抗锯齿，可作为字体/线条的放大参考。

## 文件

| 文件 | 页面 | 说明 |
| --- | --- | --- |
| `page1-identity.png` | 1/4 资料页 | PASSPORT / NAME / FOCUS / CLAUDE CODE / MRZ |
| `page2-focus.png` | 2/4 焦点页 | FOCUS*30D（无数据状态，所以很空）|
| `page3-log.png` | 3/4 日志页 | LOG*13 WEEKS / 热力图 / DAYS·TOKENS·SYNC / LESS-MORE |
| `page4-stamps.png` | 4/4 章页 | STAMPS / 章位网格 / FIRST SEEN PER THEME / ISSUED / REV A |
| `cover-official-hd.webp` | 1/4 高清 | 官方封面，900×1200 |

> 第 2 页画面很空，是因为设备**没同步到数据**（`NO DATA` / `CONNECT MAC TO SYNC`）。
> 有数据时该页会显示 30 天活跃天数、13 周热力图、天数/令牌/连续天数等。

## 注意

这些图是**第三方作者的作品**，仅作学习与设计参考，请勿用于再分发或商业用途。
其固件与设计版权归原作者 york_Z 所有。
