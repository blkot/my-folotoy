# 项目交接文档

> 仓库：`git@github.com:blkot/my-folotoy.git`
> 基线：`main @ 7478ce1`（唯一分支，已推送）
> 日期：2026-10-06
> 面向：接手继续开发的人（或未来的自己）

---

## 0. 一句话

把 **FoloToy AI Passport（ESP32-C3）** 当作一个可编程的小硬件，在上面做**自己的固件集**：
上游仓库以 submodule 固定，我们自己写应用、共享资源、后端服务，并用 CI + 网页一键刷机分发。

---

## 1. 硬件基线

| 项 | 值 |
| --- | --- |
| 芯片 | ESP32-C3（RISC-V，单核，**无 PSRAM**）|
| Flash | 8 MB |
| 屏幕 | 240 × 320，ST7789P3，**无 MISO**（读不回像素）|
| 按键 | 三个键共用 **一个 ADC 引脚**，靠分压区分（不能同时按）|
| 连接 | 芯片原生 USB-Serial/JTAG（`VID:PID = 303A:1001`），无外挂桥接 |
| 串口 | Windows 上是 `COM6`（插拔后可能变）|
| 内存 | 可用堆约 **~320 KB**；BLE 控制器 + NimBLE 会吃掉几十 KB |

**这条约束贯穿所有设计决策**：没有 PSRAM、内存紧，所以能省则省（不要 BLE 的固件就不开 BLE）。

---

## 2. 文件结构

```
my-folotoy/
├─ README.md                     总览 + 构建命令
├─ .gitmodules                   upstream/ai-passport 的 submodule 声明
├─ .gitignore                    忽略 sdkconfig / build / managed_components
│
├─ firmware/                     ★ 所有固件，一个名字一个独立 ESP-IDF 工程
│  ├─ space-key/                 BLE 空格键（设备实测可用）
│  ├─ usage-monitor/             用量监控（设备实测可用）
│  ├─ visual-novel/              视觉小说
│  └─ voice-bot/                 语音 bot（BLE 配网受内存限制）
│
├─ upstream/ai-passport/         上游仓库（submodule，只读）
│  └─ components/bsp/            ← 屏幕/按键/音频/I2C 驱动
│
├─ shared/fonts/vn_font_16.c     GB2312 16px 中文字库（3.5 MB，只存一份）
├─ services/                     各固件的后端（Python）
│  ├─ usage-server/usage_server.py
│  ├─ vn-server/
│  └─ voice-server/
├─ tools/
│  ├─ test.sh                    纯逻辑 host 测试（不需要 ESP-IDF）
│  └─ make_registry.py           从 Release 二进制生成 registry.json
├─ web/                          固件分发页（ESP Web Tools 网页刷机）
│  ├─ index.html
│  └─ firmwares/*.bin            同步下来的合并镜像
├─ docker/                       NAS 上部署分发页
├─ docs/                         设计与调研文档
│  ├─ usage-monitor-design.md    ★「本护照」设计规范
│  ├─ mobile-flash-architecture.md  手机端刷机调研
│  └─ handoff.md                 本文档
└─ .github/workflows/build-firmware.yml   CI
```

### 2.1 固件工程的内部结构（以 `space-key` 为例）

```
firmware/space-key/
├─ CMakeLists.txt          入口：EXTRA_COMPONENT_DIRS 指向上游 BSP
├─ partitions.csv          分区表（nvs / phy_init / factory）
├─ sdkconfig.defaults      入库的配置（芯片、LVGL、BLE…）
├─ sdkconfig               本地生成，含 Wi-Fi 凭据，**不入库**
└─ main/
   ├─ CMakeLists.txt
   ├─ main.c               入口
   ├─ space_key.c/.h       BLE HID 键盘
   ├─ space_ui.c/.h        状态页
   └─ esp_hid_gap.c/.h     GAP/广播胶水（取自 IDF 官方示例）
```

**每个固件都是完整独立的工程** —— 有自己的 `main/`、`partitions.csv`、`sdkconfig.defaults`。工程 `CMakeLists.txt` 只有几行：

```cmake
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../upstream/ai-passport/components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(space-key)
```

### 2.2 固件之间只共享两样东西

| 共享 | 位置 | 方式 |
| --- | --- | --- |
| **BSP 驱动** | `upstream/ai-passport/components/bsp` | submodule + `EXTRA_COMPONENT_DIRS`，**不复制、不改上游** |
| **中文字库** | `shared/fonts/vn_font_16.c` | 各工程编译时 `target_sources(...)` 引用同一份 |

除此之外全自带。新增固件 = 复制一个目录 + 改 `project()` 名 + 加进 CI matrix 和 `web/index.html`。

---

## 3. 四个固件的状态

| 固件 | 用途 | 设备验证 | 备注 |
| --- | --- | --- | --- |
| **space-key** | BLE HID 键盘；按 OK = 在 PC 上敲空格 | ✅ 可用 | 参见 §7 的两个坑 |
| **usage-monitor** | 显示各平台 AI 用量；数据来自局域网 HTTP | ✅ 可用 | 数据源目前是**桩数据** |
| **visual-novel** | 从服务器拉剧情与画面 | 构建通过 | 未做设备验证 |
| **voice-bot** | 按住说话，PC 端识别/回答/朗读 | 构建通过 | **BLE 配网被 C3 内存卡住**，见 §8 |

---

## 4. 后端服务（`services/`）

设备**只认一个 URL**，所以服务可以放 PC / NAS / 云上，换地方不用改固件。

| 服务 | 语言 | 说明 |
| --- | --- | --- |
| `usage-server/usage_server.py` | Python | 用量监控的数据源。当前是**桩 + 随机数**，协议见下 |
| `vn-server/` | Python | 视觉小说的剧情/素材后端 |
| `voice-server/` | Python | 语音 bot 的后端（ASR/LLM/TTS）|

### 用量监控的协议（紧凑行文本，不是 JSON）

设备只有几十 KB 堆，JSON 要 cJSON 加解析树，行文本用 `sscanf` 就够：

```
PLATFORMS 2
PLATFORM opencode-go OpenCode Go
WINDOW 滚动 65 2520 1.3M / 2M tokens
WINDOW 每周 30 259200 12M / 40M tokens
PLATFORM chatgpt-plus ChatGPT Plus
WINDOW 今日 40 3600 40 / 100 条
END
```

解析在 `firmware/usage-monitor/main/usage_model.c`（**纯 C**，可在开发机直接跑测试）。

---

## 5. 构建 / 烧录 / 测试

### 5.1 环境

```powershell
# ESP-IDF v5.5.3,装在:
H:\esp\esp-idf-v5.5.3
& 'H:\esp\esp-idf-v5.5.3\export.ps1'
```

### 5.2 构建 & 烧录单个固件

```powershell
cd firmware/space-key
idf.py set-target esp32c3      # 首次
idf.py build
idf.py -p COM6 flash
idf.py merge-bin -o space-key-full.bin   # 合并镜像，供网页刷机
```

### 5.3 host 测试（不需要 ESP-IDF）

```bash
# 必须用 MSYS2/MinGW bash，否则 gcc 缺 DLL 会静默退出
PATH=/mingw64/bin:$PATH bash tools/test.sh
```

当前两个套件：`test_vn_engine`、`test_usage_model`。

### 5.4 设备日志

固件控制台走 **USB-Serial-JTAG**（`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`），直接读 `COM6` 115200 即可。

> ⚠️ **不要用 DTR/RTS 复位**（实测会把 USB 弄掉，需要拔插）。要重启就让用户**拔插 USB**。

---

## 6. CI 与发布流程

`.github/workflows/build-firmware.yml`：

```
打 tag ──► matrix 并行构建四个固件 ──► merge-bin ──► Release（附 4 个 .bin）
                                                     └─► 生成 registry.json ──► 推到 gh-pages 分支
```

```bash
git tag v0.2.0 && git push origin v0.2.0
```

- 索引地址固定：`https://raw.githubusercontent.com/blkot/my-folotoy/gh-pages/registry.json`
- 用 **gh-pages 分支**而不是 Pages 部署动作：零仓库配置
- **固件二进制不入库**，只走 Release

> 当前只有 `v0.1.0`（不含两个新固件）。`space-key` / `usage-monitor` 还没发过 Release。

---

## 7. 关键经验（踩过的坑）

### 7.1 space-key：报告被 Windows 静默丢弃

**症状**：BLE 连上、加密成功、订阅成功、`notify_tx status=0`，但 PC 不敲空格。

**根因**：
1. 键盘报告描述符声明 **5 个键位（7 字节）**，但发送 **8 字节** —— 长度不匹配，Windows 直接丢弃。
   - 有意思的是 **IDF 官方示例本身也有这个不一致**，它的配套 host 宽松所以没暴露。
   - 正确布局参考 `espressif/esp-iot-solution` 的 keyboard 示例：`modifier + reserved + keycode[6]`。
2. NimBLE 广播路径把 `appearance` 硬编码成 `GENERIC`，忽略了传入的参数 → Windows 不认作键盘。

**修法**：描述符键位数改 6；广播用 `ESP_HID_APPEARANCE_KEYBOARD`。**改完必须让 PC 删除设备后重新配对**（Windows 要重读描述符）。

### 7.2 C3 的 BLE 内存很紧

- BLE + Wi-Fi **不能共存**（voice-bot 的配网就是这么卡住的）
- 只跑 BLE 也建议压配置：`CONFIG_BT_NIMBLE_ROLE_CENTRAL=n`、`OBSERVER=n`、`CONFIG_BT_CTRL_BLE_MAX_ACT=2`
- **esp_hid 的 NimBLE HID 服务需要显式开 `CONFIG_BT_NIMBLE_HID_SERVICE=y`**，否则链接期报 `esp_ble_hidd_dev_init` 未定义（该实现整段被这个开关包着）

### 7.3 LVGL 的小坑

- 用 `LV_MEM_SIZE`（**字节**），不要用已弃用的 `LV_MEM_SIZE_KILOBYTES` —— 两者同时存在会刷上千条警告
- 禁用滚动用 `lv_obj_set_scrollable(obj, false)`，`lv_obj_clear_flag` 在 LVGL 9.6 已弃用

### 7.4 屏幕截图（逆向出来的能力）

社区固件「本护照」内置了一个串口截屏协议，**官方发布流程也要求它**：

```
电脑 → 设备:  "FAP_SCREENSHOT_V1\n"
设备 → 电脑:  "FAP_SCREENSHOT_V1 240 320 RGB565LE 153600\n"  + 153600 字节 RGB565LE
```

- 触发靠**子串匹配**，不依赖换行
- 二进制窗口内**必须静默日志**（否则像素流错位）
- 整屏帧缓冲**静态预留**（运行时堆拿不出连续 150 KB）
- ⚠️ **别高频开关串口**：实测反复 open/close 会让该固件崩溃重启；单次连接复用则稳定

我们的固件**还没有**实现这个协议。若要自己截图，需要加。

---

## 8. 已知阻塞 / 未完成

| 项 | 状态 | 说明 |
| --- | --- | --- |
| **voice-bot BLE 配网** | ❌ 阻塞 | C3 只用 ~41 KB 可分配堆，BLE + Wi-Fi 无法共存（`Malloc failed`）；已文档化，**先搁置** |
| **usage-monitor 数据源** | ⚠️ 桩数据 | `usage_server.py` 现在是随机数；接真实数据见 §9 |
| **usage-server 部署** | ⚠️ 未做 | 目前跑在开发机（`192.168.50.88:8091`），未上 NAS |
| **space-key 配对码** | ⚠️ 已接受 | 固定 `123456` 但**屏幕不显示**（要人告知）。用户明确**不改** |
| **截图协议** | ⚠️ 未实现 | 我们自己固件没加，所以无法远程截图验证界面 |
| **手机刷机** | 📄 仅调研 | 见 `docs/mobile-flash-architecture.md`；Termux + nrflash 待落地 |

---

## 9. 后续建议（按优先级）

1. **接真实用量数据**：`usage_server.py` 换掉桩数据。
   - OpenCode Go **没有官方用量 API**（GitHub issue #16017 / #31084 仍未解决），社区做法是抓工作区页面
   - ChatGPT 额度同理，需自行找数据源
2. **部署 `usage-server` 到 NAS**（有 `docker/` 现成配置），固件里改成 NAS 地址
3. **打 tag 发布**：`git tag v0.2.0 && git push origin v0.2.0` → 网页就能一键刷 `space-key` / `usage-monitor`
4. **给 `usage-monitor` 加截图协议**：这样界面改动可以自己验证，不用人肉看屏
5. **真实数据下验证「本护照」风格界面**：现在空态好看，有数据时的热力图/进度条还没在设备上看过

---

## 10. 速查

```powershell
# 进入 IDF 环境
& 'H:\esp\esp-idf-v5.5.3\export.ps1'

# 构建 + 烧录
cd firmware/<名字>; idf.py build; idf.py -p COM6 flash

# host 测试
& 'C:\msys64\usr\bin\bash.exe' -lc 'cd /h/.../my-folotoy && PATH=/mingw64/bin:$PATH bash tools/test.sh'

# 看设备日志（若日志不起，拔插 USB）
# 直接读 COM6 @ 115200

# 发布
git tag v0.x.0 && git push origin v0.x.0
```

## 11. 相关文档

| 文档 | 内容 |
| --- | --- |
| `README.md` | 仓库总览、构建命令 |
| `docs/usage-monitor-design.md` | ★「本护照」设计规范（配色/网格/字体/母题）|
| `docs/mobile-flash-architecture.md` | 手机端刷机调研（Termux / WebUSB / App）|
| `docs/handoff.md` | 本文档 |
