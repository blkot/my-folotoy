#include "voice_app.h"

#include "voice_audio.h"
#include "voice_net.h"
#include "voice_prov.h"
#include "voice_ui.h"

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"

#include "sdkconfig.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "v_app";

// bsp_pins.h 里三档分压的判定窗口(mV)。>1900 视为松开。
#define BTN_MV_UP_MAX   150
#define BTN_MV_DOWN_MAX 447
#define BTN_MV_OK_MAX   1900

// 20 ms @ 16 kHz 单声道 16 bit = 640 B
#define CHUNK_BYTES ((CONFIG_VOICE_SAMPLE_RATE * CONFIG_VOICE_CHUNK_MS / 1000) * 2)

#define UI_FOOTER_MAX 48
#define UI_TEXT_MAX   128

// 电量不必每帧读:CW2017 走 I2C,频繁读既无意义也占总线。
#define BATTERY_POLL_MS 5000

// 设置页里的两个可调项。
#define SETTING_VOLUME     0
#define SETTING_BRIGHTNESS 1
#define VOLUME_STEP        10
#define BRIGHTNESS_STEPS   4

static int s_volume = CONFIG_VOICE_SPEAKER_VOLUME;
static int s_brightness = 100;

// --------------------------------------------------------------------------
// 界面:真正的绘制在 voice_ui.c,这里只负责持锁(操作 LVGL 一律持锁)。
// --------------------------------------------------------------------------
static void ui_state(voice_ui_state_t state, const char *text) {
    if (!bsp_lvgl_lock(200)) return;
    voice_ui_set_state(state, text);
    bsp_lvgl_unlock();
}

static void ui_level(int percent) {
    if (!bsp_lvgl_lock(200)) return;
    voice_ui_set_level(percent);
    bsp_lvgl_unlock();
}

static void ui_footer(const char *text) {
    if (!bsp_lvgl_lock(200)) return;
    voice_ui_set_footer(text);
    bsp_lvgl_unlock();
}

static void ui_message(voice_ui_role_t role, const char *text) {
    if (!bsp_lvgl_lock(300)) return;
    voice_ui_add_message(role, text);
    bsp_lvgl_unlock();
}

static void ui_battery(int soc) {
    if (!bsp_lvgl_lock(200)) return;
    voice_ui_set_battery(soc);
    bsp_lvgl_unlock();
}

static void ui_volume(int percent) {
    if (!bsp_lvgl_lock(200)) return;
    voice_ui_set_volume(percent);
    bsp_lvgl_unlock();
}

static void apply_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_volume = percent;
    voice_audio_set_volume((uint8_t)percent);
    ui_volume(percent);
}

static void apply_brightness(int percent) {
    if (percent < 20) percent = 20;     // 太低会以为没开机
    if (percent > 100) percent = 100;
    s_brightness = percent;
    bsp_display_backlight((uint8_t)percent);
}

// --------------------------------------------------------------------------
// 服务器可能在任何时刻插入 TXT 行(识别结果、AI 回复)。它和 PLAY 共用
// 同一条流,所以必须"读一行判断一行",不能假定下一行一定是 PLAY。
//
// 两段 TXT 用 role 区分:先来的是识别结果(用户),PLAY 之前来的是 AI 回复。
// --------------------------------------------------------------------------
static esp_err_t wait_for_play(unsigned long *announced, bool *got_reply,
                               char *reply_out, size_t reply_size) {
    *got_reply = false;
    int user_messages = 0;

    for (int attempts = 0; attempts < 12; attempts++) {
        char line[UI_TEXT_MAX];
        if (voice_net_recv_line(line, sizeof(line)) < 0) return ESP_FAIL;

        if (strncmp(line, "TXT ", 4) == 0) {
            const char *text = line + 4;
            ESP_LOGI(TAG, "TXT: %s", text);
            if (user_messages == 0) {
                ui_message(VOICE_UI_USER, text);    // 第一段是识别结果
                user_messages++;
            } else {
                // 之后的是 AI 回复,显示出来并回传给调用方。
                ui_message(VOICE_UI_BOT, text);
                if (reply_out != NULL && reply_size > 0) {
                    snprintf(reply_out, reply_size, "%s", text);
                }
                *got_reply = true;
            }
            continue;
        }
        if (strncmp(line, "PLAY ", 5) == 0) {
            // 这个数字只是提示。流式回复下服务器本来就无法预知总量,所以
            // 解析失败不该当成错误 —— 真正的终止信号是长度 0 的帧。
            if (sscanf(line + 5, "%lu", announced) != 1) {
                ESP_LOGI(TAG, "PLAY 头无长度(%s),按流式处理", line + 5);
                *announced = 0;
            }
            return ESP_OK;
        }
        if (line[0] == '\0') continue;      // 容忍空行
        ESP_LOGW(TAG, "无法识别的控制行: %s", line);
    }
    return ESP_FAIL;
}

// --------------------------------------------------------------------------
// 输入:轮询 ADC 分压值,拿到干净的按下/松开边沿 + 具体是哪个键。
// (事件回调只有 PRESS/CLICK/LONG,没有"松开",做不了按住说话。)
// --------------------------------------------------------------------------
static int poll_key(void) {
    const int mv = bsp_button_read_mv();
    if (mv < 0) return -1;
    if (mv < BTN_MV_UP_MAX) return BSP_BTN_UP;
    if (mv < BTN_MV_DOWN_MAX) return BSP_BTN_DOWN;
    if (mv < BTN_MV_OK_MAX) return BSP_BTN_OK;
    return -1;  // 松开
}

// 给电平条用的粗算 RMS;语音 RMS 通常只有满量程的 1/10,所以 ×3 更好看。
static int rms_percent(const uint8_t *pcm, size_t bytes) {
    const int16_t *samples = (const int16_t *)pcm;
    const size_t count = bytes / 2;
    if (count == 0) return 0;

    int64_t sum = 0;
    for (size_t i = 0; i < count; i++) {
        const int32_t value = samples[i];
        sum += (int64_t)value * value;
    }
    const double rms = sqrt((double)sum / (double)count);
    const int percent = (int)(rms * 300.0 / 32768.0);
    return percent > 100 ? 100 : percent;
}

// 长按判定:按住超过这个时间算长按(松开时生效,避免和"按住说话"抢)。
#define LONG_PRESS_MS 700

// 设置页:上下选择,短按确定调整当前项,长按确定退出。
static void settings_loop(void) {
    ui_state(VOICE_UI_IDLE, "SETTINGS");
    if (!bsp_lvgl_lock(300)) return;
    voice_ui_settings_open();
    voice_ui_settings_refresh(s_volume, s_brightness);
    bsp_lvgl_unlock();

    int last = -1;
    int64_t ok_pressed_at = 0;

    for (;;) {
        const int key = poll_key();
        const int64_t now = esp_timer_get_time();

        if (key != last) {
            if (key == BSP_BTN_OK && ok_pressed_at == 0) ok_pressed_at = now;

            if (key == BSP_BTN_UP || key == BSP_BTN_DOWN) {
                if (bsp_lvgl_lock(200)) {
                    const int index = (voice_ui_settings_selected() +
                                       (key == BSP_BTN_DOWN ? 1 : -1) + 2) % 2;
                    voice_ui_settings_select(index);
                    bsp_lvgl_unlock();
                }
            } else if (key != BSP_BTN_OK && ok_pressed_at != 0) {
                // 确定键松开:长按退出,短按调整当前项。
                const bool is_long = (now - ok_pressed_at) > (LONG_PRESS_MS * 1000LL);
                ok_pressed_at = 0;
                if (is_long) {
                    break;
                }
                int index = 0;
                if (bsp_lvgl_lock(200)) {
                    index = voice_ui_settings_selected();
                    bsp_lvgl_unlock();
                }
                if (index == SETTING_VOLUME) {
                    apply_volume(s_volume >= 100 ? 0 : s_volume + VOLUME_STEP);
                } else {
                    // 亮度按档位循环:25 / 50 / 75 / 100
                    const int step = 100 / BRIGHTNESS_STEPS;
                    apply_brightness(s_brightness >= 100 ? step : s_brightness + step);
                }
                if (bsp_lvgl_lock(200)) {
                    voice_ui_settings_refresh(s_volume, s_brightness);
                    bsp_lvgl_unlock();
                }
            }
            last = key;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (bsp_lvgl_lock(300)) {
        voice_ui_settings_close();
        bsp_lvgl_unlock();
    }
}

// --------------------------------------------------------------------------
// 主循环:待机 → 录音(边说边传) → 等回复 → 播放(边收边放)
// --------------------------------------------------------------------------
static void voice_task(void *arg) {
    (void)arg;
    static uint8_t chunk[CHUNK_BYTES];

    // 把服务器地址先显示出来,让人知道该连哪。
    char footer[UI_FOOTER_MAX];
    snprintf(footer, sizeof(footer), "%s:%d", CONFIG_VOICE_SERVER_HOST,
             CONFIG_VOICE_SERVER_PORT);
    ui_footer(footer);
    ui_volume(s_volume);
    apply_volume(s_volume);
    apply_brightness(s_brightness);

    int64_t battery_read_at = 0;

    for (;;) {
        // ---- 待机 ----
        // 待机期间【不】建立连接:设备只在说话那几秒需要联网。
        // 常连有两个坏处:一是白耗电,二是服务端会积压一堆"设备早已不
        // 再用、但仍显示 Established"的 socket(实测堆到 6 个),
        // 最终把设备的 socket 配额耗光。
        ui_state(VOICE_UI_IDLE, "READY");
        ui_level(0);

        // 待机时轮询按键,同时定期刷新电量。
        //
        // 按键分工(避免和"按住说话"冲突):
        //   上 / 下  → 进设置页(待机时这两个键本就没用途)
        //   确定     → 按住说话。注意这里【不】判断长按:说话本来就要按
        //              好几秒,把长按当成另一个功能会让它根本说不了话。
        int64_t battery_tick = 0;
        bool want_settings = false;
        bool want_talk = false;
        for (;;) {
            const int key = poll_key();
            const int64_t now = esp_timer_get_time();

            if (now - battery_read_at > (BATTERY_POLL_MS * 1000LL)) {
                battery_read_at = now;
                ui_battery(bsp_battery_soc());
            }

            if (key == BSP_BTN_UP || key == BSP_BTN_DOWN) {
                want_settings = true;
                break;
            }
            if (key == BSP_BTN_OK) {
                want_talk = true;
                break;
            }
            (void)battery_tick;
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        if (want_settings) {
            settings_loop();
            battery_read_at = 0;
            continue;
        }
        if (!want_talk) continue;

        // ---- 要说话了,这时才连 ----
        if (!voice_net_ready()) {
            ui_state(VOICE_UI_THINKING, "CONNECTING");
            if (voice_net_connect() != ESP_OK) {
                ui_state(VOICE_UI_OFFLINE, "NO LINK");
                ui_message(VOICE_UI_SYSTEM, "连不上服务器,检查 Wi-Fi 和 PC");
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
        }

        // ---- 录音:边说边传,不等说完 ----
        ui_state(VOICE_UI_LISTENING, "LISTENING");
        if (voice_net_send_line("REC") != ESP_OK) {
            voice_net_reconnect();
            continue;
        }

        size_t uploaded = 0;
        int ui_ticks = 0;
        bool ok = true;
        while (poll_key() == BSP_BTN_OK) {
            const int got = voice_audio_read(chunk, sizeof(chunk));
            if (got <= 0) {
                ok = false;
                break;
            }
            if (voice_net_send_frame(chunk, (size_t)got) != ESP_OK) {
                ok = false;
                break;
            }
            uploaded += (size_t)got;
            if (++ui_ticks >= 5) {  // 100 ms 刷新一次,别把 LVGL 锁抢太勤
                ui_ticks = 0;
                ui_level(rms_percent(chunk, (size_t)got));
            }
        }
        ui_level(0);
        if (!ok) {
            ESP_LOGW(TAG, "录音链路中断");
            voice_net_reconnect();
            continue;
        }
        if (uploaded == 0) continue;  // 按了就松,没录到东西
        if (voice_net_send_end() != ESP_OK) {
            voice_net_reconnect();
            continue;
        }
        ESP_LOGI(TAG, "已上传 %u B = %.2f s 音频", (unsigned)uploaded,
                 (double)uploaded / (2.0 * CONFIG_VOICE_SAMPLE_RATE));

        // ---- 等服务器回复(识别结果 TXT → AI 回复 TXT → PLAY) ----
        // 这一阶段和播放阶段都【不看按键】:用户可能因为等待而乱按,
        // 一旦响应就会把这一轮对话丢掉(实测出现过).
        ui_state(VOICE_UI_THINKING, "THINKING");
        unsigned long announced = 0;
        bool got_reply = false;
        char reply[UI_TEXT_MAX];
        reply[0] = '\0';
        if (wait_for_play(&announced, &got_reply, reply, sizeof(reply)) != ESP_OK) {
            voice_net_reconnect();
            continue;
        }
        ESP_LOGI(TAG, "服务器声明 %lu B 音频,已收到回复=%d", announced, got_reply);

        // ---- 播放:边收边放 ----
        ui_state(VOICE_UI_SPEAKING, "SPEAKING");
        const int64_t started = esp_timer_get_time();
        size_t played = 0;
        int protocol_errors = 0;
        for (;;) {
            size_t len = 0;
            const int frame = voice_net_recv_frame(chunk, sizeof(chunk), &len);
            if (frame == 0) break;                       // 收尾帧
            if (frame < 0) {
                if (frame == VN_FRAME_PROTOCOL_ERROR && ++protocol_errors <= 3) {
                    // 错位后流已经无法可靠同步,只能等这一段的收尾帧;
                    // 直接断开重连会把整轮对话丢掉。
                    ESP_LOGW(TAG, "帧错位 %d 次,继续读到收尾帧", protocol_errors);
                    continue;
                }
                ok = false;
                break;
            }
            if (voice_audio_write(chunk, len) != ESP_OK) {
                ok = false;
                break;
            }
            played += len;
        }
        if (!ok) {
            ESP_LOGW(TAG, "播放链路中断");
            voice_net_reconnect();
            continue;
        }
        ESP_LOGI(TAG, "已播放 %u B = %.2f s(服务器声明 %lu B),用时 %.0f ms",
                 (unsigned)played, (double)played / (2.0 * CONFIG_VOICE_SAMPLE_RATE),
                 announced, (esp_timer_get_time() - started) / 1000.0);

        // 一轮对话结束就断开:待机不需要连接,留着只会让服务端积压
        // 已死的 Established 连接。
        voice_net_drop();
    }
}

// 配网状态回调:把 voice_prov 的进展显示到屏幕上。
static void on_prov_status(const char *text, void *user) {
    (void)user;
    if (!bsp_lvgl_lock(200)) return;
    voice_ui_set_state(VOICE_UI_OFFLINE, "SETUP");
    voice_ui_add_message(VOICE_UI_SYSTEM, text);
    bsp_lvgl_unlock();
}

esp_err_t voice_app_start(void) {
    esp_err_t err = voice_audio_init();
    if (err != ESP_OK) return err;

    // bsp_button_read_mv() 依赖按键驱动建好的 ADC 与校准,所以必须 init;
    // 回调传 NULL:这个应用用轮询,不需要事件。
    err = bsp_button_init(NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "按键初始化失败(%s),将无法用「确定」说话", esp_err_to_name(err));
    }

    // 电量计:失败不影响语音功能,只是状态栏没有数字。
    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "电量计初始化失败,状态栏不显示电量");
    }

    if (!bsp_lvgl_lock(1000)) return ESP_ERR_TIMEOUT;
    voice_ui_build();
    voice_ui_set_state(VOICE_UI_IDLE, "READY");
    bsp_lvgl_unlock();

    // 联网:优先用 NVS 里已配好的凭据;没配过(或连不上)就进蓝牙配网。
    //
    // 放在建线程之前做,是因为配网要独占 BLE 与 Wi-Fi;拿到 IP 后再启动
    // 语音任务。配网失败也不阻塞启动 —— 界面和设置仍可用,用户下次开机
    // 还能再配。
    const esp_err_t prov_err = voice_prov_run(on_prov_status, NULL, 180000);
    if (prov_err != ESP_OK) {
        ESP_LOGW(TAG, "配网未完成(%s),先进入界面", esp_err_to_name(prov_err));
        if (bsp_lvgl_lock(300)) {
            voice_ui_set_state(VOICE_UI_OFFLINE, "NO WIFI");
            voice_ui_add_message(VOICE_UI_SYSTEM, "没有网络。长按确定可在设置里重试配网。");
            bsp_lvgl_unlock();
        }
    } else {
        if (bsp_lvgl_lock(300)) {
            voice_ui_set_state(VOICE_UI_IDLE, "READY");
            bsp_lvgl_unlock();
        }
    }

    if (xTaskCreate(voice_task, "voice", 6144, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "语音任务创建失败");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "语音 bot 就绪:短按「确定」说话,长按进设置(音量/亮度)");
    return ESP_OK;
}
