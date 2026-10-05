# 手机端刷机流水线 · 架构报告

> 调研日期：2026-10-03
> 目标：**把「AI 写码 → CI 出 bin → 手机拉近端固件 → 手机刷进 Passport」做成一条不用电脑参与的闭环**。
> 设备基线：ESP32-C3 / 8 MB Flash / 芯片原生 USB-Serial/JTAG（VID:PID = `303A:1001`）。

---

## 0. 一页结论

| 议题 | 结论 |
| --- | --- |
| 手机刷 Passport 可行吗 | **可行，且是全生态里最容易的一类设备**：C3 原生 USB CDC（`303A:1001`），无外挂桥接芯片，无需私有协议，无需物理 BOOT 键 |
| CI 出固件+发布 | **已就位 90%**：`my-folotoy` 的 `build-firmware.yml` 已经 tag → 构建 → merge-bin → Release；缺一个机器可读的 registry 索引 |
|"远端固件 registry" 要不要自建服务器 | **不要**。GitHub Release + 一个静态 `registry.json`（GitHub Pages 或 raw）就是 registry；release job 追加约 30 行即可生成 |
| 手机端取固件 | 临时：Termux（wget/GitHub API/gh，全部能自动更新）；终态：自建 App 内嵌 `okhttp`，同一个 manifest 直读 |
| 手机端刷机 | 四条路线均可行（见 §2），**推荐双轨**：Termux + nrflash 立即可用；终态自建 App 用 usb-serial-for-android + esp-serial-flasher(C) |
| Hermes/agent 编排 | 手机作为控制面（消息对话/会话管理），桌面 agent 网关（Hermes Agent / OpenCode dashboard 等）做执行面；**固件构建不要跑在手机上**（走 GitHub CI），本地兜底留给 OpenCode/桌面 agent |
| 最大工作量项 | 自建 App 的刷机协议栈（esptool 的 SLIP/STUB 协议），但有现成轮子可拼；**不封锁**，先跑通 Termux 路线再决定是否投入 |

**总体判断**：这条路成立，且你的仓库现状（`my-folotoy` 已经按「submodule + 分发页 + Release」组织）离目标只有三个增量：

1. registry 索引（`registry.json` + release job 生成逻辑）
2. Termux 端可一键执行的取件+刷机脚本
3. （可选投入）自建 Android App

---

## 1. 现状盘点（本仓库已有的东西）

以下全部在本地核实过：

| 资产 | 位置 | 状态 |
| --- | --- | --- |
| 双固件独立 ESP-IDF 工程 | `my-folotoy/firmware/{voice-bot,visual-novel}/` | 完整（main/partitions/sdkconfig 均独立，BSP 走 `EXTRA_COMPONENT_DIRS` 指上游 submodule） |
| CI：tag → 构建 → Release | `my-folotoy/.github/workflows/build-firmware.yml` | 已可产出 `<fw>-full.bin`（含 bootloader+分区表+app），支持 `workflow_dispatch` |
| 固件分发页（ESP Web Tools） | `my-folotoy/web/` + `docker/` | 已设计好（NAS Docker 部署，bin 从 Release 拉） |
| 上游 BSP | `upstream/ai-passport` (submodule) | 锁定 |
| 本机刷机参照命令 | `../环境搭建记录-Windows.md` §7.1 | `python -m esptool --chip esp32c3 -p COM6 -b 460800 write_flash 0x0 build\FoloToy-AI-Passport-full.bin` |
| 归档/校验工具（含 app_desc 解析、full.bin 逐段复核、ELF↔app sha 绑定） | `ai-passport/tools/archive_firmware.py` | 可直接在 release job 里复用 |
| 五个必需开发技能 | `ai-passport/.agents/skills/*` | 已安装（passport-build/setup/develop/device-test/debug） |
| 服务器（NAS） | `docker/compose 已有` | 可放 registry 也可放分发页 |

**注意**：`my-folotoy/firmware/*` 的 Wi-Fi/服务器地址走 Kconfig 且 `sdkconfig` 不进仓库 ——
意味着 **CI 出的是"无凭据默认版"**。要刷给自己用，两种解法：
- (a) 在 CI 里用 repository Secrets 注入 `CONFIG_*`（`idf.py -D SDKCONFIG_DEFAULTS=...` 或在 workflow 里 `echo` 一个附加 defaults 文件）→ Release 直接可刷（推荐，V2 一并做）；
- (b) 本机构建"带配置版"再推进 registry（半自动，V1 可接受）。

---

## 2. 手机端刷机：四条路线对比

皆基于同一物理事实：**Passport 对手机是一个标准 USB CDC-ACM 设备**（`303A:1001`），
Android 的 USB Host API / OTG 可以直接认领它；bootloader 进入由 CDC 的 DTR/RTS 位触发芯片
内部复位完成（等同于 Windows 上 esptool 的免 BOOT 键行为）。

| | A. Termux + nrflash | B. 手机 Chrome (WebUSB) | C. 自建原生 App（推荐终态） | D. Play 商店现成 App |
| --- | --- | --- | --- | --- |
| 安装成本 | 低（F-Droid + pip） | 零（网页） | 高（开发+签名分发） | 低 |
| 取固件 | 可（wget/GitHub API，全自动） | 需浏览器操作 | App 内原生 | 手动选文件 |
| 刷机能力 | 好（含 MD5 校验、stub、多段写入） | 一般（官方 esp-web-tools 不支持 Android，需 Tasmota fork） | **可控可审计** | 看 App 实现 |
| 可信供应链 | 中（pip 随装任何包，建议 lock） | 低（第三方 fork） | 高 | 低 |
| 日志查看 | 可（裸 CDC 读） | 可 | 可（高） | 可 |
| 免电脑完整闭环 | **今天即可** | 需自托管页面 + fork (未验证 C3+303A:1001 全链路) | 可（需开发） | 半自动 |
| 与你设想"简易手机 App"的距离 | 远（脚本不是 App） | 中（仍要网页） | 即设想本身 | 零开发但无 registry 概念 |

**推荐**：A 先行打通流程（今天就能刷），C 作为长期形态按需启动；B 仅作分发页的移动端增强；D 用来**首次验证物理链路**（不用开发就确认"手机 + OTG + Passport"三件事成立）。

---

## 3. 推荐架构（终态）

### 3.1 拓扑

```
┌─────────────── 控制面（手机） ───────────────┐
│  Hermes/agent app（消息/会话/编排）         │
│  ── 或 ── 你未来自建的 Passport App         │
│          │  (取 registry.json + bin)       │
│          ▼                                 │
│  [USB-OTG + 线 + Passport (ESP32-C3 CDC)]  │
└────────────────────────┬───────────────────┘
                         │ USB 2.0（CDC-ACM 双向）
                         ▼
   esptool 协议栈（SLIP + RAM stub + write_flash + MD5 verify）
                         │
                Passport 8MB flash（0x0 全镜像）

   ▲ 取公告/下载 bin
   │
   ├─ https://<you>.github.io/<repo>/registry.json   ← 机器可读索引
   └─ https://github.com/<you>/<repo>/releases       ← 实体 bin（产物）

   ▲ agent 执行构建（不在手机上）
   │
   └─ GitHub Actions（ubuntu + espressif/esp-idf-ci-action + ccache）
        ↑
        │ push / PR merge → tag
        │
   桌面 agent（Hermes / OpenCode / Claude Code：真正改代码 + 跑 host tests 的地方）
```

要点：**手机 = 取件 + 写 flash + 看日志**，不编译、不签发、不回滚；**桌面 = 改码与门禁**；**GitHub = 签发与分发**。任何一端坏了都能用另外一端兜底。

### 3.2 Registry 设计（这是你问的"远端固件 registry"——不重建，寄生在 GitHub）

`registry.json` 由 release job 在发布时自动生成，放到 `gh-pages`（或仓库 `docs/` + GitHub Pages）。
App / Termux 脚本只依赖这一个 URL。

```json
{
  "schema": 1,
  "channel": "stable",
  "generated_at": "<ISO-8601>",
  "firmwares": [
    {
      "id": "voice-bot",
      "name": "语音 Bot",
      "version": "0.3.1",
      "git_tag": "v0.3.1",
      "channel": "stable",
      "target": "esp32c3",
      "chip_family": "ESP32-C3",
      "flash_size_bytes": 8388608,
      "offset": 0,
      "size_bytes": 1589872,
      "sha256": "<full-bin 全镜像 sha256>",
      "url": "https://github.com/<you>/<repo>/releases/download/v0.3.1/voice-bot-full.bin",
      "release_url": "https://github.com/<you>/<repo>/releases/tag/v0.3.1",
      "min_companion_version": null,
      "notes": "<release body 摘要>"
    }
  ]
}
```

字段取舍理由：

- `offset: 0` + `size_bytes` + `sha256` 三元组足够任何 esptool 兼容客户端执行
  `write_flash 0x0 <bin> --verify`，不需要更复杂的 parts 拆分（你走的是 merge-bin 全镜像路线，与 Passport 基线一致：`0x0 / 0x8000 / 0x10000`）。
- `chip_family` / `target` 供 App 与实机 bootloader 检测（`chip_id == 5` 即 ESP32-C3）
  做一致性校验，防呆。
- `notes` / `version` 供 App 展示"这是什么、要不要刷"。
- 上游 `ai-passport/tools/archive_firmware.py` 里已经有 app_desc 提取逻辑（`application_descriptor()`，
  `esp_app_desc_t` 布局 24+8+256 字节，magic `0xABCD5432`），如果以后想自动化从
  bin 里反向读 `project_name/version/idf_ver` 而不是 trusting manifest，直接复用这份字节
  偏移即可（PC/手机两端都可以实现同样的 256 字节解析）。

生成逻辑（放进 `my-folotoy/.github/workflows/build-firmware.yml` 的 release job，一次
commit 即可生效）：

```yaml
      - name: Build registry.json
        run: |
          python3 - <<'PY'
          import json, hashlib, pathlib, os, datetime
          out = {"schema": 1, "channel": "stable",
                 "generated_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                 "firmwares": []}
          tag = os.environ["GITHUB_REF_NAME"]
          repo = os.environ["GITHUB_REPOSITORY"]
          for p in sorted(pathlib.Path("release").glob("*-full.bin")):
              data = p.read_bytes()
              fw = p.name.removesuffix("-full.bin")
              out["firmwares"].append({
                  "id": fw, "name": fw, "version": tag.removeprefix("v"),
                  "git_tag": tag, "channel": "stable", "target": "esp32c3",
                  "chip_family": "ESP32-C3", "flash_size_bytes": 8388608,
                  "offset": 0, "size_bytes": len(data),
                  "sha256": hashlib.sha256(data).hexdigest(),
                  "url": f"https://github.com/{repo}/releases/download/{tag}/{p.name}",
                  "release_url": f"https://github.com/{repo}/releases/tag/{tag}",
              })
          pathlib.Path("site/registry.json").write_text(json.dumps(out, indent=2, ensure_ascii=False))
          PY
      - name: Publish to gh-pages
        # 推荐用一个 pinned 的 peaceiris/actions-gh-release 或官方 pages 工作流
```

> 安全提示：**每次发布都要同时写 Release 和 Pages 两个目标**，release job 的
> `permissions: contents: write` + Pages 的 `contents: read / pages: write` 分开
> 配置，遵循上游 `ai-passport/docs/development/ci/CI-*.md` 的最小权限规范。

### 3.3 手机端形态（两条腿）

**腿 1（今天可用）：Termux 工作流**

```bash
# 一次性安装（F-Droid Termux + F-Droid Termux:API）
pkg install python libusb termux-api
pip install nrflash                # Termux-ESP-Flasher 的 pip 包，原生 CDC 支持 303A:1001

# 取件（任选）：
#  a) GitHub Release 直链
wget -O /tmp/fw.bin https://github.com/<you>/<repo>/releases/download/v0.3.1/voice-bot-full.bin
#  b) gh CLI
gh release download v0.3.1 -R '<you>/<repo>' -p '*-full.bin'

# 校验 + 刷机 + 回读校验
echo "<registry里的sha256>  /tmp/fw.bin" | sha256sum -c
nrflash write --chip esp32c3 --offset 0x0 /tmp/fw.bin --verify
```

局限：Termux 里的 `gh` 需要一次性 PAT（或 gh oauth 流程）；PAT 建议用
**GitHub App / fine-grained PAT，只读 `contents`**，安全边界小。
这是脚本级方案，适合"我现在就能刷"。

**腿 2（投入开发，终态）：自建 Android App（Kotlin + Jetpack Compose）**

模块划分：

```
app/
  ui/          列表 / 详情 / 进度 / 日志 四屏 (Compose + Material3)
  net/         registry.json fetch + GitHub Release 下载 + sha256 校验
  serial/      usb-serial-for-android (CdcAcmSerialDriver, DTR/RTS 控制)
  flasher/     esp-serial-flasher (C, NDK 构建) 或 esptool-js (via WebView/J2V8)
  persist/     已刷历史、偏好
```

协议层两条可选实现，成本/收益：

| 方案 | 优点 | 代价 |
| --- | --- | --- |
| **esp-serial-flasher (C/NDK)** | Espressif 官方、C3 USB-CDC 一等公民、带 stub / MD5 verify / 多段写 / RAM 下载，`esp_loader_io.h` 抽象只要实现 read/write/set DTR-RTS 三个函数 | 需要一套 NDK + JNI glue（约 200-400 行 Kotlin/C） |
| **esptool-js + WebView/QuickJS** | 业务逻辑与 npm 生态同心，官网 demo 即可抄 | 需在 WebView 里暴露一个符合 Web Serial 接口的 bridge，异步边界复杂 |

**数据流（终态一次刷机的完整时序）**：

```
你在手机上对 agent 说  -> "把 voice-bot 最新版带上今早的 bugfix 刷进去"
        ↓
Hermes/OpenCode 桌面 agent 决定动作 -> 建 issue/直接以 agent 身份推 commit 到 my-folotoy
        ↓
GitHub Actions: build → validate.sh --firmware 门禁 → merge-bin → artifact
        ↓ (tag push)
GitHub Release: voice-bot-full.bin 发布 + registry.json 生成 + 部署到 Pages
        ↓
手机端 App (或 Termux) 通知有新固件
        ↓
你插 OTG → App 拉下载 → sha256 校验 → CDC Claim → DTR/RTS 进 bootloader
        ↓
SLIP + RAM stub + write_flash + MD5 verify → 重启进固件
        ↓
App 经 CDC 读启动日志 → 自动验收（关键词"Battery=1 / 主任务 Return"…）
        ↓
回执给你（通知 / 对话流）
```

> 手动模式（不走 agent）同样成立：App 列表 → 点下载 → 点刷写 → 看日志，
> agent 只是这条流水线的**一个**触发器，不是必需品。

---

## 4. agent 编排集成（Hermes / OpenCode / Claude Code 家族）

三种可行形态，按成熟度从高到低：

| | 形态 | 手机参与度 | 可靠性 |
| --- | --- | --- | --- |
| 1 | **手机当远程控制台**：Hermes 类移动 app 通过 Tailscale/LAN 连回桌面网关（Nous Hermes Agent / OpenCode / Claude Code 的 dashboard），对话触发 git push → tag → CI → Release。手机不是执行者，只是界面。 | 低 | 高 |
| 2 | **手机消息路由触发 CI**：Hermes 的 Cron 或消息集成（Telegram/IM）作为上传通道，`/build voice-bot` → webhook → `gh workflow run build-firmware.yml` → 回链。 | 中 | 中 |
| 3 | **手机端 agent 直接驱动刷机**：需要手机上有本地 agent + 串口权限，当前生态还不成熟（Termux 里的 OpenCode 需自己折腾 python/node ABI；Hermes 类客户端多半只做会话/审批转发，不做 USB 硬件 I/O）。 | 高 | 低 |

**推荐 1，逐步爬向 3**：刷机操作本来就低频、依赖物理接线，
把"对话/审批/看日志"留在手机、把"改代码/跑测试/触发 CI"留在桌面 agent（或
Hermes Gateway 类远端编排）是最稳的切分。
真正值得集成到手机 App 里的**只有一件事**：一条"检查 registry → 发现新版本 → 一键刷写 + 看日志"的固定流水线动作，这恰好就是自建 App 的核心价值。

---

## 5. 落地顺序（建议的 P0 → P3）

| 阶段 | 内容 | 产出 | 估计改动量 |
| --- | --- | --- | --- |
| **P0 验证链路** | 手机装 Termux + nrflash，用现有 Release 里任一 full.bin 真刷一次 Passport | 确认「手机 + OTG + C3 CDC」物理通路 + 记录你手机的 OTG/供电表现 | 纯验证，零代码 |
| **P0' CI 增强** | `my-folotoy/build-firmware.yml` 增量：release job 里加 sha256 → registry.json → 部署 GitHub Pages | 机器可读 registry 上线 | ~40 行 yaml + ~50 行 python |
| **P1 Termux 编排脚本** | `tools/mobile/`：`fetch.sh`（curl registry.json → 判断 newest → 下载 → sha256 -c → nrflash --verify），加一个 **Termux:Widget 一键图标**（可绑定到桌面） | 手机上一键「拉最新并刷机」脚本 | ~30 行 shell |
| **P2 手机 App MVP（若判断值得）** | Kotlin App：registry fetch + 版本列表 + usb-serial-for-android + esptool-js(WebView) 简化版（只支持 `write_flash 0x0`） | 可发布给朋友测试的一键刷 Passport 小工具 | ~2-3 天 |
| **P3 完整体验** | 版本对比（on-device app_desc via BLE/GATT 或 serial），自动触发 + 通知 + 日志 tail + 多设备管理 | 与 agent 编排打通的 device fleet 管理器 | 2 周+ |

> 建议**先跑通 P0 + P1**，那里零风险；P2/P3 视 P0/P1 用起来的真实痛点决定要不要做——
> 如果 Termux 满足你了，App 只是小团队分发时才需要。

---

## 6. 风险与开放问题

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| 手机构型/OTG 线质量参差，flash 中途耗电/掉线 → 半写状态 | 可恢复（C3 的 USB-JTAG ROM 兜底仍在，重刷即可救） | `--verify` 每次必做；失败重试一次仍失败才报警 |
| `nrflash`/`Termux-ESP-Flasher` 是个人项目，非 Espressif 官方 | 转协议兼容性仍有边角 | P0 实测；异常时退回 PC + esptool |
| Play 商店 / F-Droid 上现成 ESP32_Flasher 等闭源 App 供应链不可控 | 低（你只出 bin），但可能弹"USB 设备权限" | 仅用于 P0 验证，不作为生产分发 |
| WebUSB 路线（ESP Web Tools fork）跨机型兼容性未充分验证 | 移动分发页不必然可用 | 在 `web/` 页面里提示"手机用户建议用 Termux + nrflash"作为兜底 |
| 无凭据固件（Kconfig 空默认值）刷到非你本人设备 | 体验差，不是安全问题 | CI 里用 repository Secrets 注入 Wi-Fi/服务器配置产出"即刷即用"的个性化 bin，或本机构建后手动上传 Release |
| GitHub Actions 排队/限流 | 触发后延迟可达 3~10 分钟 | 对内可接受；紧急热修复仍走本地构建通道 |
| 你设备上的出厂固件/用户数据（如 WiFi 配置）在 merge-bin 全镜像刷写时会被擦 | 真实且频繁（日常迭代都会遇到） | 想保留 NVS 时改为只刷 `0x10000` app-only bin；或接受每次重配 Wi-Fi |

> ⚠️ 最后一条值得强调：merge-bin `0x0` 全镜像刷写会**连同 NVS 一起覆盖**，不是
> 增量更新。日常反复迭代时若想保留 Wi-Fi 配置等 NVS 数据，可以只刷 app 分区
> （本仓库基线是 `0x10000`，拿到 app-only bin 后用 `nrflash write 0x10000:xxx-app.bin`
> 等）；将来若要"给别人刷"的产品形态，V3 应考虑 partition 增加 `ota_0/ota_1` +
> 配套 BLE/WiFi OTA 通道，让日常迭代不经过 USB。但这超出本报告「USB 刷机骨干」
> 的范围，放在附录 B。

---

## 7. 与现有仓库 / 流程的衔接

- 上游规范（`ai-passport/AGENTS.md`）里"真机测试"部分保持不变：定成
  「**CI 出产物后，USB 线接到手机而不是电脑**」——手机上运行 `nrflash` + 串口日志
  检查，`validate.sh --firmware` 的门禁（build 前置）由 CI 承担，设备侧由手机小工具
  负责"能启动能交互"的验收。
- `my-folotoy/web/` 与本方案**并存不冲突**：电脑用户继续走浏览器 Web Serial（原流程），
  手机用户走 App/Termux，两者都从同一个 Release/registry 取文件。分发页可以补一段
  "手机用户怎么办"文案（`Termux + nrflash` 脚本可直接粘贴）。
- `ai-passport` 上游的 `docs/development/ci/CI-build-and-release.md` 规范（tag 命名、
  双语 changelog、release notes 三要素）在 `my-folotoy` 侧可以按此精神执行但不必强制
  （这是你个人的固件开发 repo，不是上游）。建议 tag 仍按 `vX.Y.Z-<fw>` 命名以保持 app 名
  自明。

---

## 8. 参考资料

- [Termux-ESP-Flasher (nrflash)](https://github.com/7wp81x/Termux-ESP-Flasher) —
  无 root Termux 原生 `.bin` 刷机，明确支持 `303A:1001`（C3/S3/S2 原生 CDC）路径。
- [usb-serial-for-android](https://github.com/mik3y/usb-serial-for-android) —
  Android USB host 串口驱动库（CDC/ACM 分类按接口识别，无需自定义 ProbeTable）。
- [esp-serial-flasher（Espressif 官方 C 库）](https://github.com/espressif/esp-serial-flasher) —
  C3 USB-CDC 目标一等支持（含 stub / MD5 verify / RAM 下载）；v2 公共 API，适合 NDK 集成。
- [esptool-js issue #109 / PR #58](https://github.com/espressif/esptool-js/issues/109) —
  esptool-js 在 Android 上的能力边界 + web-serial-polyfill 的可用性说明（仅验证性；生产仍建议原生库）。
- [Chrome for Android Web Serial 现状](https://developer.chrome.com/docs/capabilities/serial) 与
  [chromestatus](https://chromestatus.com/feature/6043992171085824)（USB 侧至今未打开；
  2025 年仅支持蓝牙 SPP）。有 [Tasmota fork](https://jason2866.github.io/esp-web-tools/) 用 WebUSB 实现了 Android，但属社区 fork。
- [pyserial PR #780](https://github.com/pyserial/pyserial/pull/780) —
  USB 串口在 Android 下通过 Chaquopy + usb-serial-for-android 跑 esptool 的实验性方案。
- `ai-passport` 仓库内：
  [`CI-build-and-release`](../upstream/ai-passport/docs/development/ci/CI-build-and-release.md)、
  [`archive_firmware.py`](../upstream/ai-passport/tools/archive_firmware.py)（app_desc 字节布局）、
  本机实测记录 [`../环境搭建记录-Windows.md`](../环境搭建记录-Windows.md)。

---

## 附录 A：手机端方案生态（快照清单）

| 项目 | 平台 | 协议栈 | 备注 |
| --- | --- | --- | --- |
| nrflash / Termux-ESP-Flasher | Termux | 自带 ROM+stub 协议（直写 USB endpoint） | 内置 `303A:1001`；需 Termux:API |
| ESP32_Flasher（Play 商店） | Android app | 内嵌 esptool Java 移植 | 闭源；P0 验证用 |
| ESP Web Tools (官方) | 桌面 Chrome/Edge | esptool-js + Web Serial | 无 Android |
| ESP Web Tools (Tasmota fork) | Android Chrome | esptool-js + WebUSB | 支持 Android；需自托管 |
| usb-serial-for-android + esp-serial-flasher | Android 原生 | C 库 + JVM 串口桥 | 最稳终态（本报告 C 路线） |
| pyserial + serial.android (PR 780) | Android + Chaquopy | 真 esptool.py | 未合入 upstream，需自带 fork |
| EspflashKotlin | JVM/Kotlin | 简化版 esptool | 可参考协议实现 |

## 附录 B：未来 OTA 化（本期不实施）

若 Passport 固件成熟后要脱离 USB：
在 `partitions.csv` 增加 `ota_0 / ota_1` + 实现 BLE/WiFi OTA 任务，
配合本报告 registry.json 的 `sha256` 做签名/回滚，即可把"刷机"从插线动作
极简为"手机点一处"。该改动会动到上游 BSP 基线分区表，务必遵守
`ai-passport/AGENTS.md` 里"用户固件可改分区表，但必须重新校验 layout"的约束。

## 附录 C：P0 实测记录（待填）

- 手机型号 / Android 版本：
- OTG 线材（是否需要供电 hub）：
- `nrflash probe` 输出：
- 刷写耗时：
- 刷后启动日志验收（"Battery=1 / Display=1 / Audio=1 / Button=1"）：

---

*报告到这里。每节都可以独立执行；下一步建议从 P0（真机验证 Termux 链路）开始，零代码改动，一天内可得到结论。*
