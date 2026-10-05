# my-folotoy

FoloToy AI Passport（ESP32-C3）的**自建固件集**。

上游仓库以 submodule 形式固定在 `upstream/ai-passport`，本仓库只放我们的
应用代码、共享资源、服务端与固件分发页。上游保持原样，不修改。

## 目录

```
upstream/ai-passport/      上游仓库(submodule)—— BSP 驱动从这里来
shared/fonts/              两个固件共用的 GB2312 中文字库(只存一份)
firmware/
  voice-bot/               语音 bot:按住说话,识别/回答/朗读都在 PC 上
  visual-novel/            视觉小说:剧情与画面从服务器拉取
services/
  voice-server/            语音 bot 的后端(Python)
  vn-server/               视觉小说的后端(FastAPI)
tools/test.sh              纯逻辑 host 测试(不需要 ESP-IDF)
web/                       固件分发页(ESP Web Tools 一键刷机)
docker/                    在 NAS 上部署分发页
docs/                      调研报告(手机端刷机流水线等)
```

## 为什么这样组织

每个固件是一个**独立完整的 ESP-IDF 工程**，因为它有自己的 `main/`、
`partitions.csv` 和 `sdkconfig.defaults`。它们共享的只有两样：

1. **BSP 驱动** —— 通过 `EXTRA_COMPONENT_DIRS` 指向上游 submodule，
   不复制代码，上游更新直接生效；
2. **中文字库**（3.5 MB）—— 放在 `shared/`，两个固件构建时各自引用，
   仓库里只存一份。

工程里的 `CMakeLists.txt` 因此只有几行：

```cmake
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../upstream/ai-passport/components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(voice-bot)
```

## 构建

需要 ESP-IDF **v5.5.3**。

```bash
cd firmware/voice-bot
idf.py set-target esp32c3
idf.py build
idf.py merge-bin -o voice-bot-full.bin   # 合并镜像,供网页刷机用
```

`visual-novel` 同理。

> 首次构建前请确认 submodule 已就位：
> `git submodule update --init --recursive`

### Wi-Fi 与服务器地址

凭据有两条来源，**优先用 NVS 里已配好的**：

1. **设备端配网（BLUFI over BLE）** —— 开机时若 NVS 没有凭据（或连不上），
   设备会广播 `BLUFI_FoloPassport`，用手机 App 通过蓝牙把 Wi-Fi 名称与密码
   发进去并存进 NVS。之后每次开机自动重连。
2. **Kconfig（仅开发用）** —— 也可以直接写进构建配置，省掉配网这一步：

```bash
idf.py menuconfig    # 找到 "AI voice bot (walkie-talkie)" 一栏
```

填好的值存放在该固件目录下的 `sdkconfig` 里，**这个文件已被 gitignore**，
不会进仓库。要给别人一份"开箱即用"的固件，请在本地构建后再分发 `.bin`。

#### ⚠️ BLE 配网的已知限制（未完成）

在 ESP32-C3 上这条路**内存不够**，目前**跑不通**，不要在上面浪费时间：

| 现象 | 根因 |
|---|---|
| 手机侧卡在"建立安全通道" | Wi-Fi 与 BLE 同时运行时堆只剩 ~6 KB，BLUFI 打包时 `Malloc failed` |
| 扫描附近 Wi-Fi 超时 | 配网期间故意没启动 Wi-Fi（为了省内存），扫描必然失败 |
| 停止 BLE 后启 Wi-Fi 卡住 | 两者切换期间资源未完全回收 |

**根因是硬件限制**：C3 可动态分配的主堆只有 **41 KB**（其余被 BLE/Wi-Fi 协议栈
静态预留），而 BLE 控制器 + Wi-Fi 驱动无法同时装下。

**当前可用的替代方案**：用 Kconfig 写入凭据后自行构建（见上）。

`firmware/voice-bot/main/voice_prov*.c` 保留了完整实现（BLUFI 流程照抄上游
`demo/blufi-provisioning`，含内存优化与崩溃修复），将来若换到有 PSRAM 的
S3 芯片可以直接复用。

## 测试

```bash
./tools/test.sh          # 纯逻辑测试,不碰硬件
```

覆盖 `vn_engine.c` 这类与 ESP-IDF/LVGL 解耦的状态机。

> Windows 上请从 MSYS2/MinGW 的 bash 里跑。直接调用 `gcc` 时若 PATH 不全，
> 会因缺 DLL 而静默退出（退出码 1、无任何输出）。

## 服务端

两个后端都是 Python，与固件解耦：

```bash
# 语音 bot 后端
cd services/voice-server
python -m venv .venv && .venv/Scripts/pip install -r requirements.txt
python voice_server.py --host 0.0.0.0 --port 8090 --device cuda --preload --warm-fillers

# 视觉小说后端
cd services/vn-server
pip install -r requirements.txt
python -m uvicorn vn_server.app:app --host 0.0.0.0 --port 8080
```

`services/voice-server/config.local.toml` 放 API key（已 gitignore）；
也可以改用环境变量。

调试界面时可只起 `mock_server.py`：协议与真服务器一致，但不加载任何模型、
不消耗 token，返回固定文本和蜂鸣音。

## 固件分发页

`web/` 是一个静态页，用 Espressif 官方的 [ESP Web Tools] 通过浏览器的
Web Serial 直接把固件写进设备，不需要安装 esptool。

在 NAS 上部署：

```bash
docker compose -f docker/docker-compose.yml up -d --build
# 默认监听 8500
```

固件二进制**不进仓库**，由 GitHub Release 分发。部署后把它们放进
`web/firmwares/`（该目录已挂载进容器，替换文件即可，无需重建镜像）：

```
web/firmwares/voice-bot-full.bin
web/firmwares/visual-novel-full.bin
```

页面对不存在的文件会显示"暂无可用的固件文件"，不会给出坏链接。

> 刷机需要**桌面版 Chrome 或 Edge**（Web Serial 只在 Chromium 内核可用），
> 以及一根**支持数据传输**的 USB 线。

## 发布

打 tag 即触发 CI：矩阵并行构建两个固件，生成合并镜像并发布到 Release。

```bash
git tag v0.1.0
git push origin v0.1.0
```

CI 会检出 submodule，所以推 tag 前请确认 submodule 的提交已推送。

## 授权

固件与上游一致。中文字库由 Noto Sans SC（SIL OFL 1.1）生成。

[ESP Web Tools]: https://github.com/esphome/esp-web-tools
