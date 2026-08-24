#include "blufi.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
#include "board.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_event.h"
#include "esp_gap_ble_api.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "freertos/task.h"
#include "wifi_manager.h"
#include "wifi_board.h"

// ESP-Hosted BT controller support for ESP32-P4
// For ESP32-P4 with ESP-Hosted, the BT controller is on the co-processor (ESP32-C6).
// The hosted_hci_bluedroid_* functions are provided by the esp-hosted component
// when both CONFIG_ESP_HOSTED_ENABLED and CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID are set.

#if defined(CONFIG_ESP_HOSTED_ENABLED) && defined(CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID)
extern "C" {
#include "esp_hosted.h"
#include "esp_hosted_bluedroid.h"
#include "esp_bluedroid_hci.h"
}
#endif

#define BLUFI_DEVICE_NAME "Xiaozhi-Blufi"

extern "C" {
void esp_blufi_adv_start(void);

void esp_blufi_adv_stop(void);

void esp_blufi_disconnect(void);

void btc_blufi_report_error(esp_blufi_error_state_t state);

void esp_blufi_gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param);
}

#include <wifi_station.h>
#include "esp_crc.h"
#include "esp_random.h"
#include "mbedtls/md5.h"
#include "ssid_manager.h"

static const char* BLUFI_TAG = "BLUFI_CLASS";

static wifi_mode_t GetWifiModeWithFallback(const WifiManager& wifi) {
    if (wifi.IsConfigMode()) {
        return WIFI_MODE_AP;
    }
    if (wifi.IsInitialized() && wifi.IsConnected()) {
        return WIFI_MODE_STA;
    }

    wifi_mode_t mode = WIFI_MODE_STA;
    esp_wifi_get_mode(&mode);
    return mode;
}

Blufi& Blufi::GetInstance() {
    static Blufi instance;
    return instance;
}

Blufi::Blufi()
    : m_sec(nullptr),
      m_ble_is_connected(false),
      m_sta_connected(false),
      m_sta_got_ip(false),
      m_provisioned(false),
      m_deinited(false),
      m_sta_ssid_len(0),
      m_sta_is_connecting(false),
      m_conn_success_sent(false),
      m_conn_success_acked(false),
      m_conn_success_send_time(0) {
    memset(&m_sta_config, 0, sizeof(m_sta_config));
    memset(m_sta_bssid, 0, sizeof(m_sta_bssid));
    memset(m_sta_ssid, 0, sizeof(m_sta_ssid));
    memset(&m_sta_conn_info, 0, sizeof(m_sta_conn_info));
}

Blufi::~Blufi() {
    if (m_sec) {
        _security_deinit();
    }
}

esp_err_t Blufi::init() {
    ESP_LOGI(BLUFI_TAG, "Blufi::init: entry");
    esp_err_t ret = ESP_FAIL;
    inited_ = true;
    m_provisioned = false;
    m_deinited = false;
    m_wifi_list_requested = false;
    m_ap_records.clear();
    m_has_recent_scan_results = false;
    m_scan_in_progress = false;
    m_conn_success_sent = false;
    m_conn_success_acked = false;
    m_conn_success_send_time = 0;
    m_waiting_for_conn_result = false;

    ESP_LOGI(BLUFI_TAG, "Blufi::init: calling _controller_init");
    ret = _controller_init();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "BLUFI controller init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(BLUFI_TAG, "Blufi::init: calling _host_and_cb_init");
    ret = _host_and_cb_init();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "BLUFI host and cb init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(BLUFI_TAG, "Blufi::init: done, BLUFI VERSION %04x", esp_blufi_get_version());
    return ESP_OK;
}

esp_err_t Blufi::deinit() {
    esp_err_t ret = ESP_OK;

    ESP_LOGW(BLUFI_TAG, ">>>>>>>>>> Blufi::deinit() ENTRY, m_conn_success_sent=%d, m_conn_success_acked=%d, m_deinited=%d, m_provisioned=%d <<<<<<<<<<",
             m_conn_success_sent, m_conn_success_acked, m_deinited, m_provisioned);
    _unregister_scan_handler();
    _reset_scan_state();

    if (inited_) {
        if (m_deinited) {
            return ESP_OK;
        }
        m_deinited = true;

        if (m_ip_handler_instance != nullptr) {
            esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, m_ip_handler_instance);
            m_ip_handler_instance = nullptr;
        }
        if (m_disconnect_handler_instance != nullptr) {
            esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                                  m_disconnect_handler_instance);
            m_disconnect_handler_instance = nullptr;
        }

        ret = _host_deinit();
        if (ret) {
            ESP_LOGE(BLUFI_TAG, "Host deinit failed: %s", esp_err_to_name(ret));
        }
        ret = _controller_deinit();
        if (ret) {
            ESP_LOGE(BLUFI_TAG, "Controller deinit failed: %s", esp_err_to_name(ret));
        }
#if defined(CONFIG_ESP_HOSTED_ENABLED) && defined(CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID)
        // ESP32-P4 uses ESP-Hosted with external BT controller, no memory to release
#else
        {
            size_t before = esp_get_free_heap_size();
            esp_err_t r = esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
            if (r == ESP_OK) {
                ESP_LOGI(BLUFI_TAG, "BT memory released: +%u bytes", (unsigned int)(esp_get_free_heap_size() - before));
            } else {
                ESP_LOGW(BLUFI_TAG, "esp_bt_controller_mem_release failed: %s", esp_err_to_name(r));
            }
        }
#endif
    }

    {
        std::vector<wifi_ap_record_t>().swap(m_ap_records);
    }
    m_has_recent_scan_results = false;

    return ret;
}

esp_err_t Blufi::_host_init() {
    ESP_LOGI(BLUFI_TAG, "_host_init: entry");
    esp_err_t ret;

#if !defined(CONFIG_ESP_HOSTED_ENABLED) || !defined(CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID)
    ESP_LOGI(BLUFI_TAG, "_host_init: calling esp_bt_controller_init");
    ret = esp_bt_controller_init(esp_bt_controller_config_t{});
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "_host_init: esp_bt_controller_init failed: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }
    ESP_LOGI(BLUFI_TAG, "_host_init: controller_init OK, calling esp_bt_controller_enable");
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "_host_init: esp_bt_controller_enable(BLE) failed: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }
    ESP_LOGI(BLUFI_TAG, "_host_init: controller_enable OK");
#else
    ESP_LOGI(BLUFI_TAG, "_host_init: ESP-Hosted mode, opening HCI channel");
    hosted_hci_bluedroid_open();

    ESP_LOGI(BLUFI_TAG, "_host_init: attaching HCI driver to Bluedroid");
    esp_bluedroid_hci_driver_operations_t hci_ops = {};
    hci_ops.send = hosted_hci_bluedroid_send;
    hci_ops.check_send_available = hosted_hci_bluedroid_check_send_available;
    hci_ops.register_host_callback = hosted_hci_bluedroid_register_host_callback;

    ret = esp_bluedroid_attach_hci_driver(&hci_ops);
    if (ret != ESP_OK) {
        ESP_LOGE(BLUFI_TAG, "_host_init: esp_bluedroid_attach_hci_driver failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(BLUFI_TAG, "_host_init: HCI driver attached successfully");
#endif

    ESP_LOGI(BLUFI_TAG, "_host_init: calling esp_bluedroid_init");
    ret = esp_bluedroid_init();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "_host_init: esp_bluedroid_init failed: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }
    ESP_LOGI(BLUFI_TAG, "_host_init: bluedroid_init OK, calling esp_bluedroid_enable");
    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "_host_init: esp_bluedroid_enable failed: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }
#if !defined(CONFIG_ESP_HOSTED_ENABLED) || !defined(CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID)
    ESP_LOGI(BLUFI_TAG, "_host_init: bluedroid_enable OK, BD ADDR: " ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(esp_bt_dev_get_address()));
#else
    ESP_LOGI(BLUFI_TAG, "_host_init: bluedroid_enable OK (ESP-Hosted mode)");
#endif
    return ESP_OK;
}

esp_err_t Blufi::_host_deinit() {
    esp_err_t ret = esp_blufi_profile_deinit();
    if (ret != ESP_OK)
        return ret;

    ret = esp_bluedroid_disable();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s disable bluedroid failed: %s", __func__, esp_err_to_name(ret));
        return ESP_FAIL;
    }
    ret = esp_bluedroid_deinit();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s deinit bluedroid failed: %s", __func__, esp_err_to_name(ret));
        return ESP_FAIL;
    }

#if defined(CONFIG_ESP_HOSTED_ENABLED) && defined(CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID)
    ESP_LOGI(BLUFI_TAG, "_host_deinit: ESP-Hosted mode, closing HCI channel");
    hosted_hci_bluedroid_close();
#endif

    return ESP_OK;
}

esp_err_t Blufi::_gap_register_callback() {
    ESP_LOGI(BLUFI_TAG, "_gap_register_callback: entry");
    esp_err_t rc = esp_ble_gap_register_callback(esp_blufi_gap_event_handler);
    if (rc) {
        ESP_LOGE(BLUFI_TAG, "_gap_register_callback: esp_ble_gap_register_callback failed: %x", rc);
        return rc;
    }
    ESP_LOGI(BLUFI_TAG, "_gap_register_callback: calling esp_blufi_profile_init");
    rc = esp_blufi_profile_init();
    if (rc) {
        ESP_LOGE(BLUFI_TAG, "_gap_register_callback: esp_blufi_profile_init failed: %x", rc);
        return rc;
    }
    ESP_LOGI(BLUFI_TAG, "_gap_register_callback: done");
    return ESP_OK;
}

esp_err_t Blufi::_host_and_cb_init() {
    ESP_LOGI(BLUFI_TAG, "_host_and_cb_init: entry");
    static esp_blufi_callbacks_t blufi_callbacks = {
        .event_cb = &_event_callback_trampoline,
        .negotiate_data_handler = &_negotiate_data_handler_trampoline,
        .encrypt_func = &_encrypt_func_trampoline,
        .decrypt_func = &_decrypt_func_trampoline,
        .checksum_func = &_checksum_func_trampoline,
    };

    ESP_LOGI(BLUFI_TAG, "_host_and_cb_init: calling _host_init");
    esp_err_t ret = _host_init();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s initialise host failed: %s", __func__, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(BLUFI_TAG, "_host_and_cb_init: calling esp_blufi_register_callbacks");
    ret = esp_blufi_register_callbacks(&blufi_callbacks);
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s blufi register failed, error code = %x", __func__, ret);
        return ret;
    }

    ESP_LOGI(BLUFI_TAG, "_host_and_cb_init: calling _gap_register_callback");
    ret = _gap_register_callback();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s gap register failed, error code = %x", __func__, ret);
        return ret;
    }
    ESP_LOGI(BLUFI_TAG, "_host_and_cb_init: done");
    return ESP_OK;
}

esp_err_t Blufi::_controller_init() {
#if defined(CONFIG_ESP_HOSTED_ENABLED) && defined(CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID)
    ESP_LOGI(BLUFI_TAG, "_controller_init: ESP-Hosted mode, probing C6 BT capability");

    esp_hosted_coprocessor_fwver_t fwver = {};
    int ver_ret = esp_hosted_get_coprocessor_fwversion(&fwver);
    if (ver_ret == 0) {
        ESP_LOGI(BLUFI_TAG,
                 "_controller_init: slave FW version %u.%u.%u (rev=%d)",
                 (unsigned)fwver.major1, (unsigned)fwver.minor1, (unsigned)fwver.patch1,
                 (int)fwver.revision);
    } else {
        ESP_LOGW(BLUFI_TAG, "_controller_init: failed to query slave FW version (ret=%d)", ver_ret);
    }

    esp_err_t ret = esp_hosted_bt_controller_init();
    if (ret == ESP_OK) {
        ESP_LOGI(BLUFI_TAG, "_controller_init: esp_hosted_bt_controller_init OK (newer slave)");

        ret = esp_hosted_bt_controller_enable();
        if (ret != ESP_OK) {
            ESP_LOGE(BLUFI_TAG, "_controller_init: esp_hosted_bt_controller_enable failed: %s",
                     esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(BLUFI_TAG, "_controller_init: esp_hosted_bt_controller_enable OK");
    } else {
        ESP_LOGW(BLUFI_TAG,
                 "_controller_init: esp_hosted_bt_controller_init failed (%s). "
                 "Assuming legacy C6 factory firmware with BT already initialised.",
                 esp_err_to_name(ret));
    }
#else
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s initialize controller failed: %s", __func__, esp_err_to_name(ret));
        return ret;
    }
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s enable controller failed: %s", __func__, esp_err_to_name(ret));
        return ret;
    }
#endif
    return ESP_OK;
}

esp_err_t Blufi::_controller_deinit() {
#if defined(CONFIG_ESP_HOSTED_ENABLED) && defined(CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID)
    ESP_LOGI(BLUFI_TAG, "_controller_deinit: ESP-Hosted mode, no co-processor BT deinit needed");
    return ESP_OK;
#else
    esp_err_t ret = esp_bt_controller_disable();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s disable controller failed: %s", __func__, esp_err_to_name(ret));
    }
    ret = esp_bt_controller_deinit();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s deinit controller failed: %s", __func__, esp_err_to_name(ret));
    }
    return ret;
#endif
}

static int myrand(void* rng_state, unsigned char* output, size_t len) {
    esp_fill_random(output, len);
    return 0;
}

void Blufi::_security_init() {
    m_sec = new BlufiSecurity();
    if (m_sec == nullptr) {
        ESP_LOGE(BLUFI_TAG, "Failed to allocate security context");
        return;
    }
    memset(m_sec, 0, sizeof(BlufiSecurity));
    m_sec->dhm = new mbedtls_dhm_context();
    m_sec->aes = new mbedtls_aes_context();

    mbedtls_dhm_init(m_sec->dhm);
    mbedtls_aes_init(m_sec->aes);

    memset(m_sec->iv, 0x0, sizeof(m_sec->iv));
}

void Blufi::_security_deinit() {
    if (m_sec == nullptr)
        return;

    if (m_sec->dh_param) {
        free(m_sec->dh_param);
    }
    mbedtls_dhm_free(m_sec->dhm);
    mbedtls_aes_free(m_sec->aes);
    delete m_sec->dhm;
    delete m_sec->aes;
    delete m_sec;
    m_sec = nullptr;
}

void Blufi::_dh_negotiate_data_handler(uint8_t* data, int len, uint8_t** output_data,
                                       int* output_len, bool* need_free) {
    if (m_sec == nullptr) {
        ESP_LOGE(BLUFI_TAG, "Security not initialized in DH handler");
        btc_blufi_report_error(ESP_BLUFI_INIT_SECURITY_ERROR);
        return;
    }

    if (len < 1) {
        ESP_LOGE(BLUFI_TAG, "DH handler: data too short");
        btc_blufi_report_error(ESP_BLUFI_DATA_FORMAT_ERROR);
        return;
    }

    uint8_t type = data[0];
    switch (type) {
        case 0x00:
            if (len < 3) {
                ESP_LOGE(BLUFI_TAG, "DH_PARAM_LEN packet too short");
                btc_blufi_report_error(ESP_BLUFI_DATA_FORMAT_ERROR);
                return;
            }

            m_sec->dh_param_len = (data[1] << 8) | data[2];
            if (m_sec->dh_param) {
                free(m_sec->dh_param);
                m_sec->dh_param = nullptr;
            }
            m_sec->dh_param = (uint8_t*)malloc(m_sec->dh_param_len);
            if (m_sec->dh_param == nullptr) {
                ESP_LOGE(BLUFI_TAG, "DH malloc failed");
                btc_blufi_report_error(ESP_BLUFI_DH_MALLOC_ERROR);
            }
            break;
        case 0x01: {
            if (m_sec->dh_param == nullptr) {
                ESP_LOGE(BLUFI_TAG, "DH param not allocated");
                btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
                return;
            }
            uint8_t* param = m_sec->dh_param;
            memcpy(m_sec->dh_param, &data[1], m_sec->dh_param_len);
            int ret = mbedtls_dhm_read_params(m_sec->dhm, &param, &param[m_sec->dh_param_len]);
            if (ret) {
                ESP_LOGE(BLUFI_TAG, "mbedtls_dhm_read_params failed %d", ret);
                btc_blufi_report_error(ESP_BLUFI_READ_PARAM_ERROR);
                return;
            }

            const int dhm_len = mbedtls_dhm_get_len(m_sec->dhm);

            ret = mbedtls_dhm_make_public(m_sec->dhm, dhm_len, m_sec->self_public_key, dhm_len,
                                          myrand, NULL);
            if (ret != 0) {
                ESP_LOGE(BLUFI_TAG, "mbedtls_dhm_make_public failed: %d", ret);
                btc_blufi_report_error(ESP_BLUFI_MAKE_PUBLIC_ERROR);
                return;
            }
            ret = mbedtls_dhm_calc_secret(m_sec->dhm, m_sec->share_key, SHARE_KEY_LEN,
                                          &m_sec->share_len, myrand, NULL);
            if (ret != 0) {
                ESP_LOGE(BLUFI_TAG, "mbedtls_dhm_calc_secret failed: %d", ret);
                btc_blufi_report_error(ESP_BLUFI_ENCRYPT_ERROR);
                return;
            }

            ret = mbedtls_md5(m_sec->share_key, m_sec->share_len, m_sec->psk);
            if (ret != 0) {
                ESP_LOGE(BLUFI_TAG, "mbedtls_md5 failed: %d", ret);
                btc_blufi_report_error(ESP_BLUFI_CALC_MD5_ERROR);
                return;
            }
            ret = mbedtls_aes_setkey_enc(m_sec->aes, m_sec->psk, PSK_LEN * 8);
            if (ret != 0) {
                ESP_LOGE(BLUFI_TAG, "mbedtls_aes_setkey_enc failed: -0x%04X", -ret);
                btc_blufi_report_error(ESP_BLUFI_ENCRYPT_ERROR);
                return;
            }
            *output_data = m_sec->self_public_key;
            *output_len = dhm_len;
            *need_free = false;
            ESP_LOGI(BLUFI_TAG, "DH negotiation completed successfully");

            free(m_sec->dh_param);
            m_sec->dh_param = nullptr;
            m_sec->dh_param_len = 0;
            break;
        }
        default:
            ESP_LOGE(BLUFI_TAG, "DH handler unknown type: %d", type);
    }
}

int Blufi::_aes_encrypt(uint8_t iv8, uint8_t* crypt_data, int crypt_len) {
    if (!m_sec || !m_sec->aes || !crypt_data || crypt_len <= 0) {
        ESP_LOGE(BLUFI_TAG, "Invalid parameters for AES encryption");
        return -ESP_ERR_INVALID_ARG;
    }

    size_t iv_offset = 0;
    uint8_t iv0[16];
    memcpy(iv0, m_sec->iv, 16);
    iv0[0] = iv8;
    int ret = mbedtls_aes_crypt_cfb128(m_sec->aes, MBEDTLS_AES_ENCRYPT, crypt_len, &iv_offset, iv0,
                                       crypt_data, crypt_data);

    if (ret == 0) {
        return crypt_len;
    } else {
        ESP_LOGE(BLUFI_TAG, "AES encrypt failed: %d", ret);
        return ret;
    }
}

int Blufi::_aes_decrypt(uint8_t iv8, uint8_t* crypt_data, int crypt_len) {
    if (!m_sec || !m_sec->aes || !crypt_data || crypt_len < 0) {
        ESP_LOGE(BLUFI_TAG, "Invalid parameters for AES decryption %p %p %d", m_sec->aes,
                 crypt_data, crypt_len);
        return -ESP_ERR_INVALID_ARG;
    }

    size_t iv_offset = 0;
    uint8_t iv0[16];
    memcpy(iv0, m_sec->iv, 16);
    iv0[0] = iv8;
    int ret = mbedtls_aes_crypt_cfb128(m_sec->aes, MBEDTLS_AES_DECRYPT, crypt_len, &iv_offset, iv0,
                                       crypt_data, crypt_data);
    if (ret != 0) {
        ESP_LOGE(BLUFI_TAG, "AES decrypt failed: %d", ret);
        return ret;
    } else {
        return crypt_len;
    }
}

uint16_t Blufi::_crc_checksum(uint8_t iv8, uint8_t* data, int len) {
    return esp_crc16_be(0, data, len);
}

int Blufi::_get_softap_conn_num() {
    auto& wifi = WifiManager::GetInstance();
    if (!wifi.IsInitialized() || !wifi.IsConfigMode()) {
        return 0;
    }

    wifi_sta_list_t sta_list{};
    if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
        return sta_list.num;
    }
    return 0;
}

void Blufi::_register_scan_handler() {
    _unregister_scan_handler();
    esp_err_t err = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                                        &_wifi_scan_event_handler, this,
                                                        &m_scan_handler_instance);
    if (err != ESP_OK) {
        ESP_LOGW(BLUFI_TAG, "Failed to register scan handler: %s", esp_err_to_name(err));
    }
}

void Blufi::_unregister_scan_handler() {
    if (m_scan_handler_instance != nullptr) {
        esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, m_scan_handler_instance);
        m_scan_handler_instance = nullptr;
    }
}

void Blufi::_reset_scan_state() {
    m_scan_in_progress = false;
    m_scan_should_save_ssid = true;
    m_wifi_list_requested = false;
}

void Blufi::_ip_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id,
                              void* event_data) {
    auto* self = static_cast<Blufi*>(arg);
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(BLUFI_TAG, "IP_EVENT_STA_GOT_IP received, m_ble_is_connected=%d",
                 self->m_ble_is_connected);
        self->_on_got_ip();
    }
}

void Blufi::_on_got_ip() {
    ESP_LOGI(BLUFI_TAG, "_on_got_ip: ENTER, m_ble_is_connected=%d, m_conn_success_sent=%d, m_deinited=%d",
             m_ble_is_connected, m_conn_success_sent, m_deinited);

    if (!m_ble_is_connected) {
        ESP_LOGI(BLUFI_TAG, "Got IP but BLE disconnected, deferring to main flow");
        // 即使蓝牙已断开,仍要标记 provisioned 让主程序拿到 Connected 事件
        m_provisioned = true;
        return;
    }

    auto& wifi = WifiManager::GetInstance();
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);

    auto current_ssid = wifi.GetSsid();
    if (!current_ssid.empty()) {
        m_sta_ssid_len = static_cast<int>(std::min(current_ssid.size(), sizeof(m_sta_ssid)));
        memcpy(m_sta_ssid, current_ssid.c_str(), m_sta_ssid_len);
    }

    wifi_ap_record_t ap_info{};
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        memcpy(m_sta_bssid, ap_info.bssid, sizeof(m_sta_bssid));
    }

    esp_blufi_extra_info_t info = {};
    memcpy(info.sta_bssid, m_sta_bssid, sizeof(m_sta_bssid));
    info.sta_bssid_set = true;
    info.sta_ssid = m_sta_ssid;
    info.sta_ssid_len = m_sta_ssid_len;

    if (m_conn_success_sent) {
        ESP_LOGI(BLUFI_TAG, "CONN_SUCCESS already sent, skip duplicate IP report");
        return;
    }

    ESP_LOGI(BLUFI_TAG, "Sending WiFi connected status report via IP event handler");
    ESP_LOGI(BLUFI_TAG, "  - opmode=%d, state=ESP_BLUFI_STA_CONN_SUCCESS(0), ssid_len=%d",
             mode, m_sta_ssid_len);
    ESP_LOGI(BLUFI_TAG, "  - ssid='%s'", m_sta_ssid);
    ESP_LOGI(BLUFI_TAG, "  - BLE notify path: %s (GATT subscribed)",
             m_ble_is_connected ? "active" : "INACTIVE!");
    ESP_LOGI(BLUFI_TAG, "  - m_conn_success_acked=%d, m_conn_success_send_time=%lld us",
             m_conn_success_acked, m_conn_success_send_time);

    esp_err_t err = esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_SUCCESS, 0, &info);
    m_conn_success_sent = (err == ESP_OK);
    m_conn_success_send_time = esp_timer_get_time();

    ESP_LOGI(BLUFI_TAG, "CONN_SUCCESS via IP_EVENT sent: %s", esp_err_to_name(err));
    ESP_LOGI(BLUFI_TAG, "  - m_conn_success_sent now=%d, waiting for phone ACK...",
             m_conn_success_sent);
    ESP_LOGI(BLUFI_TAG, "  - ESP32 will deinit BLE when phone sends GET_WIFI_STATUS (or 30s fallback)");

    // === 新增: 主程序一定要看到 m_provisioned=true 才能切换到 activating 状态 ===
    m_provisioned = true;

    // WiFi 连接成功，保存 SSID 和密码到 NVS
    std::string password(reinterpret_cast<const char*>(m_sta_config.sta.password));
    if (!current_ssid.empty() && !password.empty()) {
        ESP_LOGI(BLUFI_TAG, "Saving WiFi credentials to NVS: SSID=%s", current_ssid.c_str());
        SsidManager::GetInstance().AddSsid(current_ssid, password);
    }

    // === 新增: CONN_SUCCESS 重发守护任务 ===
    // 问题: BLE notify 在某些时序下会丢包(尤其 P4+C6 走 ESP-Hosted 透传时),
    //       手机可能收不到第一次 CONN_SUCCESS,导致配网页一直转圈。
    // 解决: 启动一个守护任务,每 2 秒检查一次,如果 5 秒内没收到 ACK,
    //       重发 CONN_SUCCESS,最多 3 次。
    if (err == ESP_OK && m_ble_is_connected && !m_deinited) {
        ESP_LOGI(BLUFI_TAG, "  - Starting CONN_SUCCESS retry watchdog (3 attempts, 2s interval)");
        xTaskCreate(
            [](void* ctx) {
                auto* self = static_cast<Blufi*>(ctx);
                constexpr int kRetryIntervalMs = 2000;
                constexpr int kMaxRetries = 3;
                int retry = 0;

                while (retry < kMaxRetries && !self->m_conn_success_acked && !self->m_deinited) {
                    vTaskDelay(pdMS_TO_TICKS(kRetryIntervalMs));

                    if (self->m_conn_success_acked || self->m_deinited) {
                        ESP_LOGI(BLUFI_TAG, "  - Watchdog: ACK/deinit detected, exit");
                        break;
                    }

                    if (!self->m_ble_is_connected) {
                        ESP_LOGW(BLUFI_TAG, "  - Watchdog: BLE disconnected, exit");
                        break;
                    }

                    retry++;
                    ESP_LOGW(BLUFI_TAG, "  - Watchdog: retry #%d, re-sending CONN_SUCCESS", retry);

                    wifi_mode_t m;
                    esp_wifi_get_mode(&m);
                    esp_blufi_extra_info_t retry_info = {};
                    memcpy(retry_info.sta_bssid, self->m_sta_bssid, sizeof(self->m_sta_bssid));
                    retry_info.sta_bssid_set = true;
                    retry_info.sta_ssid = self->m_sta_ssid;
                    retry_info.sta_ssid_len = self->m_sta_ssid_len;

                    esp_err_t r = esp_blufi_send_wifi_conn_report(m, ESP_BLUFI_STA_CONN_SUCCESS, 0, &retry_info);
                    ESP_LOGW(BLUFI_TAG, "  - Watchdog: retry #%d sent: %s", retry, esp_err_to_name(r));
                }

                if (retry >= kMaxRetries && !self->m_conn_success_acked && !self->m_deinited) {
                    ESP_LOGE(BLUFI_TAG, "  - Watchdog: %d retries exhausted, force deinit BLE", kMaxRetries);
                    self->deinit();
                }

                vTaskDelete(nullptr);
            },
            "blufi_conn_retry", 3072, this, 4, nullptr);
    }
}

void Blufi::_sta_disconnect_event_handler(void* arg, esp_event_base_t event_base,
                                          int32_t event_id, void* event_data) {
    auto* self = static_cast<Blufi*>(arg);

    if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        auto* ev = static_cast<wifi_event_sta_disconnected_t*>(event_data);
        uint8_t reason = ev->reason;
        self->m_last_disconnect_reason = reason;

        ESP_LOGW(BLUFI_TAG, "WiFi STA disconnected, reason=0x%02x (%s)",
                 reason, self->_disconnect_reason_str(reason));

        if (self->m_conn_success_sent) {
            ESP_LOGI(BLUFI_TAG, "Disconnect after CONN_SUCCESS, ignoring");
            return;
        }

        if (self->m_waiting_for_conn_result) {
            ESP_LOGW(BLUFI_TAG, "WiFi connect failed (reason=%d), notifying phone immediately", reason);

            wifi_mode_t mode;
            esp_wifi_get_mode(&mode);

            esp_blufi_extra_info_t info = {};
            info.sta_ssid = self->m_sta_ssid;
            info.sta_ssid_len = self->m_sta_ssid_len;
            info.sta_bssid_set = false;

            esp_err_t err = esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_FAIL, 0, &info);
            ESP_LOGI(BLUFI_TAG, "STA_CONN_FAIL sent: %s", esp_err_to_name(err));

            const char* err_detail = self->_disconnect_reason_str(reason);
            char payload[128];
            int len = snprintf(payload, sizeof(payload),
                               "{\"blufi_err\":{\"code\":%d,\"reason\":\"%s\"}}",
                               reason, err_detail);
            esp_err_t r = esp_blufi_send_custom_data((uint8_t*)payload, len);
            ESP_LOGI(BLUFI_TAG, "blufi_err detail sent: %s, result=%d", payload, r);

            self->m_waiting_for_conn_result = false;
        }
    }
}

const char* Blufi::_disconnect_reason_str(uint8_t reason) {
    switch (reason) {
        case WIFI_REASON_AUTH_EXPIRE:           return "AUTH_EXPIRE";
        case WIFI_REASON_AUTH_LEAVE:           return "AUTH_LEAVE";
        case WIFI_REASON_ASSOC_NOT_AUTHED:     return "ASSOC_NOT_AUTHED";
        case WIFI_REASON_DISASSOC_PWRCAP_BAD:  return "DISASSOC_PWRCAP_BAD";
        case WIFI_REASON_NOT_AUTHED:           return "NOT_AUTHED";
        case WIFI_REASON_NOT_ASSOCED:          return "NOT_ASSOCED";
        case WIFI_REASON_ASSOC_LEAVE:          return "ASSOC_LEAVE";
        case WIFI_REASON_IE_INVALID:           return "IE_INVALID";
        case WIFI_REASON_MIC_FAILURE:          return "MIC_FAILURE";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4WAY_HANDSHAKE_TIMEOUT";
        case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT: return "GROUP_KEY_UPDATE_TIMEOUT";
        case WIFI_REASON_IE_IN_4WAY_DIFFERS:  return "IE_IN_4WAY_DIFFERS";
        case WIFI_REASON_GROUP_CIPHER_INVALID: return "GROUP_CIPHER_INVALID";
        case WIFI_REASON_PAIRWISE_CIPHER_INVALID: return "PAIRWISE_CIPHER_INVALID";
        case WIFI_REASON_AKMP_INVALID:         return "AKMP_INVALID";
        case WIFI_REASON_UNSUPP_RSN_IE_VERSION: return "UNSUPP_RSN_IE_VERSION";
        case WIFI_REASON_INVALID_RSN_IE_CAP:   return "INVALID_RSN_IE_CAP";
        case WIFI_REASON_802_1X_AUTH_FAILED:   return "802_1X_AUTH_FAILED";
        case WIFI_REASON_CIPHER_SUITE_REJECTED: return "CIPHER_SUITE_REJECTED";
        case WIFI_REASON_INVALID_PMKID:        return "INVALID_PMKID";
        case WIFI_REASON_INVALID_MDE:          return "INVALID_MDE";
        case WIFI_REASON_INVALID_FTE:          return "INVALID_FTE";
        case WIFI_REASON_BEACON_TIMEOUT:       return "BEACON_TIMEOUT";
        case WIFI_REASON_NO_AP_FOUND:          return "NO_AP_FOUND";
        case WIFI_REASON_AUTH_FAIL:            return "AUTH_FAIL";
        case WIFI_REASON_ASSOC_FAIL:           return "ASSOC_FAIL";
        case WIFI_REASON_HANDSHAKE_TIMEOUT:     return "HANDSHAKE_TIMEOUT";
        case WIFI_REASON_CONNECTION_FAIL:       return "CONNECTION_FAIL";
        case WIFI_REASON_AP_TSF_RESET:         return "AP_TSF_RESET";
        case WIFI_REASON_ROAMING:              return "ROAMING";
        default: {
            static char buf[16];
            snprintf(buf, sizeof(buf), "UNKNOWN_0x%02X", reason);
            return buf;
        }
    }
}

void Blufi::_ensure_disconnect_handler_registered() {
    if (m_disconnect_handler_instance == nullptr) {
        esp_err_t err = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                                           &_sta_disconnect_event_handler, this,
                                                           &m_disconnect_handler_instance);
        if (err == ESP_OK) {
            ESP_LOGI(BLUFI_TAG, "Disconnect handler registered on-demand");
        } else {
            ESP_LOGW(BLUFI_TAG, "Failed to register disconnect handler: %s", esp_err_to_name(err));
        }
    }
    if (m_ip_handler_instance == nullptr) {
        esp_err_t err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                           &_ip_event_handler, this,
                                                           &m_ip_handler_instance);
        if (err == ESP_OK) {
            ESP_LOGI(BLUFI_TAG, "IP handler registered on-demand");
        } else {
            ESP_LOGW(BLUFI_TAG, "Failed to register IP handler: %s", esp_err_to_name(err));
        }
    }
}

void Blufi::start_wifi_scan() {
    ESP_LOGI(BLUFI_TAG, "Starting dedicated WiFi scan");

    if (m_scan_in_progress) {
        ESP_LOGW(BLUFI_TAG, "Scan already in progress, skipping");
        return;
    }

    m_scan_in_progress = true;

    auto& wifi = WifiManager::GetInstance();
    if (wifi.IsInitialized()) {
        wifi.StopStation();
    }
    wifi.DisableScan();

    wifi_mode_t current_mode;
    esp_err_t err = esp_wifi_get_mode(&current_mode);

    if (current_mode == WIFI_MODE_AP || current_mode == WIFI_MODE_APSTA) {
        ESP_LOGI(BLUFI_TAG, "Switching WiFi mode from AP/APSTA to STA before scan");
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) {
            ESP_LOGE(BLUFI_TAG, "esp_wifi_set_mode(WIFI_MODE_STA) failed: %s", esp_err_to_name(err));
            m_scan_in_progress = false;
            wifi.EnableScan();
            return;
        }
    }

    err = esp_wifi_start();
    ESP_LOGI(BLUFI_TAG, "esp_wifi_start -> %s", esp_err_to_name(err));
    if (err == ESP_ERR_WIFI_STATE) {
        ESP_LOGI(BLUFI_TAG, "WiFi already started (ESP_ERR_WIFI_STATE)");
    } else if (err != ESP_OK) {
        ESP_LOGE(BLUFI_TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        m_scan_in_progress = false;
        wifi.EnableScan();
        return;
    }

    _register_scan_handler();
    err = esp_wifi_scan_start(NULL, false);
    ESP_LOGI(BLUFI_TAG, "esp_wifi_scan_start -> %s", esp_err_to_name(err));
    if (err == ESP_ERR_WIFI_STATE) {
        ESP_LOGI(BLUFI_TAG, "Scan already in progress");
    } else if (err != ESP_OK) {
        ESP_LOGE(BLUFI_TAG, "esp_wifi_scan_start FAILED: %s", esp_err_to_name(err));
        m_scan_in_progress = false;
        WifiManager::GetInstance().EnableScan();
        return;
    }

    ESP_LOGI(BLUFI_TAG, "WiFi scan started successfully");
}

void Blufi::_send_wifi_list() {
    if (m_ap_records.empty()) {
        ESP_LOGW(BLUFI_TAG, "No AP records available to send");
        return;
    }

    std::vector<wifi_ap_record_t> sorted_aps = m_ap_records;
    std::sort(sorted_aps.begin(), sorted_aps.end(), [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) {
        return a.rssi > b.rssi;
    });

    const size_t kMaxSendAps = 20;
    size_t send_count = std::min(sorted_aps.size(), kMaxSendAps);

    ESP_LOGI(BLUFI_TAG, "Sending WiFi list with %d APs (total cached: %d)", send_count, m_ap_records.size());

    std::vector<esp_blufi_ap_record_t> blufi_ap_list;
    blufi_ap_list.resize(send_count);
    for (size_t i = 0; i < send_count; ++i) {
        const auto& ap = sorted_aps[i];
        auto& blufi_ap = blufi_ap_list[i];
        memset(&blufi_ap, 0, sizeof(blufi_ap));
        memcpy(blufi_ap.ssid, ap.ssid, std::min((size_t)32, sizeof(ap.ssid)));
        blufi_ap.rssi = ap.rssi;
    }

    esp_blufi_send_wifi_list(blufi_ap_list.size(), blufi_ap_list.data());

    m_wifi_list_requested = false;
    ESP_LOGI(BLUFI_TAG, "Retaining %d APs for connect hints", m_ap_records.size());
}

void Blufi::_wifi_scan_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id,
                                     void* event_data) {
    Blufi* self = static_cast<Blufi*>(arg);

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        if (!self->m_scan_in_progress) {
            ESP_LOGD(BLUFI_TAG, "Ignoring non-Blufi scan done event");
            return;
        }

        ESP_LOGI(BLUFI_TAG, "WiFi scan done");

        uint16_t ap_num = 0;
        {
            std::lock_guard<std::mutex> lock(WifiManager::GetScanMutex());
            esp_wifi_scan_get_ap_num(&ap_num);

            if (ap_num > 0 && self->m_scan_should_save_ssid) {
                self->m_ap_records.resize(ap_num);
                esp_wifi_scan_get_ap_records(&ap_num, self->m_ap_records.data());
                self->m_has_recent_scan_results = true;

                ESP_LOGI(BLUFI_TAG, "Found %d APs", ap_num);
            }
        }

        if (ap_num == 0 && !self->m_has_recent_scan_results) {
            ESP_LOGW(BLUFI_TAG, "No APs found, retrying scan once...");
            self->m_has_recent_scan_results = true;
            WifiManager::GetInstance().DisableScan();
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_err_t retry_err = esp_wifi_scan_start(NULL, false);
            ESP_LOGI(BLUFI_TAG, "Retry scan -> %s", esp_err_to_name(retry_err));
            if (retry_err == ESP_OK || retry_err == ESP_ERR_WIFI_STATE) {
                self->m_scan_should_save_ssid = true;
                return;
            }
            ESP_LOGW(BLUFI_TAG, "Retry scan failed: %s", esp_err_to_name(retry_err));
        }

        self->m_scan_in_progress = false;
        WifiManager::GetInstance().EnableScan();

        if (self->m_wifi_list_requested) {
            self->m_wifi_list_requested = false;
            self->_send_wifi_list();
        }

        self->_unregister_scan_handler();
    }
}

void Blufi::_handle_event(esp_blufi_cb_event_t event, esp_blufi_cb_param_t* param) {
    switch (event) {
        case ESP_BLUFI_EVENT_INIT_FINISH:
            ESP_LOGI(BLUFI_TAG, "BLUFI init finish");
            esp_ble_gap_set_device_name(BLUFI_DEVICE_NAME);
            esp_blufi_adv_start();
            break;
        case ESP_BLUFI_EVENT_DEINIT_FINISH:
            ESP_LOGI(BLUFI_TAG, "BLUFI deinit finish");
            break;
        case ESP_BLUFI_EVENT_BLE_CONNECT:
            ESP_LOGI(BLUFI_TAG, "BLUFI ble connect - phone APP connected via BLE GATT");
            m_ble_is_connected = true;
            esp_blufi_adv_stop();
            _security_init();
            m_ap_records.clear();
            m_has_recent_scan_results = false;
            m_scan_in_progress = false;
            m_wifi_list_requested = false;
            m_conn_success_sent = false;
            m_conn_success_acked = false;
            m_conn_success_send_time = 0;
            m_waiting_for_conn_result = false;
            ESP_LOGI(BLUFI_TAG, "  - State reset: m_ble_is_connected=true, m_conn_success_sent=false");
            if (m_ip_handler_instance == nullptr) {
                ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                    &_ip_event_handler, this, &m_ip_handler_instance));
                ESP_LOGI(BLUFI_TAG, "IP event handler registered");
            }
            if (m_disconnect_handler_instance == nullptr) {
                ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                                    &_sta_disconnect_event_handler, this,
                                                    &m_disconnect_handler_instance));
                ESP_LOGI(BLUFI_TAG, "Disconnect event handler registered");
            }
            break;
        case ESP_BLUFI_EVENT_BLE_DISCONNECT:
            ESP_LOGI(BLUFI_TAG, "BLUFI ble disconnect");
            m_ble_is_connected = false;
            _security_deinit();
            _unregister_scan_handler();
            _reset_scan_state();
            WifiManager::GetInstance().EnableScan();
            if (m_ip_handler_instance != nullptr) {
                esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, m_ip_handler_instance);
                m_ip_handler_instance = nullptr;
                ESP_LOGI(BLUFI_TAG, "IP event handler unregistered in BLE_DISCONNECT");
            }
            if (m_disconnect_handler_instance != nullptr) {
                esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                                     m_disconnect_handler_instance);
                m_disconnect_handler_instance = nullptr;
                ESP_LOGI(BLUFI_TAG, "Disconnect event handler unregistered in BLE_DISCONNECT");
            }
            if (!m_provisioned) {
                esp_blufi_adv_start();
            } else {
                esp_blufi_adv_stop();
                if (!m_deinited) {
                    deinit();
                }
            }
            break;
        case ESP_BLUFI_EVENT_SET_WIFI_OPMODE: {
            ESP_LOGI(BLUFI_TAG, "BLUFI Set WIFI opmode %d", param->wifi_mode.op_mode);
            auto& wifi_manager = WifiManager::GetInstance();
            if (!wifi_manager.IsInitialized() && !wifi_manager.Initialize()) {
                ESP_LOGE(BLUFI_TAG, "Failed to initialize WifiManager for opmode change");
                break;
            }
            // 仅初始化 WifiManager,真正 StartStation 留到 RECV_STA_PASSWD
            // 否则 RECV_STA_PASSWD 后 StartStation(hint) 会先 Stop 再 Start,中间浪费一轮空扫
            break;
        }
        case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP: {
            ESP_LOGI(BLUFI_TAG, "BLUFI request connect - sending CONNECTING status");
            // 真正的连接由 RECV_STA_PASSWD 触发;这里只把状态置为 connecting 并回包给手机
            m_sta_is_connecting = true;
            m_sta_connected = false;
            m_sta_got_ip = false;
            if (m_ble_is_connected) {
                wifi_mode_t mode;
                esp_wifi_get_mode(&mode);
                esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONNECTING, 0, &m_sta_conn_info);
            }
            break;
        }
        case ESP_BLUFI_EVENT_REQ_DISCONNECT_FROM_AP:
            ESP_LOGI(BLUFI_TAG, "BLUFI request wifi disconnect from AP");
            if (WifiManager::GetInstance().IsInitialized()) {
                WifiManager::GetInstance().StopStation();
            }
            m_sta_is_connecting = false;
            m_sta_connected = false;
            m_sta_got_ip = false;
            break;
        case ESP_BLUFI_EVENT_GET_WIFI_STATUS: {
            auto& wifi = WifiManager::GetInstance();
            wifi_mode_t mode = GetWifiModeWithFallback(wifi);
            const int softap_conn_num = _get_softap_conn_num();

            // 收到手机请求任意 BLUFI 数据 = 手机已收到 CONN_SUCCESS = ACK!
            if (m_conn_success_sent && !m_conn_success_acked) {
                m_conn_success_acked = true;
                int64_t ack_time = esp_timer_get_time();
                ESP_LOGI(BLUFI_TAG, "GET_WIFI_STATUS: phone ACK detected! elapsed=%lld ms, deinit BLE",
                         (ack_time - m_conn_success_send_time) / 1000);
                if (!m_deinited) {
                    deinit();
                }
            }

            if (wifi.IsInitialized() && wifi.IsConnected()) {
                m_sta_connected = true;
                m_sta_got_ip = true;

                auto current_ssid = wifi.GetSsid();
                if (!current_ssid.empty()) {
                    m_sta_ssid_len =
                        static_cast<int>(std::min(current_ssid.size(), sizeof(m_sta_ssid)));
                    memcpy(m_sta_ssid, current_ssid.c_str(), m_sta_ssid_len);
                }

                wifi_ap_record_t ap_info{};
                if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                    memcpy(m_sta_bssid, ap_info.bssid, sizeof(m_sta_bssid));
                }

                esp_blufi_extra_info_t info;
                memset(&info, 0, sizeof(esp_blufi_extra_info_t));
                memcpy(info.sta_bssid, m_sta_bssid, 6);
                info.sta_ssid = m_sta_ssid;
                info.sta_ssid_len = m_sta_ssid_len;
                info.sta_bssid_set = true;

                if (!m_conn_success_sent) {
                    ESP_LOGI(BLUFI_TAG, "GET_WIFI_STATUS: WiFi connected, sending CONN_SUCCESS once");
                    esp_err_t err = esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_SUCCESS, softap_conn_num,
                                                    &info);
                    m_conn_success_sent = (err == ESP_OK);
                    m_conn_success_send_time = esp_timer_get_time();
                    ESP_LOGI(BLUFI_TAG, "CONN_SUCCESS sent: %s", esp_err_to_name(err));
                }
            } else if (m_sta_is_connecting) {
                esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONNECTING, softap_conn_num,
                                                &m_sta_conn_info);
                ESP_LOGI(BLUFI_TAG, "BLUFI get wifi status: connecting");
            } else {
                esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_FAIL, softap_conn_num,
                                                &m_sta_conn_info);
                ESP_LOGI(BLUFI_TAG, "BLUFI get wifi status: not connected");
            }
            break;
        }
        case ESP_BLUFI_EVENT_RECV_STA_BSSID:
            memcpy(m_sta_config.sta.bssid, param->sta_bssid.bssid, 6);
            m_sta_config.sta.bssid_set = true;
            ESP_LOGI(BLUFI_TAG, "Recv STA BSSID");
            break;
        case ESP_BLUFI_EVENT_RECV_STA_SSID:
            strncpy((char*)m_sta_config.sta.ssid, (char*)param->sta_ssid.ssid,
                    param->sta_ssid.ssid_len);
            m_sta_config.sta.ssid[param->sta_ssid.ssid_len] = '\0';
            ESP_LOGI(BLUFI_TAG, "Recv STA SSID: %s", m_sta_config.sta.ssid);
            break;
        case ESP_BLUFI_EVENT_RECV_STA_PASSWD: {
            strncpy((char*)m_sta_config.sta.password, (char*)param->sta_passwd.passwd,
                    param->sta_passwd.passwd_len);
            m_sta_config.sta.password[param->sta_passwd.passwd_len] = '\0';
            ESP_LOGI(BLUFI_TAG, "Recv STA PASSWORD : %s", m_sta_config.sta.password);

            // ===== 核心修复点 =====
            // 旧逻辑: 自己用 esp_wifi_* API 直连,绕过了 WifiManager,
            //   主程序永远收不到 Connecting/Connected 事件,网络状态完全失联。
            // 新逻辑: 全部交给 WifiManager::StartStation(direct_connect_hint),
            //   WifiStation 内部:
            //     1) STA_START -> StartDirectConnect() -> WifiManager 触发 NotifyEvent(Connecting, ssid) → 主程序看到"正在连接"
            //     2) GOT_IP   -> on_connected_(ssid)        -> WifiManager 触发 NotifyEvent(Connected, ssid) → 主程序确认网络就绪
            _ensure_disconnect_handler_registered();

            std::string ssid(reinterpret_cast<const char*>(m_sta_config.sta.ssid));
            std::string password(reinterpret_cast<const char*>(m_sta_config.sta.password));

            int hint_channel = 0;
            uint8_t hint_bssid[6] = {0};
            bool have_hint = false;

            if (m_has_recent_scan_results) {
                auto best_it = std::find_if(m_ap_records.begin(), m_ap_records.end(),
                                            [&ssid](const wifi_ap_record_t& ap) {
                    return strcmp(reinterpret_cast<const char*>(ap.ssid), ssid.c_str()) == 0;
                });
                if (best_it != m_ap_records.end()) {
                    hint_channel = best_it->primary;
                    memcpy(hint_bssid, best_it->bssid, sizeof(hint_bssid));
                    have_hint = true;
                    ESP_LOGI(BLUFI_TAG, "_do_wifi_connect: AP hint channel=%d RSSI=%d",
                             hint_channel, best_it->rssi);
                }
            }

            auto& wifi = WifiManager::GetInstance();
            if (!wifi.IsInitialized() && !wifi.Initialize()) {
                ESP_LOGE(BLUFI_TAG, "Failed to initialize WifiManager");
                break;
            }

            _unregister_scan_handler();
            _reset_scan_state();
            m_scan_should_save_ssid = false;

            m_sta_ssid_len = static_cast<int>(std::min(ssid.size(), sizeof(m_sta_ssid)));
            memcpy(m_sta_ssid, ssid.c_str(), m_sta_ssid_len);
            if (have_hint) {
                memcpy(m_sta_bssid, hint_bssid, sizeof(m_sta_bssid));
                memcpy(m_sta_config.sta.bssid, hint_bssid, sizeof(hint_bssid));
                m_sta_config.sta.bssid_set = true;
                m_sta_config.sta.channel = hint_channel;
                m_sta_conn_info.sta_bssid_set = true;
                memcpy(m_sta_conn_info.sta_bssid, hint_bssid, sizeof(hint_bssid));
            } else {
                memset(m_sta_bssid, 0, sizeof(m_sta_bssid));
                m_sta_config.sta.bssid_set = false;
                m_sta_config.sta.channel = 0;
                m_sta_conn_info.sta_bssid_set = false;
            }
            m_sta_connected = false;
            m_sta_got_ip = false;
            m_sta_is_connecting = true;
            m_sta_conn_info.sta_ssid = m_sta_ssid;
            m_sta_conn_info.sta_ssid_len = m_sta_ssid_len;

            m_waiting_for_conn_result = true;

            if (have_hint) {
                WifiApRecord hint = {
                    .ssid = ssid,
                    .password = password,
                    .channel = hint_channel,
                    .authmode = WIFI_AUTH_WPA2_PSK,
                    .bssid = {0}
                };
                memcpy(hint.bssid, hint_bssid, sizeof(hint.bssid));
                ESP_LOGI(BLUFI_TAG, "Starting station with direct connect hint");
                wifi.StartStation(hint);
            } else {
                ESP_LOGI(BLUFI_TAG, "Starting station (no cached hint)");
                wifi.StartStation();
            }

            // BLE 上立即回包 CONNECTING 让手机知道我们收到密码、开始连接
            if (m_ble_is_connected) {
                wifi_mode_t mode;
                esp_wifi_get_mode(&mode);
                ESP_LOGI(BLUFI_TAG, "Sending STA_CONNECTING once after recv password");
                esp_err_t err = esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONNECTING, 0, &m_sta_conn_info);
                ESP_LOGI(BLUFI_TAG, "STA_CONNECTING sent: %s", esp_err_to_name(err));
            }

            // 同步 MAC 给手机
            {
                uint8_t mac[6] = {0};
                esp_wifi_get_mac(WIFI_IF_STA, mac);
                char payload[96];
                int len = snprintf(payload, sizeof(payload),
                                   "{\"deviceMac\":\"%02x:%02x:%02x:%02x:%02x:%02x\"}",
                                   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
                ESP_LOGI(BLUFI_TAG, "Queue MAC after recv password: %s", payload);
                esp_err_t r = esp_blufi_send_custom_data((uint8_t*)payload, len);
                ESP_LOGI(BLUFI_TAG, "esp_blufi_send_custom_data returned: %d", r);
            }

            // fallback 守护: 万一 NotifyEvent(Connected) 因任何原因没触发,
            // 30s 后兜底检测一次,确保主程序一定能进入 activating 状态
            const char* ssid_for_task = strdup(ssid.c_str());
            xTaskCreate(
                [](void* ctx) {
                    auto* self = static_cast<Blufi*>(ctx);
                    auto& w = WifiManager::GetInstance();
                    constexpr int kConnectTimeoutMs = 30000;
                    constexpr TickType_t kDelayTick = pdMS_TO_TICKS(200);
                    int waited_ms = 0;

                    while (waited_ms < kConnectTimeoutMs) {
                        vTaskDelay(kDelayTick);
                        waited_ms += 200;
                        if (self->m_provisioned) {
                            ESP_LOGI(BLUFI_TAG, "fallback: provisioned, exit");
                            vTaskDelete(nullptr);
                            return;
                        }
                        if (w.IsConnected()) {
                            ESP_LOGI(BLUFI_TAG, "fallback: WiFi connected at %d ms", waited_ms);
                            self->m_sta_is_connecting = false;
                            self->m_sta_connected = true;
                            self->m_provisioned = true;
                            vTaskDelete(nullptr);
                            return;
                        }
                    }

                    ESP_LOGW(BLUFI_TAG, "fallback: WiFi connect timeout (%d ms)", waited_ms);
                    self->m_sta_is_connecting = false;
                    self->m_sta_got_ip = false;
                    if (self->m_ble_is_connected) {
                        wifi_mode_t fail_mode;
                        esp_wifi_get_mode(&fail_mode);
                        esp_blufi_send_wifi_conn_report(fail_mode, ESP_BLUFI_STA_CONN_FAIL, 0,
                                                        &self->m_sta_conn_info);
                    }
                    if (!self->m_deinited) {
                        self->deinit();
                    }
                    vTaskDelete(nullptr);
                },
                "blufi_wifi_conn", 4096, this, 5, nullptr);
            break;
        }
        case ESP_BLUFI_EVENT_GET_WIFI_LIST: {
            ESP_LOGI(BLUFI_TAG, "BLUFI get wifi list");
            if (m_scan_in_progress) {
                m_wifi_list_requested = true;
            } else {
                m_scan_should_save_ssid = true;
                m_wifi_list_requested = true;
                start_wifi_scan();
            }
            break;
        }
        default:
            ESP_LOGW(BLUFI_TAG, "Unhandled event: %d", event);
            break;
    }
}

void Blufi::_event_callback_trampoline(esp_blufi_cb_event_t event, esp_blufi_cb_param_t* param) {
    GetInstance()._handle_event(event, param);
}

void Blufi::_negotiate_data_handler_trampoline(uint8_t* data, int len, uint8_t** output_data,
                                               int* output_len, bool* need_free) {
    GetInstance()._dh_negotiate_data_handler(data, len, output_data, output_len, need_free);
}

int Blufi::_encrypt_func_trampoline(uint8_t iv8, uint8_t* crypt_data, int crypt_len) {
    return GetInstance()._aes_encrypt(iv8, crypt_data, crypt_len);
}

int Blufi::_decrypt_func_trampoline(uint8_t iv8, uint8_t* crypt_data, int crypt_len) {
    return GetInstance()._aes_decrypt(iv8, crypt_data, crypt_len);
}

uint16_t Blufi::_checksum_func_trampoline(uint8_t iv8, uint8_t* data, int len) {
    return _crc_checksum(iv8, data, len);
}
