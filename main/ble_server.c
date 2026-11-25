/**
 * @file    ble_server.c
 * @brief   BLE GATT 服务实现（NimBLE）。
 */
#include "ble_server.h"

#include <string.h>

#include "esp_log.h"
#include "esp_nimble_hci.h"
#include "esp_bt.h"                  /* esp_ble_tx_power_set 需要 */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "nvs_flash.h"

static const char *TAG = "ble_server";

/* ==================== UUID 定义 ====================
 * 注意 BLE_UUID128_INIT 的字节顺序是"小端"（低位字节在前），
 * 所以下面写的顺序看起来和目标 UUID 字符串是反的，这是正常的。
 *
 * 目标 UUID：
 *   服务  6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 *   RX    6E400002-B5A3-F393-E0A9-E50E24DCCA9E
 *   TX    6E400003-B5A3-F393-E0A9-E50E24DCCA9E
 */

static const ble_uuid128_t s_svc_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);

static const ble_uuid128_t s_rx_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);

static const ble_uuid128_t s_tx_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

/* ==================== 内部状态 ==================== */

static uint8_t  s_own_addr_type;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_handle   = 0;

static ble_server_rx_cb_t s_rx_cb = NULL;

/* ==================== 前向声明 ==================== */

static int  ble_gap_event_cb(struct ble_gap_event *event, void *arg);
static int  ble_gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg);
static void ble_on_sync(void);
static void ble_on_reset(int reason);
static void ble_host_task(void *param);
static void ble_advertise(void);

/* ==================== GATT 服务定义 ==================== */

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                /* RX：手机写入数据 */
                .uuid       = &s_rx_uuid.u,
                .access_cb  = ble_gatt_access_cb,
                .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                /* TX：ESP32 通知手机 */
                .uuid       = &s_tx_uuid.u,
                .access_cb  = ble_gatt_access_cb,
                .flags      = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_tx_handle,
            },
            {
                0,  /* 数组结束标记 */
            }
        },
    },
    {
        0,  /* 服务数组结束标记 */
    },
};

/* ==================== GATT 访问回调 ==================== */

static int ble_gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
    {
        /* 手机写入数据：取出全部字节交给上层 */
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        uint8_t  buf[512];

        if (len > sizeof(buf)) {
            /* 单次写入过长：BLE MTU 一般不会到这，但保险起见截断 */
            ESP_LOGW(TAG, "write too long: %u, truncated to %u",
                     len, (unsigned)sizeof(buf));
            len = (uint16_t)sizeof(buf);
        }

        if (ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL) != 0) {
            ESP_LOGE(TAG, "mbuf to flat failed");
            return BLE_ATT_ERR_UNLIKELY;
        }

        if (s_rx_cb != NULL) {
            s_rx_cb(buf, len);
        }

        return 0;
    }

    case BLE_GATT_ACCESS_OP_READ_CHR:
        /* 本项目的 TX 特征只做 Notify，不支持读取 */
        return BLE_ATT_ERR_READ_NOT_PERMITTED;

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

/* ==================== GAP 事件 ==================== */

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "phone connected, handle=%d", s_conn_handle);
        } else {
            ESP_LOGW(TAG, "connect failed, status=%d", event->connect.status);
            ble_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "phone disconnected, reason=0x%02X", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ble_advertise();        /* 断开后重新广播，等待再次连接 */
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ble_advertise();
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU updated: %d", event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe: attr=%d notify=%d",
                 event->subscribe.attr_handle, event->subscribe.cur_notify);
        return 0;

    default:
        return 0;
    }
}

/* ==================== 广播 ==================== */

/**
 * @brief 启动广播。
 *
 * @details 【BLE 广播包长度限制】这很容易踩坑，记录在此：
 *
 *   传统 BLE 广播（ADV_IND）有两个包，每个上限都是 **31 字节**：
 *     - 广播包      (Advertising Data)
 *     - 扫描响应包  (Scan Response)
 *
 *   各字段的开销（类型字节 + 长度字节 = 2 字节头）：
 *     Flags            3 字节
 *     128 位 UUID     18 字节   ← 非常占地方！
 *     设备名 "OLCD-ESP32" 12 字节
 *
 *   如果把「Flags + 128位UUID + 设备名」全塞进广播包：
 *     3 + 18 + 12 = 33 字节 > 31 字节  →  ble_gap_adv_set_fields()
 *     返回 BLE_HS_EMSGSIZE (4)，广播直接失败，手机搜不到设备。
 *
 *   所以必须拆分：
 *     广播包      = Flags + 设备名        (3 + 12 = 15 字节) OK
 *     扫描响应包  = 128 位服务 UUID       (18 字节)          OK
 *
 *   手机主动扫描时会把两个包合并，仍能按 UUID 过滤，功能不受影响。
 */
static void ble_advertise(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields  adv_fields;
    struct ble_hs_adv_fields  rsp_fields;
    const char *name;
    int rc;

    name = ble_svc_gap_device_name();

    /* ---------- 1. 广播包：只放 flags + 设备名 ---------- */
    memset(&adv_fields, 0, sizeof(adv_fields));
    adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv_fields.name = (const uint8_t *)name;
    adv_fields.name_len = (uint8_t)strlen(name);
    adv_fields.name_is_complete = 1;

    rc = ble_gap_adv_set_fields(&adv_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv set fields failed: %d (BLE_HS_EMSGSIZE=4 表示超 31 字节)",
                 rc);
        return;
    }

    /* ---------- 2. 扫描响应包：放 128 位服务 UUID ---------- */
    memset(&rsp_fields, 0, sizeof(rsp_fields));
    rsp_fields.uuids128 = &s_svc_uuid;
    rsp_fields.num_uuids128 = 1;
    rsp_fields.uuids128_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        /* 扫描响应失败不致命：手机仍能看到设备名并连接，
         * 只是无法按 UUID 过滤。这里只警告，继续启动广播。 */
        ESP_LOGW(TAG, "adv rsp set fields failed: %d (继续广播)", rc);
    }

    /* ---------- 3. 启动广播 ---------- */
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;   /* 可连接 */
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;   /* 通用可发现 */

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv start failed: %d", rc);
    } else {
        ESP_LOGI(TAG, "advertising as \"%s\"", name);
    }
}

/* ==================== 协议栈生命周期 ==================== */

static void ble_on_sync(void)
{
    int rc;

    rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure addr failed: %d", rc);
        return;
    }

    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer addr type failed: %d", rc);
        return;
    }

    ble_advertise();
}

static void ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset, reason=%d", reason);
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();                  /* 内部是死循环，退出时才返回 */
    nimble_port_freertos_deinit();
}

/* ==================== 对外接口 ==================== */

esp_err_t ble_server_init(ble_server_rx_cb_t rx_cb)
{
    esp_err_t ret;

    s_rx_cb = rx_cb;

    /* 1. NVS（BLE 需要存配对信息） */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 2. NimBLE 端口初始化 */
    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble port init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 2.5 【关键】降低 BLE 发射功率，避免 brownout 复位
     *
     * 现象：芯片刚启动 BLE 射频就报
     *         E BOD: Brownout detector was triggered
     * 原因是射频启动瞬间电流突增（峰值 300~500mA），
     * 供电能力不足时电压跌破阈值，芯片保护性复位。
     *
     * 默认发射功率是 +9dBm，这里降到 0dBm。
     * 0dBm 的 BLE 通讯距离约 10~20 米，连手机完全够用，
     * 但瞬时电流能降低约 40%，对劣质 USB 线/供电有明显帮助。
     *
     * 注意：ESP32-S3 没有 BT_LE_DFT_TX_POWER_LEVEL 这个 Kconfig 项
     * （那是 C3 等芯片才有的），所以只能在这里用运行时 API 设置。
     */
    {
        esp_err_t tx_ret = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT,
                                                ESP_PWR_LVL_N0);
        if (tx_ret == ESP_OK) {
            ESP_LOGI(TAG, "BLE TX power set to 0dBm (默认 +9dBm，降低以减小电流峰值)");
        } else {
            ESP_LOGW(TAG, "set tx power failed: %s", esp_err_to_name(tx_ret));
        }

        /* 广播用的功率也一并降低 */
        (void)esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_N0);
        /* 连接后的数据通道功率 */
        (void)esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_CONN_HDL0, ESP_PWR_LVL_N0);
    }

    /* 3. 注册主机回调 */
    ble_hs_cfg.sync_cb  = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;

    /* 4. 注册 GATT 服务 */
    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts count failed: %d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts add failed: %d", rc);
        return ESP_FAIL;
    }

    /* 5. 设置设备名 */
    rc = ble_svc_gap_device_name_set("OLCD-ESP32");
    if (rc != 0) {
        ESP_LOGW(TAG, "set device name failed: %d", rc);
    }

    /* 6. 启动主机任务 */
    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "BLE server started");
    return ESP_OK;
}

esp_err_t ble_server_notify(const uint8_t *data, size_t len)
{
    struct os_mbuf *om;

    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_tx_handle == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (data == NULL || len == 0) {
        return ESP_OK;
    }

    om = ble_hs_mbuf_from_flat(data, (uint16_t)len);
    if (om == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int rc = ble_gatts_notify_custom(s_conn_handle, s_tx_handle, om);
    return (rc == 0) ? ESP_OK : ESP_FAIL;
}

int ble_server_is_connected(void)
{
    return (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) ? 1 : 0;
}
