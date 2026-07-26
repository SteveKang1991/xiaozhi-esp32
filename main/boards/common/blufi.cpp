#include "blufi.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_event.h"
#include "esp_gap_ble_api.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/task.h"
#include "wifi_manager.h"

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
      m_sta_is_connecting(false) {
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
    // For standard ESP targets with built-in BT controller
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
    // ESP-Hosted mode: BT controller is on co-processor (ESP32-C6)
    // Initialize via ESP-Hosted API
    ESP_LOGI(BLUFI_TAG, "_host_init: ESP-Hosted mode, opening HCI channel");
    hosted_hci_bluedroid_open();

    // Attach HCI driver operations to Bluedroid
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
    // ESP-Hosted mode: close HCI channel
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
    // ESP32-P4 with ESP-Hosted: BT controller is on the co-processor (ESP32-C6).
    // C6 slave firmware v2.12.x does NOT auto-init the BT controller at boot
    // (unlike older factory firmware), so we MUST explicitly drive its BT controller
    // here via RPC BEFORE opening the HCI channel and starting Bluedroid on the host.
    //
    // Reference: managed_components/espressif__esp_hosted/examples/host_bluedroid_host_only/main/main.c
    //
    // Required order on the host side (P4):
    //   1. esp_hosted_bt_controller_init()    -> RPC to C6: initialize its BT controller
    //   2. esp_hosted_bt_controller_enable()  -> RPC to C6: enable its BT controller (BLE mode) and register VHCI callback
    //   3. hosted_hci_bluedroid_open()        -> Open the HCI channel (done in _host_init)
    //   4. esp_bluedroid_attach_hci_driver()  -> Attach Bluedroid to the VHCI HCI driver
    //   5. esp_bluedroid_init() / _enable()   -> Start the host-side BT stack
    //
    // Without steps 1+2, Bluedroid_enable() on P4 will send its first HCI command
    // (HCI_RESET, opcode 0xC03) and time out after ~8s because the C6 controller
    // is not initialised and has no VHCI callback registered.
    ESP_LOGI(BLUFI_TAG, "_controller_init: ESP-Hosted mode, driving C6 BT controller via RPC");

    esp_err_t ret = esp_hosted_bt_controller_init();
    if (ret != ESP_OK) {
        ESP_LOGE(BLUFI_TAG, "_controller_init: esp_hosted_bt_controller_init failed: %s",
                 esp_err_to_name(ret));
        // Hard failure: there is no point continuing without a BT controller.
        return ret;
    }
    ESP_LOGI(BLUFI_TAG, "_controller_init: esp_hosted_bt_controller_init OK");

    ret = esp_hosted_bt_controller_enable();
    if (ret != ESP_OK) {
        ESP_LOGE(BLUFI_TAG, "_controller_init: esp_hosted_bt_controller_enable failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(BLUFI_TAG, "_controller_init: esp_hosted_bt_controller_enable OK");

    // NOTE: esp_hosted_init() and esp_hosted_connect_to_slave() are already
    // called automatically at startup by esp-hosted's internal startup hooks.
#else
    // Standard ESP targets with built-in BT controller
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
    // ESP32-P4 with ESP-Hosted: BT controller is on co-processor (factory firmware).
    // No deinit needed - C6 BT stays active independently.
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
        self->_on_got_ip();
    }
}

void Blufi::_on_got_ip() {
    if (!m_ble_is_connected) {
        ESP_LOGI(BLUFI_TAG, "Got IP but BLE disconnected, skipping status report");
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

    ESP_LOGI(BLUFI_TAG, "Sending WiFi connected status report via IP event handler");
    esp_err_t err = esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_SUCCESS, 0, &info);
    if (err != ESP_OK) {
        ESP_LOGW(BLUFI_TAG, "esp_blufi_send_wifi_conn_report returned: %s", esp_err_to_name(err));
    }
}

void Blufi::start_wifi_scan() {
    ESP_LOGI(BLUFI_TAG, "Starting dedicated WiFi scan");

    if (m_scan_in_progress) {
        ESP_LOGW(BLUFI_TAG, "Scan already in progress, skipping");
        return;
    }

    m_scan_in_progress = true;

    wifi_mode_t current_mode;
    esp_err_t err = esp_wifi_get_mode(&current_mode);

    if (current_mode == WIFI_MODE_AP || current_mode == WIFI_MODE_APSTA) {
        ESP_LOGI(BLUFI_TAG, "Switching WiFi mode from AP/APSTA to STA before scan");
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) {
            ESP_LOGE(BLUFI_TAG, "esp_wifi_set_mode(WIFI_MODE_STA) failed: %s", esp_err_to_name(err));
            m_scan_in_progress = false;
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
        return;
    }

    ESP_LOGI(BLUFI_TAG, "WiFi scan started successfully");
}

void Blufi::_send_wifi_list() {
    if (m_ap_records.empty()) {
        ESP_LOGW(BLUFI_TAG, "No AP records available to send");
        return;
    }

    ESP_LOGI(BLUFI_TAG, "Sending WiFi list with %d APs", m_ap_records.size());

    std::vector<esp_blufi_ap_record_t> blufi_ap_list;
    blufi_ap_list.resize(m_ap_records.size());
    for (size_t i = 0; i < m_ap_records.size(); ++i) {
        const auto& ap = m_ap_records[i];
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
        ESP_LOGI(BLUFI_TAG, "WiFi scan done");

        uint16_t ap_num = 0;
        esp_wifi_scan_get_ap_num(&ap_num);

        if (ap_num == 0) {
            ESP_LOGW(BLUFI_TAG, "No APs found");
            self->m_ap_records.clear();
            self->m_has_recent_scan_results = false;
        } else {
            if (static_cast<Blufi*>(arg)->m_scan_should_save_ssid == true) {
                self->m_ap_records.resize(ap_num);
                esp_wifi_scan_get_ap_records(&ap_num, self->m_ap_records.data());
                self->m_has_recent_scan_results = true;

                ESP_LOGI(BLUFI_TAG, "Found %d APs", ap_num);
                for (const auto& ap : self->m_ap_records) {
                    ESP_LOGI(BLUFI_TAG, "  SSID: %s, RSSI: %d, Authmode: %d", (char*)ap.ssid,
                             ap.rssi, ap.authmode);
                }
            }
        }
        self->m_scan_in_progress = false;

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
            ESP_LOGI(BLUFI_TAG, "BLUFI ble connect");
            m_ble_is_connected = true;
            esp_blufi_adv_stop();
            _security_init();
            if (m_ip_handler_instance == nullptr) {
                ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                    &_ip_event_handler, this, &m_ip_handler_instance));
                ESP_LOGI(BLUFI_TAG, "IP event handler registered");
            }
            break;
        case ESP_BLUFI_EVENT_BLE_DISCONNECT:
            ESP_LOGI(BLUFI_TAG, "BLUFI ble disconnect");
            m_ble_is_connected = false;
            _security_deinit();
            _unregister_scan_handler();
            _reset_scan_state();
            if (m_ip_handler_instance != nullptr) {
                esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, m_ip_handler_instance);
                m_ip_handler_instance = nullptr;
                ESP_LOGI(BLUFI_TAG, "IP event handler unregistered");
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
            switch (param->wifi_mode.op_mode) {
                case WIFI_MODE_STA:
                    wifi_manager.StartStation();
                    break;
                case WIFI_MODE_AP:
                    wifi_manager.StartConfigAp();
                    break;
                case WIFI_MODE_APSTA:
                    ESP_LOGW(BLUFI_TAG, "APSTA mode not supported, starting station only");
                    wifi_manager.StartStation();
                    break;
                default:
                    wifi_manager.StopStation();
                    wifi_manager.StopConfigAp();
                    break;
            }
            break;
        }
        case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP: {
            ESP_LOGI(BLUFI_TAG, "BLUFI request wifi connect to AP via esp-wifi-connect");
            std::string ssid(reinterpret_cast<const char*>(m_sta_config.sta.ssid));
            std::string password(reinterpret_cast<const char*>(m_sta_config.sta.password));

            if (m_has_recent_scan_results) {
                auto best_it = std::find_if(m_ap_records.begin(), m_ap_records.end(), [&ssid](const wifi_ap_record_t& ap) {
                    return strcmp(reinterpret_cast<const char*>(ap.ssid), ssid.c_str()) == 0;
                });
                if (best_it != m_ap_records.end()) {
                    m_sta_config.sta.channel = best_it->primary;
                    memcpy(m_sta_config.sta.bssid, best_it->bssid, sizeof(best_it->bssid));
                    m_sta_config.sta.bssid_set = true;
                    ESP_LOGI(BLUFI_TAG,
                             "Using cached AP hint for SSID %s: RSSI=%d channel=%d",
                             ssid.c_str(), best_it->rssi, best_it->primary);
                } else {
                    m_sta_config.sta.channel = 0;
                    m_sta_config.sta.bssid_set = false;
                    ESP_LOGW(BLUFI_TAG, "No cached AP hint found for SSID %s", ssid.c_str());
                }
            } else {
                m_sta_config.sta.channel = 0;
                m_sta_config.sta.bssid_set = false;
                ESP_LOGW(BLUFI_TAG, "No recent scan results available for SSID %s", ssid.c_str());
            }

            SsidManager::GetInstance().AddSsid(ssid, password);
            _unregister_scan_handler();
            _reset_scan_state();
            m_scan_should_save_ssid = false;

            m_sta_ssid_len = static_cast<int>(std::min(ssid.size(), sizeof(m_sta_ssid)));
            memcpy(m_sta_ssid, ssid.c_str(), m_sta_ssid_len);
            if (m_sta_config.sta.bssid_set) {
                memcpy(m_sta_bssid, m_sta_config.sta.bssid, sizeof(m_sta_bssid));
                m_sta_conn_info.sta_bssid_set = true;
                memcpy(m_sta_conn_info.sta_bssid, m_sta_config.sta.bssid, sizeof(m_sta_bssid));
            } else {
                memset(m_sta_bssid, 0, sizeof(m_sta_bssid));
                m_sta_conn_info.sta_bssid_set = false;
            }
            m_sta_connected = false;
            m_sta_got_ip = false;
            m_sta_is_connecting = true;
            m_sta_conn_info = {};
            m_sta_conn_info.sta_ssid = m_sta_ssid;
            m_sta_conn_info.sta_ssid_len = m_sta_ssid_len;

            auto& wifi_manager = WifiManager::GetInstance();

            if (wifi_manager.IsInitialized()) {
                if (wifi_manager.IsConfigMode()) {
                    wifi_manager.StopConfigAp();
                }
                wifi_manager.StopStation();
            }

            if (!wifi_manager.IsInitialized() && !wifi_manager.Initialize()) {
                ESP_LOGE(BLUFI_TAG, "Failed to initialize WifiManager");
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(500));

            if (m_sta_config.sta.bssid_set) {
                WifiApRecord direct_connect_hint = {
                    .ssid = ssid,
                    .password = password,
                    .channel = m_sta_config.sta.channel,
                    .authmode = WIFI_AUTH_WPA2_PSK,
                    .bssid = {0}
                };
                memcpy(direct_connect_hint.bssid, m_sta_config.sta.bssid, sizeof(direct_connect_hint.bssid));
                wifi_manager.StartStation(direct_connect_hint);
            } else {
                wifi_manager.StartStation();
            }

            xTaskCreate(
                [](void* ctx) {
                    auto* self = static_cast<Blufi*>(ctx);
                    auto& wifi = WifiManager::GetInstance();
                    constexpr int kConnectTimeoutMs = 10000;
                    constexpr TickType_t kDelayTick = pdMS_TO_TICKS(200);
                    int waited_ms = 0;

                    while (waited_ms < kConnectTimeoutMs && !wifi.IsConnected()) {
                        vTaskDelay(kDelayTick);
                        waited_ms += 200;
                    }

                    wifi_mode_t mode = GetWifiModeWithFallback(wifi);
                    const int softap_conn_num = _get_softap_conn_num();

                    if (wifi.IsConnected()) {
                        self->m_sta_is_connecting = false;
                        self->m_sta_connected = true;
                        self->m_provisioned = true;

                        auto current_ssid = wifi.GetSsid();
                        if (!current_ssid.empty()) {
                            self->m_sta_ssid_len = static_cast<int>(
                                std::min(current_ssid.size(), sizeof(self->m_sta_ssid)));
                            memcpy(self->m_sta_ssid, current_ssid.c_str(), self->m_sta_ssid_len);
                        }

                        wifi_ap_record_t ap_info{};
                        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                            memcpy(self->m_sta_bssid, ap_info.bssid, sizeof(self->m_sta_bssid));
                        }

                        ESP_LOGI(BLUFI_TAG, "WiFi connected, waiting for IP/status handling");
                    } else {
                        self->m_sta_is_connecting = false;
                        self->m_sta_connected = false;
                        self->m_sta_got_ip = false;

                        esp_blufi_extra_info_t info = {};
                        info.sta_ssid = self->m_sta_ssid;
                        info.sta_ssid_len = self->m_sta_ssid_len;
                        esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_FAIL,
                                                        softap_conn_num, &info);
                        ESP_LOGE(BLUFI_TAG, "Failed to connect to WiFi via esp-wifi-connect");
                    }
                    vTaskDelete(nullptr);
                },
                "blufi_wifi_conn", 4096, this, 5, nullptr);
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

            if (wifi.IsInitialized() && wifi.IsConnected()) {
                m_sta_connected = true;
                m_sta_got_ip = true;

                auto current_ssid = wifi.GetSsid();
                if (!current_ssid.empty()) {
                    m_sta_ssid_len =
                        static_cast<int>(std::min(current_ssid.size(), sizeof(m_sta_ssid)));
                    memcpy(m_sta_ssid, current_ssid.c_str(), m_sta_ssid_len);
                }

                esp_blufi_extra_info_t info;
                memset(&info, 0, sizeof(esp_blufi_extra_info_t));
                memcpy(info.sta_bssid, m_sta_bssid, 6);
                info.sta_ssid = m_sta_ssid;
                info.sta_ssid_len = m_sta_ssid_len;
                esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_SUCCESS, softap_conn_num,
                                                &info);
            } else if (m_sta_is_connecting) {
                esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONNECTING, softap_conn_num,
                                                &m_sta_conn_info);
            } else {
                esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_FAIL, softap_conn_num,
                                                &m_sta_conn_info);
            }
            ESP_LOGI(BLUFI_TAG, "BLUFI get wifi status");
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
        case ESP_BLUFI_EVENT_RECV_STA_PASSWD:
            strncpy((char*)m_sta_config.sta.password, (char*)param->sta_passwd.passwd,
                    param->sta_passwd.passwd_len);
            m_sta_config.sta.password[param->sta_passwd.passwd_len] = '\0';
            ESP_LOGI(BLUFI_TAG, "Recv STA PASSWORD : %s", m_sta_config.sta.password);
            break;
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
