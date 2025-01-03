/*Reciba una nueva URL de OTA mediante MQTT con el asunto ota_update_url.
Tras recibir la nueva URL de OTA, guárdela en NVS y actualice la variable ota_update_url.
*/

#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_https_ota.h>
#include <esp_sleep.h>
#include "flash.h"
#include "esp_pm.h"
#include "mqtt_client.h"



static const char *TAG = "RPI-I";
static EventGroupHandle_t wifi_event_group;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_SSID "YourHotspotSSID"
#define WIFI_PASS "YourHotspotPassword"
#define DEFAULT_SERVER_URL "。。。。。。"
#define DEFAULT_OTA_UPDATE_URL "。。。。。。"
/*DEFAULT_SERVER_URL: URL por defecto del servidor Thingsboard para la comunicación IoT.
DEFAULT_OTA_UPDATE_URL: dirección predeterminada de descarga de firmware de actualización OTA para actualizaciones de firmware.
*/

char server_url[128] = DEFAULT_SERVER_URL;
char ota_update_url[256] = DEFAULT_OTA_UPDATE_URL;

void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Disconnected from WiFi. Attempting to reconnect...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void) {
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi Station initialized and connecting to hotspot");
}

void load_urls_from_nvs(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        size_t required_size;

        // Load server URL
        err = nvs_get_str(nvs_handle, "server_url", NULL, &required_size);
        if (err == ESP_OK && required_size <= sizeof(server_url)) {
            nvs_get_str(nvs_handle, "server_url", server_url, &required_size);
            ESP_LOGI(TAG, "Loaded server URL from NVS: %s", server_url);
        } else {
            ESP_LOGW(TAG, "Failed to load server URL, using default: %s", server_url);
        }

        // Load OTA URL
        err = nvs_get_str(nvs_handle, "ota_update_url", NULL, &required_size);
        if (err == ESP_OK && required_size <= sizeof(ota_update_url)) {
            nvs_get_str(nvs_handle, "ota_update_url", ota_update_url, &required_size);
            ESP_LOGI(TAG, "Loaded OTA Update URL from NVS: %s", ota_update_url);
        } else {
            ESP_LOGW(TAG, "Failed to load OTA Update URL, using default: %s", ota_update_url);
        }

        nvs_close(nvs_handle);
    } else {
        ESP_LOGW(TAG, "NVS open failed, using default URLs");
    }
}

void save_ota_url_to_nvs(const char *new_url) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, "ota_update_url", new_url);
        if (err == ESP_OK) {
            nvs_commit(nvs_handle);
            ESP_LOGI(TAG, "OTA Update URL saved to NVS: %s", new_url);
        } else {
            ESP_LOGE(TAG, "Failed to save OTA Update URL to NVS");
        }
        nvs_close(nvs_handle);
    } else {
        ESP_LOGE(TAG, "Failed to open NVS for writing");
    }
}

void configure_power_management(void) {
    esp_pm_config_esp32_t pm_config = {
        .max_freq_mhz = 80,
        .min_freq_mhz = 10,
        .light_sleep_enable = true,
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm_config));
    ESP_LOGI(TAG, "Power management configured: light sleep enabled");
}

void perform_ota_update(void) {
    ESP_LOGI(TAG, "Starting OTA update");

    esp_http_client_config_t ota_client_config = {
        .url = ota_update_url,
        .cert_pem = NULL, // Add certificate for secure connection if needed
    };

    esp_err_t ret = esp_https_ota(&ota_client_config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA update successful. Restarting...");
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA update failed");
    }
}

void mqtt_event_handler_cb(esp_mqtt_event_handle_t event) {
    switch (event->event_id) {
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT_EVENT_DATA, topic: %.*s, data: %.*s",
                     event->topic_len, event->topic,
                     event->data_len, event->data);

            if (strncmp(event->topic, "ota_update_url", event->topic_len) == 0) {
                strncpy(ota_update_url, event->data, event->data_len);
                ota_update_url[event->data_len] = '\0';
                save_ota_url_to_nvs(ota_update_url);
                ESP_LOGI(TAG, "Updated OTA URL: %s", ota_update_url);
            }
            break;
        default:
            break;
    }
}

void setup_mqtt(void) {
    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = server_url,
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler_cb, client);
    esp_mqtt_client_start(client);
}

void app_main(void) {
    ESP_LOGI(TAG, "Initializing RPI-I Module...");

    flash_init();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_event_group = xEventGroupCreate();

    load_urls_from_nvs();
    configure_power_management();
    wifi_init_sta();

    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, true, true, portMAX_DELAY);

    setup_mqtt();

    ESP_LOGI(TAG, "Connected to WiFi. Checking for OTA update...");
    perform_ota_update();

    ESP_LOGI(TAG, "Entering deep sleep...");
    esp_deep_sleep(10 * 60 * 1000000); // 10 minutes deep sleep
}
