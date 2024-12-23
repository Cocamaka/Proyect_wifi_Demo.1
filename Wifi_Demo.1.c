/*Gestión de conexión WiFi: conéctese al punto de acceso designado a través de ESP32 e intente volver a conectarse automáticamente cuando se desconecte.
Configuración de la URL del servidor: la URL del servidor se almacena y lee a través de NVS, y la configuración se puede cambiar dinámicamente.
Modo de bajo consumo: permite un sueño ligero y un ajuste dinámico de frecuencia para reducir el consumo de energía.
Diseño modular: Separe las funciones principales en módulos independientes, como la inicialización de WiFi, la administración de NVS y la administración de energía, para facilitar la expansión y el mantenimiento.*/

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
#include "flash.h"
#include "esp_pm.h"

static const char *TAG = "RPI-I";
static EventGroupHandle_t wifi_event_group;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_SSID "YourHotspotSSID"
#define WIFI_PASS "YourHotspotPassword"
#define DEFAULT_SERVER_URL

char server_url[128] = DEFAULT_SERVER_URL;


/*Función:
Conéctese a WiFi con SSID y contraseña fijos.
Utilice grupos de eventos y controladores de eventos para administrar el estado de la conexión.
Intenta volver a conectarse automáticamente cuando se desconecta el WiFi para garantizar una conexión de red estable.

proceso:
Utilice esp_wifi_init para inicializar la configuración de WiFi.
Configure el SSID y la contraseña de WiFi.
Inicie WiFi y registre un controlador de eventos para monitorear el estado de la conexión.
Si está desconectado, intente conectarse nuevamente.
*/
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


/*Función:
Cargue la URL del servidor almacenado desde NVS (almacenamiento no volátil).
Si no se encuentra ninguna URL válida, se utiliza el valor predeterminado.
Proporciona la función de actualizar dinámicamente las URL y guardar nuevas URL en NVS.

proceso:
Inicialice NVS.
Lea el valor de la clave server_url. Si no se encuentra o la lectura falla, se utiliza la URL predeterminada.
Puede guardar dinámicamente la nueva URL del servidor a través de save_server_url_to_nvs para su próximo uso.
*/
void load_server_url_from_nvs(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        size_t required_size;
        err = nvs_get_str(nvs_handle, "server_url", NULL, &required_size);
        if (err == ESP_OK && required_size <= sizeof(server_url)) {
            nvs_get_str(nvs_handle, "server_url", server_url, &required_size);
            ESP_LOGI(TAG, "Loaded server URL from NVS: %s", server_url);
        } else {
            ESP_LOGW(TAG, "Failed to load server URL from NVS, using default: %s", server_url);
        }
        nvs_close(nvs_handle);
    } else {
        ESP_LOGW(TAG, "NVS open failed, using default server URL: %s", server_url);
    }
}



void save_server_url_to_nvs(const char *new_url) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, "server_url", new_url);
        if (err == ESP_OK) {
            nvs_commit(nvs_handle);
            ESP_LOGI(TAG, "Server URL saved to NVS: %s", new_url);
        } else {
            ESP_LOGE(TAG, "Failed to save server URL to NVS");
        }
        nvs_close(nvs_handle);
    } else {
        ESP_LOGE(TAG, "Failed to open NVS for writing");
    }
}

/*Función:
Habilite el modo de suspensión ligero y ajuste dinámicamente la frecuencia de la CPU para reducir el consumo de energía.
Establezca la frecuencia máxima en 80 MHz y la frecuencia mínima en 10 MHz.

proceso:
Configure los parámetros de administración de energía.
Utilice esp_pm_configure para aplicar la configuración.
El sistema entra automáticamente en modo de suspensión ligera cuando está inactivo para reducir el consumo de energía.
*/
void configure_power_management(void) {
    esp_pm_config_esp32_t pm_config = {
        .max_freq_mhz = 80,
        .min_freq_mhz = 10,
        .light_sleep_enable = true,
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm_config));
    ESP_LOGI(TAG, "Power management configured: light sleep enabled");
}

/*Función:
Inicialice el almacenamiento NVS y WiFi.
Cargue la URL del servidor desde NVS.
Configurar la administración de energía.
Espere a que se complete la conexión WiFi y registre el estado de la conexión.

proceso:
Llame a flash_init para garantizar que la inicialización de NVS sea exitosa.
Llame a load_server_url_from_nvs para cargar la URL del servidor.
Llame a configure_power_management para habilitar el modo de bajo consumo.
Llame a wifi_init_sta para conectarse a WiFi.
Utilice el grupo de eventos xEventGroupWaitBits para esperar a que se complete la conexión WiFi.
*/
void app_main(void) {
    ESP_LOGI(TAG, "Initializing RPI-I Module...");

    flash_init();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_event_group = xEventGroupCreate();

    load_server_url_from_nvs();
    configure_power_management();
    wifi_init_sta();

    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, true, true, portMAX_DELAY);

    ESP_LOGI(TAG, "Connected to WiFi. Using server URL: %s", server_url);
}
