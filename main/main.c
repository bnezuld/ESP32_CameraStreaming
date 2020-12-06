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

#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_timer.h"

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";


//WROVER-KIT PIN Map
#define CAM_PIN_PWDN    32 //power down is not used
#define CAM_PIN_RESET   -1 //software reset will be performed
#define CAM_PIN_XCLK    0 //21
#define CAM_PIN_SIOD    26 //26
#define CAM_PIN_SIOC    27 //27

#define CAM_PIN_D7      35 //35
#define CAM_PIN_D6      34 //34
#define CAM_PIN_D5      39 //39
#define CAM_PIN_D4      36 //36
#define CAM_PIN_D3      21 //19
#define CAM_PIN_D2      19 //18
#define CAM_PIN_D1      18 //5
#define CAM_PIN_D0       5 //4
#define CAM_PIN_VSYNC   25 //25
#define CAM_PIN_HREF    23 //23
#define CAM_PIN_PCLK    22 //22

static camera_config_t camera_config = {
    .pin_pwdn  = CAM_PIN_PWDN,
    .pin_reset = CAM_PIN_RESET,
    .pin_xclk = CAM_PIN_XCLK,
    .pin_sscb_sda = CAM_PIN_SIOD,
    .pin_sscb_scl = CAM_PIN_SIOC,

    .pin_d7 = CAM_PIN_D7,
    .pin_d6 = CAM_PIN_D6,
    .pin_d5 = CAM_PIN_D5,
    .pin_d4 = CAM_PIN_D4,
    .pin_d3 = CAM_PIN_D3,
    .pin_d2 = CAM_PIN_D2,
    .pin_d1 = CAM_PIN_D1,
    .pin_d0 = CAM_PIN_D0,
    .pin_vsync = CAM_PIN_VSYNC,
    .pin_href = CAM_PIN_HREF,
    .pin_pclk = CAM_PIN_PCLK,

    //XCLK 20MHz or 10MHz for OV2640 double FPS (Experimental)
    .xclk_freq_hz = 20000000,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,

    .pixel_format = PIXFORMAT_JPEG,//YUV422,GRAYSCALE,RGB565,JPEG
    .frame_size = FRAMESIZE_UXGA,//QQVGA-QXGA Do not use sizes above QVGA when not JPEG

    .jpeg_quality = 12, //0-63 lower number means higher quality
    .fb_count = 1 //if more than one, i2s runs in continuous mode. Use only with JPEG
};

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
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
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

esp_err_t init_camera(){
    //power up the camera if PWDN pin is defined
    if(CAM_PIN_PWDN != -1){
        //pinMode(CAM_PIN_PWDN, OUTPUT);
        //digitalWrite(CAM_PIN_PWDN, LOW);
    }

    //initialize the camera
    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera Init Failed");
        return err;
    }

    return ESP_OK;
}

httpd_handle_t stream_httpd = NULL;

static esp_err_t stream_handler(httpd_req_t *req){
  camera_fb_t * fb = NULL;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t * _jpg_buf = NULL;
  char * part_buf[64];

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if(res != ESP_OK){
    return res;
  }

  while(true){
    fb = esp_camera_fb_get();
    if (!fb) {
      ESP_LOGE(TAG, "Camera Capture Failed");
      res = ESP_FAIL;
    } else {
      if(fb->width > 400){
        if(fb->format != PIXFORMAT_JPEG){
          bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
          esp_camera_fb_return(fb);
          fb = NULL;
          if(!jpeg_converted){
            ESP_LOGE(TAG, "JPEG compression failed");
            res = ESP_FAIL;
          }
        } else {
          _jpg_buf_len = fb->len;
          _jpg_buf = fb->buf;
        }
      }
    }
    if(res == ESP_OK){
      size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);
      res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
    }
    if(res == ESP_OK){
      res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
    }
    if(res == ESP_OK){
      res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
    }
    if(fb){
      esp_camera_fb_return(fb);
      fb = NULL;
      _jpg_buf = NULL;
    } else if(_jpg_buf){
      free(_jpg_buf);
      _jpg_buf = NULL;
    }
    if(res != ESP_OK){
      break;
    }
    //Serial.printf("MJPG: %uB\n",(uint32_t)(_jpg_buf_len));
  }
  return res;
}

void startCameraServer(){
  httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
  http_config.server_port = 80;

  httpd_uri_t index_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = stream_handler,
    .user_ctx  = NULL
  };
  
  //Serial.printf("Starting web server on port: '%d'\n", config.server_port);
  if (httpd_start(&stream_httpd, &http_config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &index_uri);
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

    ESP_ERROR_CHECK(init_camera());
    startCameraServer();
}