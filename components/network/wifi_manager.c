/*
 * Wi-Fi Station manager with SmartConfig (EspTouch) provisioning.
 *
 * Flow:
 *   1. Attempt to load SSID/password from NVS.
 *   2. If found, connect in STA mode.
 *   3. If not found or connect fails, start SmartConfig.
 *   4. On SmartConfig success, save credentials to NVS.
 *   5. Block until IP address obtained.
 */

#include "network.h"
#include "esp_wifi.h"
#include "esp_smartconfig.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "wifi_mgr";

/* NVS keys */
#define NVS_NAMESPACE   "wifi_cfg"
#define NVS_KEY_SSID    "ssid"
#define NVS_KEY_PASS    "password"

/* Event group bits */
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define SC_DONE_BIT         BIT2

/* Max STA connection retries before falling back to SmartConfig */
#define MAX_STA_RETRIES     5

static EventGroupHandle_t s_wifi_event_group = NULL;
static int  s_retry_count  = 0;
static bool s_has_stored_creds = false;
static bool s_connected = false;
static esp_ip4_addr_t s_ip_addr;

/* ---- NVS helpers ---- */

static esp_err_t nvs_load_wifi_credentials(char *ssid, size_t ssid_len,
                                            char *password, size_t pass_len)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) return ret;

    ret = nvs_get_str(handle, NVS_KEY_SSID, ssid, &ssid_len);
    if (ret != ESP_OK) { nvs_close(handle); return ret; }

    ret = nvs_get_str(handle, NVS_KEY_PASS, password, &pass_len);
    nvs_close(handle);
    return ret;
}

static esp_err_t nvs_save_wifi_credentials(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) return ret;

    ret = nvs_set_str(handle, NVS_KEY_SSID, ssid);
    if (ret != ESP_OK) { nvs_close(handle); return ret; }

    ret = nvs_set_str(handle, NVS_KEY_PASS, password);
    if (ret != ESP_OK) { nvs_close(handle); return ret; }

    ret = nvs_commit(handle);
    nvs_close(handle);
    return ret;
}

/* ---- SmartConfig start ---- */

static void start_smartconfig(void)
{
    ESP_LOGI(TAG, "Starting SmartConfig (EspTouch)...");
    esp_smartconfig_set_type(SC_TYPE_ESPTOUCH);
    smartconfig_start_config_t sc_cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_smartconfig_start(&sc_cfg));
}

/* ---- Event handlers ---- */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            if (s_has_stored_creds) {
                ESP_LOGI(TAG, "STA started, connecting with stored credentials...");
                esp_wifi_connect();
            } else {
                ESP_LOGI(TAG, "STA started, no stored credentials");
                start_smartconfig();
            }
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            s_connected = false;
            s_retry_count++;
            if (s_retry_count < MAX_STA_RETRIES) {
                ESP_LOGW(TAG, "Disconnected, retrying (%d/%d)...",
                         s_retry_count, MAX_STA_RETRIES);
                esp_wifi_connect();
            } else {
                ESP_LOGW(TAG, "Max retries reached, starting SmartConfig");
                s_retry_count = 0;
                s_has_stored_creds = false;
                start_smartconfig();
            }
            break;

        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_ip_addr = event->ip_info.ip;
        s_connected = true;
        s_retry_count = 0;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

    } else if (event_base == SC_EVENT) {
        switch (event_id) {
        case SC_EVENT_SCAN_DONE:
            ESP_LOGI(TAG, "SmartConfig: scan done");
            break;

        case SC_EVENT_FOUND_CHANNEL:
            ESP_LOGI(TAG, "SmartConfig: found channel");
            break;

        case SC_EVENT_GOT_SSID_PSWD: {
            smartconfig_event_got_ssid_pswd_t *evt =
                (smartconfig_event_got_ssid_pswd_t *)event_data;

            char ssid[33] = {0};
            char password[65] = {0};
            memcpy(ssid, evt->ssid, sizeof(evt->ssid));
            memcpy(password, evt->password, sizeof(evt->password));

            ESP_LOGI(TAG, "SmartConfig: SSID=%s", ssid);

            /* Save to NVS */
            esp_err_t ret = nvs_save_wifi_credentials(ssid, password);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to save credentials to NVS: %s",
                         esp_err_to_name(ret));
            }

            /* Configure and connect */
            wifi_config_t wifi_cfg = {0};
            memcpy(wifi_cfg.sta.ssid, evt->ssid, sizeof(wifi_cfg.sta.ssid));
            memcpy(wifi_cfg.sta.password, evt->password, sizeof(wifi_cfg.sta.password));

            ESP_ERROR_CHECK(esp_wifi_disconnect());
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
            esp_wifi_connect();
            break;
        }

        case SC_EVENT_SEND_ACK_DONE:
            ESP_LOGI(TAG, "SmartConfig: ACK sent, stopping SmartConfig");
            esp_smartconfig_stop();
            xEventGroupSetBits(s_wifi_event_group, SC_DONE_BIT);
            break;

        default:
            break;
        }
    }
}

/* ---- Public API ---- */

esp_err_t wifi_manager_init(void)
{
    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) {
        return ESP_ERR_NO_MEM;
    }

    /* Initialize TCP/IP stack and default event loop */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    /* Wi-Fi init with default config */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Register event handlers */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID,
                                                &wifi_event_handler, NULL));

    /* Try loading stored credentials */
    char ssid[33] = {0};
    char password[65] = {0};
    if (nvs_load_wifi_credentials(ssid, sizeof(ssid),
                                   password, sizeof(password)) == ESP_OK
        && strlen(ssid) > 0) {
        ESP_LOGI(TAG, "Loaded stored SSID: %s", ssid);
        s_has_stored_creds = true;

        wifi_config_t wifi_cfg = {0};
        strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
        strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    } else {
        ESP_LOGI(TAG, "No stored credentials, will use SmartConfig");
        s_has_stored_creds = false;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    /* Block until connected */
    ESP_LOGI(TAG, "Waiting for Wi-Fi connection...");
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    return s_connected;
}

esp_err_t wifi_manager_get_ip_str(char *buf, size_t buf_len)
{
    if (!s_connected || !buf || buf_len < 16) {
        return ESP_ERR_INVALID_STATE;
    }
    snprintf(buf, buf_len, IPSTR, IP2STR(&s_ip_addr));
    return ESP_OK;
}
