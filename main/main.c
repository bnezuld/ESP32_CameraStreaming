/* Hello World Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_spi_flash.h"
#include "esp_wifi.h"
#include "esp_wpa2.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_smartconfig.h"

#include <string.h>
#include <stdlib.h>

#include "nvs.h"
#include "driver/gpio.h"


#define STORAGE_NAMESPACE "storage"

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_STARTED_BIT   BIT2
#define WIFI_SMARTCONFIG_DONE_BIT BIT3
static const char *TAG = "camera_streaming";


static void smartconfig_example_task(void * parm);

esp_err_t SaveLoginInfo(uint8_t ssid[33], uint8_t password[65])
{
    nvs_handle_t my_handle;
    esp_err_t err;

    // Open
    err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return err;

    size_t required_size = 0;

    // Write value including previously saved blob if available
    required_size = sizeof(uint8_t) * 33;
    err = nvs_set_blob(my_handle, "previous_ssid", ssid, required_size);
    if (err != ESP_OK) return err;

    required_size = sizeof(uint8_t) * 65;
    err = nvs_set_blob(my_handle, "previous_pwd", password, required_size);
    if (err != ESP_OK) return err;

    // Commit
    err = nvs_commit(my_handle);
    if (err != ESP_OK) return err;

    // Close
    nvs_close(my_handle);
    return ESP_OK;
}


esp_err_t LoginPreviousWifiInfo()
{
    nvs_handle_t my_handle;
    esp_err_t err;

    wifi_config_t wifi_config;
    bzero(&wifi_config, sizeof(wifi_config_t));

   // Open
    err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return err;

    // Read the size of memory space required for blob
    uint8_t ssid[33];
    size_t required_size = 33;
    err = nvs_get_blob(my_handle, "previous_ssid", ssid, &required_size);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
    ESP_LOGI(TAG, "SSID:%s", ssid);
    
    memcpy(wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));

    // Read the size of memory space required for blob
    uint8_t password[65];
    required_size = 65;
    err = nvs_get_blob(my_handle, "previous_pwd", password, &required_size);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;

    memcpy(wifi_config.sta.password, password, sizeof(wifi_config.sta.password));


    // Close
    nvs_close(my_handle);
    //return ESP_OK;
    ESP_LOGI(TAG, "PASSWORD:%s", password);

    //connect to wifi
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_STARTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    if(bits)
    {
        ESP_LOGI(TAG, "wifi started");
        
        err = esp_wifi_disconnect();
        if (err != ESP_OK) return err;
        err = esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);
        if (err != ESP_OK) return err;
        err = esp_wifi_connect();
        if (err != ESP_OK) return err;
        ESP_LOGI(TAG, "attepting connection");
        return ESP_OK;
    }
    //bits was false so maybe should return error
    return ESP_OK;
}

static void event_handler(void* arg, esp_event_base_t event_base, 
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        //give semaphore wifiStart
        xEventGroupSetBits(s_wifi_event_group, WIFI_STARTED_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        xEventGroupClearBits(s_wifi_event_group, WIFI_FAIL_BIT);
        ESP_LOGI(TAG, "wifi connected");
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_SCAN_DONE) {
        ESP_LOGI(TAG, "Scan done");
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_FOUND_CHANNEL) {
        ESP_LOGI(TAG, "Found channel");
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_GOT_SSID_PSWD) {
        ESP_LOGI(TAG, "Got SSID and password");

        smartconfig_event_got_ssid_pswd_t *evt = (smartconfig_event_got_ssid_pswd_t *)event_data;
        wifi_config_t wifi_config;
        uint8_t ssid[33] = { 0 };
        uint8_t password[65] = { 0 };

        bzero(&wifi_config, sizeof(wifi_config_t));
        memcpy(wifi_config.sta.ssid, evt->ssid, sizeof(wifi_config.sta.ssid));
        memcpy(wifi_config.sta.password, evt->password, sizeof(wifi_config.sta.password));
        wifi_config.sta.bssid_set = evt->bssid_set;
        if (wifi_config.sta.bssid_set == true) {
            memcpy(wifi_config.sta.bssid, evt->bssid, sizeof(wifi_config.sta.bssid));
        }

        memcpy(ssid, evt->ssid, sizeof(evt->ssid));
        memcpy(password, evt->password, sizeof(evt->password));
        ESP_LOGI(TAG, "SSID:%s", ssid);
        ESP_LOGI(TAG, "PASSWORD:%s", password);

        esp_err_t err = SaveLoginInfo(ssid, password);
        if (err != ESP_OK) ESP_LOGI(TAG, "save failed:%i", err);;

        ESP_ERROR_CHECK( esp_wifi_disconnect() );
        ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
        ESP_ERROR_CHECK( esp_wifi_connect() );
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_SEND_ACK_DONE) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_SMARTCONFIG_DONE_BIT);
    }
}

static void initialise_wifi(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );

    ESP_ERROR_CHECK( esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL) );
    ESP_ERROR_CHECK( esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL) );
    ESP_ERROR_CHECK( esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL) );

    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_start() );
}

static void smartconfig_example_task(void * parm)
{
    EventBits_t uxBits;
    ESP_ERROR_CHECK( esp_smartconfig_set_type(SC_TYPE_ESPTOUCH) );
    smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_smartconfig_start(&cfg) );
    while (1) {
        uxBits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_SMARTCONFIG_DONE_BIT, true, false, portMAX_DELAY); 
        if(uxBits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "WiFi Connected to ap");
        }
        if(uxBits & WIFI_SMARTCONFIG_DONE_BIT) {
            ESP_LOGI(TAG, "smartconfig over");
            esp_smartconfig_stop();
            vTaskDelete(NULL);
        }
    }
}

void app_main(void)
{
    s_wifi_event_group = xEventGroupCreate();
    
    //initialize nvs for saving/loading flash data
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK( err );
    
    initialise_wifi();

    err = LoginPreviousWifiInfo();
    if (err != ESP_OK) 
    {
        ESP_LOGI(TAG, "login error:%i", err);
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    }

    ESP_LOGI(TAG, "waiting for connection");
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_STARTED_BIT , pdFALSE, pdTRUE, portMAX_DELAY);
    if(bits & WIFI_STARTED_BIT)
    {
        ESP_LOGI(TAG, "wifi started");
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_FAIL_BIT | WIFI_CONNECTED_BIT , pdFALSE, pdFALSE, portMAX_DELAY);
        if(bits & WIFI_FAIL_BIT)
        {
            ESP_LOGI(TAG, "login fail");
            xTaskCreate(smartconfig_example_task, "smartconfig_example_task", 4096, NULL, 3, NULL);
        }else if(bits & WIFI_CONNECTED_BIT)
        {
            ESP_LOGI(TAG, "login succesfull");
        }
    }
}
