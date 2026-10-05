// main/voice_prov.c —— BLUFI 配网实现。
//
// 逻辑主体来自上游 demo/blufi-provisioning 分支的 demo_blufi.c(470 行),
// 这里做了三处必要改动:
//   1. 去掉 demo.h / ui_pixel.h 依赖 —— 上游那套是演示框架,本固件不用;
//      状态通过调用方给的回调显示(见 voice_prov.h)。
//   2. 去掉上游的 demo 入口(enter/exit/key),改成一次阻塞式的
//      voice_prov_run(),由应用层在开机时决定是否调用。
//   3. 精简:去掉与配网无关的状态机和界面刷新计时器。
//
// BLUFI 的协议处理本身没有改动 —— 那部分是经过验证的,不该重写。
#include "voice_prov.h"

#include "voice_prov_radio.h"
#include "voice_prov_security.h"

#include "esp_blufi.h"
#include "esp_blufi_api.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "host/ble_hs.h"
#include "nvs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

#include <string.h>

static const char *TAG = "v_prov";

// EspBlufi 小程序默认只列出以 "BLUFI" 开头的设备。
static const char *DEVICE_NAME = "BLUFI_FoloPassport";

#define BLUFI_AP_LIST_COUNT 16
// 拿到 IP 或出错时置位,让 voice_prov_run 能立刻返回。
#define BIT_DONE BIT0

static EventGroupHandle_t s_events;
static voice_prov_status_fn s_status_fn;
static void *s_status_user;

static esp_netif_t *s_sta_netif;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;
static wifi_config_t s_sta_config;
static char s_ip[16];

static volatile bool s_wifi_connecting;
static volatile bool s_wifi_got_ip;
static volatile bool s_ble_connected;
static volatile bool s_failed;

static bool s_wifi_started;
static bool s_host_initialized;
static bool s_host_running;
static bool s_profile_initialized;
static bool s_gatt_initialized;
static bool s_btc_initialized;
// nimble_port_stop() 之后要等这个信号量,确认 host 任务真的退出了。
static SemaphoreHandle_t s_host_stopped;

static void report(const char *text)
{
    ESP_LOGI(TAG, "%s", text);
    if (s_status_fn != NULL) s_status_fn(text, s_status_user);
}

// --------------------------------------------------------------------------
// 已保存的凭据
// --------------------------------------------------------------------------
bool voice_prov_has_credentials(void)
{
    // Wi-Fi 驱动把凭据存在 NVS 的 nvs.net80211 命名空间里。
    // 这里只看"有没有配过",不读明文。
    wifi_config_t config = { 0 };
    if (esp_wifi_get_config(WIFI_IF_STA, &config) != ESP_OK) return false;
    return config.sta.ssid[0] != '\0';
}

esp_err_t voice_prov_forget(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("nvs.net80211", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(handle);
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "已清除保存的 Wi-Fi 凭据");
    return err;
}

// --------------------------------------------------------------------------
// Wi-Fi 事件
// --------------------------------------------------------------------------
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    switch (id) {
    case WIFI_EVENT_STA_START:
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        if (s_wifi_connecting) {
            s_wifi_connecting = false;
            s_failed = true;
            report("连不上，检查名称和密码");
            if (s_events) xEventGroupSetBits(s_events, BIT_DONE);
        }
        break;
    default:
        break;
    }
}

static void ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id != IP_EVENT_STA_GOT_IP) return;

    const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
    s_wifi_got_ip = true;
    s_wifi_connecting = false;
    s_failed = false;

    char line[48];
    snprintf(line, sizeof(line), "已连接 %s", s_ip);
    report(line);

    if (s_events) xEventGroupSetBits(s_events, BIT_DONE);
}

// --------------------------------------------------------------------------
// BLUFI:把状态回给手机
// --------------------------------------------------------------------------
static void send_wifi_report(esp_blufi_sta_conn_state_t state)
{
    // 注意:这一版 esp_blufi_extra_info_t 里没有 sta_ip 字段,只报连接状态即可。
    // 手机 App 拿到"连接成功"就可以结束配网流程。
    esp_blufi_send_wifi_conn_report(WIFI_MODE_STA, state, 0, NULL);
}

// 回应"扫描附近 Wi-Fi"请求。
//
// ⚠️ 配网期间故意没启动 Wi-Fi(为了给 BLE 留出内存,见 voice_prov_run),
// 所以 esp_wifi_scan_start() 必然失败。失败时【必须回一个空列表】:
// 不回的话小程序会一直等,最后报"扫描周围网络超时"。
// 空列表会让它提示用户手动输入 SSID,而不是卡住。
static void send_wifi_list(void)
{
    wifi_ap_record_t *records = calloc(BLUFI_AP_LIST_COUNT, sizeof(wifi_ap_record_t));
    if (records == NULL) {
        esp_blufi_send_wifi_list(0, NULL);
        return;
    }

    uint16_t count = BLUFI_AP_LIST_COUNT;
    const esp_err_t scan_err = esp_wifi_scan_start(NULL, true);
    if (scan_err != ESP_OK) {
        ESP_LOGW(TAG, "扫描不可用(Wi-Fi 未启动): %s;回空列表", esp_err_to_name(scan_err));
        free(records);
        esp_blufi_send_wifi_list(0, NULL);
        return;
    }

    if (esp_wifi_scan_get_ap_records(&count, records) != ESP_OK) {
        free(records);
        esp_blufi_send_wifi_list(0, NULL);
        return;
    }

    esp_blufi_ap_record_t *list = calloc(count, sizeof(esp_blufi_ap_record_t));
    if (list == NULL) {
        free(records);
        esp_blufi_send_wifi_list(0, NULL);
        return;
    }

    for (int i = 0; i < count; i++) {
        list[i].rssi = records[i].rssi;
        memcpy(list[i].ssid, records[i].ssid, sizeof(list[i].ssid));
    }
    esp_blufi_send_wifi_list(count, list);
    free(list);
    free(records);
}

static esp_err_t wifi_start(void);
static void prov_stop(void);

// BLUFI 要求连网时调用。
//
// ⚠️ 关键:必须【先停掉 BLE 再启动 Wi-Fi】。两者同时开着时内部 RAM 不够,
// BLE 会报 BLE_ERR_MEM_CAPACITY,手机侧表现为卡在"建立安全通道"。
// 凭据此时已经通过 esp_wifi_set_config() 写进驱动(配网途中就写好了),
// 所以停 BLE 不会丢凭据。
static void request_wifi_connect(void)
{
    if (s_wifi_connecting) return;

    // 先把 BLE 完整停掉,给 Wi-Fi 腾内存。
    // ⚠️ 必须用 prov_stop() 而不是只调 nimble_port_stop():
    // 后者不会等 host 任务退出、也不会释放 GATT/BTC,残留状态会让
    // 紧接着的 Wi-Fi 初始化失败(实测报 "Failed to deinit Wi-Fi driver 0x3001")。
    // 凭据此时已经写进驱动(配网途中就写好了),停 BLE 不会丢。
    if (s_host_initialized) {
        prov_stop();
    }

    if (!s_wifi_started) {
        if (wifi_start() != ESP_OK) {
            s_failed = true;
            report("Wi-Fi 启动失败");
            if (s_events) xEventGroupSetBits(s_events, BIT_DONE);
            return;
        }
    }

    s_wifi_connecting = true;
    esp_wifi_disconnect();
    if (esp_wifi_connect() != ESP_OK) {
        s_wifi_connecting = false;
        s_failed = true;
        report("连接请求失败");
        if (s_events) xEventGroupSetBits(s_events, BIT_DONE);
    }
}

static void blufi_event(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param)
{
    switch (event) {
    case ESP_BLUFI_EVENT_INIT_FINISH:
        esp_blufi_adv_start_with_name(DEVICE_NAME);
        ESP_LOGI(TAG, "BLUFI 初始化完成,开始广播 \"%s\"", DEVICE_NAME);
        report("用手机蓝牙配网");
        break;

    case ESP_BLUFI_EVENT_BLE_CONNECT:
        s_ble_connected = true;
        esp_blufi_adv_stop();
        if (voice_prov_security_init() != 0) {
            report("安全协商初始化失败");
            s_failed = true;
        } else {
            report("手机已连接，请选择 Wi-Fi");
        }
        break;

    case ESP_BLUFI_EVENT_BLE_DISCONNECT:
        s_ble_connected = false;
        voice_prov_security_deinit();
        esp_blufi_adv_start_with_name(DEVICE_NAME);
        // 断开了但如果已经拿到 IP,不必再配。
        if (!s_wifi_got_ip) report("用手机蓝牙配网");
        break;

    case ESP_BLUFI_EVENT_SET_WIFI_OPMODE:
        esp_wifi_set_mode(WIFI_MODE_STA);
        break;

    case ESP_BLUFI_EVENT_RECV_STA_BSSID:
        memcpy(s_sta_config.sta.bssid, param->sta_bssid.bssid, 6);
        s_sta_config.sta.bssid_set = true;
        esp_wifi_set_config(WIFI_IF_STA, &s_sta_config);
        break;

    case ESP_BLUFI_EVENT_RECV_STA_SSID:
        if (param->sta_ssid.ssid_len >= sizeof(s_sta_config.sta.ssid)) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            break;
        }
        memset(s_sta_config.sta.ssid, 0, sizeof(s_sta_config.sta.ssid));
        memset(s_sta_config.sta.password, 0, sizeof(s_sta_config.sta.password));
        memset(s_sta_config.sta.bssid, 0, sizeof(s_sta_config.sta.bssid));
        s_sta_config.sta.bssid_set = false;
        s_sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
        memcpy(s_sta_config.sta.ssid, param->sta_ssid.ssid, param->sta_ssid.ssid_len);
        esp_wifi_set_config(WIFI_IF_STA, &s_sta_config);
        break;

    case ESP_BLUFI_EVENT_RECV_STA_PASSWD:
        if (param->sta_passwd.passwd_len >= sizeof(s_sta_config.sta.password)) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            break;
        }
        memset(s_sta_config.sta.password, 0, sizeof(s_sta_config.sta.password));
        memcpy(s_sta_config.sta.password, param->sta_passwd.passwd,
               param->sta_passwd.passwd_len);
        esp_wifi_set_config(WIFI_IF_STA, &s_sta_config);
        break;

    case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP:
        report("正在连接…");
        request_wifi_connect();
        break;

    case ESP_BLUFI_EVENT_REQ_DISCONNECT_FROM_AP:
        esp_wifi_disconnect();
        break;

    case ESP_BLUFI_EVENT_GET_WIFI_STATUS:
        if (s_wifi_got_ip) {
            send_wifi_report(ESP_BLUFI_STA_CONN_SUCCESS);
        } else if (s_wifi_connecting) {
            send_wifi_report(ESP_BLUFI_STA_CONNECTING);
        } else {
            send_wifi_report(ESP_BLUFI_STA_CONN_FAIL);
        }
        break;

    case ESP_BLUFI_EVENT_GET_WIFI_LIST:
        send_wifi_list();
        break;

    case ESP_BLUFI_EVENT_RECV_SLAVE_DISCONNECT_BLE:
        esp_blufi_disconnect();
        break;

    case ESP_BLUFI_EVENT_DEINIT_FINISH:
        break;

    case ESP_BLUFI_EVENT_REPORT_ERROR:
        ESP_LOGE(TAG, "BLUFI 报告错误: %d", param->report_error.state);
        esp_blufi_send_error_info(param->report_error.state);
        break;

    default:
        break;
    }

    if (event == ESP_BLUFI_EVENT_BLE_CONNECT && s_wifi_got_ip) {
        // 已经连上了,重连时直接把结果告诉手机。
        send_wifi_report(ESP_BLUFI_STA_CONN_SUCCESS);
    }
}

// --------------------------------------------------------------------------
// NimBLE 主机
// --------------------------------------------------------------------------
static void host_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "BLE 主机任务已启动,进入 nimble_port_run");
    nimble_port_run();              // 直到 nimble_port_stop() 才返回
    ESP_LOGI(TAG, "BLE 主机任务退出");
    // 通知 prov_stop() 可以安全释放资源了。
    if (s_host_stopped != NULL) xSemaphoreGive(s_host_stopped);
    nimble_port_freertos_deinit();
}

static void host_reset(int reason)
{
    ESP_LOGE(TAG, "BLE 主机复位,原因=%d", reason);
}

static void host_sync(void)
{
    // ⚠️ 这里只做上游 blufi_sync() 做的事,不要自行加操作。
    // 曾经在这里加过 ble_hs_id_infer_auto() 想"固定地址",结果在
    // profile init 之后立刻 Store access fault —— 同步回调里 BLE 尚未
    // 完全就绪,多余的操作会踩到非法地址。
    const int rc = esp_blufi_profile_init();
    if (rc == 0) {
        s_profile_initialized = true;
    } else {
        ESP_LOGE(TAG, "BLUFI profile 初始化失败: %d", rc);
        if (s_events) xEventGroupSetBits(s_events, BIT_DONE);
    }
}

// --------------------------------------------------------------------------
// 启动
// --------------------------------------------------------------------------
static esp_err_t wifi_start(void)
{
    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (s_sta_netif == NULL) return ESP_FAIL;
    }

    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&init);
    if (err != ESP_OK) return err;

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL,
                                        &s_wifi_handler);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event, NULL,
                                        &s_ip_handler);

    // 起始 STA 模式;已保存的凭据会被驱动自动读取。
    s_sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    // 凭据要写回 NVS(配网成功后下次开机自动重连),所以明确用 FLASH 存储。
    // 这一步上游有,漏掉的话配网结果不持久。
    err = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &s_sta_config);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) return err;

    // 配网期间关省电,蓝牙与 Wi-Fi 共存时更稳。
    esp_wifi_set_ps(WIFI_PS_NONE);
    s_wifi_started = true;
    return ESP_OK;
}

// ⚠️ 必须是静态存储期:esp_blufi_register_callbacks() 只保存这个指针,
// 不会拷贝内容。之前写成复合字面量(&(esp_blufi_callbacks_t){...})是栈上的
// 临时对象,函数返回后指针失效,BLUFI 回调一被调用就 Store access fault 崩溃。
static esp_blufi_callbacks_t s_blufi_callbacks = {
    .event_cb = blufi_event,
    .negotiate_data_handler = voice_prov_negotiate,
    .encrypt_func = voice_prov_encrypt,
    .decrypt_func = voice_prov_decrypt,
    .checksum_func = voice_prov_checksum,
};

static esp_err_t host_start(void)
{
    // ⚠️ 顺序照抄上游 demo/blufi-provisioning,不要自行"精简"。
    // 少任何一步都会在运行期炸:
    //   - 缺 esp_blufi_btc_init() → 控制器 host 线程没建,
    //     之后 osi_thread_post 断言失败 (thread != NULL) 并复位;
    //   - 缺 esp_blufi_gatt_svr_init() → BLUFI 的 GATT 服务不存在,
    //     手机连上后收不到任何数据;
    //   - 缺 ble_svc_gap_device_name_set() → 广播名不对,
    //     小程序按 "BLUFI" 前缀过滤时看不到设备。
    esp_err_t err = esp_blufi_register_callbacks(&s_blufi_callbacks);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "注册 BLUFI 回调失败: %s", esp_err_to_name(err));
        return err;
    }

    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init 失败: %s", esp_err_to_name(err));
        return err;
    }
    s_host_initialized = true;

    // host 任务退出时用它通知 prov_stop();必须在启动任务前创建。
    s_host_stopped = xSemaphoreCreateBinary();
    if (s_host_stopped == NULL) return ESP_ERR_NO_MEM;

    ble_hs_cfg.reset_cb = host_reset;
    ble_hs_cfg.sync_cb = host_sync;
    ble_hs_cfg.gatts_register_cb = esp_blufi_gatt_svr_register_cb;

    if (esp_blufi_gatt_svr_init() != 0) {
        ESP_LOGE(TAG, "BLUFI GATT 服务初始化失败");
        return ESP_FAIL;
    }
    s_gatt_initialized = true;

    if (ble_svc_gap_device_name_set(DEVICE_NAME) != 0) {
        ESP_LOGE(TAG, "设置 BLE 设备名失败");
        return ESP_FAIL;
    }

    esp_blufi_btc_init();
    s_btc_initialized = true;

    // esp_nimble_enable() 内部用 xTaskCreatePinnedToCore 建 host 任务,
    // 但它【不检查返回值】—— 堆不够时任务根本建不起来,函数却仍返回 ESP_OK,
    // 表现为"蓝牙启动了但永不广播"。所以在调用前后打印剩余堆,便于定位。
    ESP_LOGI(TAG, "启动 BLE 主机前剩余堆: %u B", (unsigned)esp_get_free_heap_size());
    err = esp_nimble_enable(host_task);
    ESP_LOGI(TAG, "esp_nimble_enable 返回 %s,之后剩余堆: %u B",
             esp_err_to_name(err), (unsigned)esp_get_free_heap_size());
    if (err == ESP_OK) s_host_running = true;
    return err;
}

// ⚠️ 顺序照抄上游 demo_stop,每一步都不能省。
// 特别是 nimble_port_stop() 之后【必须等 host 任务真正退出】才能
// 释放资源 —— 少了这个等待,NimBLE 线程还在跑就被拆掉,
// 表现为配网超时后 Load access fault 崩溃。
static void prov_stop(void)
{
    voice_prov_security_deinit();

    if (s_host_initialized) {
        if (s_profile_initialized) esp_blufi_adv_stop();
        if (s_gatt_initialized) {
            esp_blufi_gatt_svr_deinit();
            s_gatt_initialized = false;
        }

        bool host_stopped = !s_host_running;
        if (s_host_running) {
            const int rc = nimble_port_stop();
            if (rc == 0) {
                xSemaphoreTake(s_host_stopped, portMAX_DELAY);   // ← 必须等
                host_stopped = true;
            } else {
                ESP_LOGE(TAG, "nimble_port_stop 失败: %d", rc);
            }
        }
        if (host_stopped) nimble_port_deinit();
        s_host_running = false;

        if (s_profile_initialized) {
            esp_blufi_profile_deinit();
            s_profile_initialized = false;
        }
        if (s_btc_initialized) {
            esp_blufi_btc_deinit();
            s_btc_initialized = false;
        }
        s_host_initialized = false;
    }

    if (s_host_stopped != NULL) {
        vSemaphoreDelete(s_host_stopped);
        s_host_stopped = NULL;
    }
}

esp_err_t voice_prov_run(voice_prov_status_fn on_status, void *user, int timeout_ms)
{
    s_status_fn = on_status;
    s_status_user = user;
    s_events = xEventGroupCreate();
    if (s_events == NULL) return ESP_ERR_NO_MEM;

    esp_err_t err = voice_prov_radio_nvs_prepare();
    if (err != ESP_OK) {
        report("存储初始化失败");
        goto done;
    }
    err = voice_prov_radio_network_prepare();
    if (err != ESP_OK) {
        report("网络初始化失败");
        goto done;
    }

    // ⚠️ 内存是这里最紧的资源:Wi-Fi 与 BLE 同时开着会把内部 RAM 挤光,
    // 表现为 BLUFI 报 BLE_ERR_MEM_CAPACITY、手机侧卡在"建立安全通道"。
    // 所以两者【串行】使用:
    //   已有凭据 → 只开 Wi-Fi 连接,根本不启动 BLE;
    //   没有凭据 → 只开 BLE 配网,拿到凭据并确认能连上后才停掉 BLE。
    if (voice_prov_has_credentials()) {
        err = wifi_start();
        if (err != ESP_OK) {
            report("Wi-Fi 初始化失败");
            goto done;
        }
        report("正在连接已保存的网络…");
        s_wifi_connecting = true;
        esp_wifi_connect();
        const EventBits_t bits = xEventGroupWaitBits(s_events, BIT_DONE, pdFALSE, pdFALSE,
                                                     pdMS_TO_TICKS(15000));
        if ((bits & BIT_DONE) && s_wifi_got_ip) {
            err = ESP_OK;
            goto done;
        }
        s_wifi_connecting = false;
        xEventGroupClearBits(s_events, BIT_DONE);
        report("连不上已保存的网络，改用蓝牙配网");
    }

    // 走到这里说明需要配网。此时【不】启动 Wi-Fi,把内存让给 BLE。
    err = host_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "蓝牙启动失败: %s", esp_err_to_name(err));
        report("蓝牙启动失败");
        goto done;
    }
    ESP_LOGI(TAG, "BLE 主机已启动,等待广播");

    report("用手机蓝牙配网");
    const EventBits_t bits = xEventGroupWaitBits(s_events, BIT_DONE, pdFALSE, pdFALSE,
                                                 pdMS_TO_TICKS(timeout_ms));
    err = (bits & BIT_DONE) && s_wifi_got_ip ? ESP_OK : ESP_ERR_TIMEOUT;
    if (err != ESP_OK) {
        report("配网超时");
    }

done:
    // 配网结束就停掉 BLE:后续语音链路只用 Wi-Fi,留着 BLE 白占内存。
    prov_stop();
    if (s_events != NULL) {
        vEventGroupDelete(s_events);
        s_events = NULL;
    }
    return err;
}
