#include <assert.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_pm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "main.h"

static bool g_wifi_connected = false;

static void pipecat_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data) {
  static int s_retry_num = 0;
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    if (s_retry_num < 10) { // Increased retry attempts
      esp_wifi_connect();
      s_retry_num++;
      ESP_LOGI(LOG_TAG, "retry to connect to the AP (%d/10)", s_retry_num);
    } else {
      ESP_LOGE(LOG_TAG, "Failed to connect after 10 attempts, restarting...");
      esp_restart();
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(LOG_TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
    s_retry_num = 0; // Reset retry counter on successful connection
    g_wifi_connected = true;
  }
}

void pipecat_init_wifi() {
  // Disable power management for maximum WiFi performance
  esp_pm_config_t pm_config = {
      .max_freq_mhz = 240,
      .min_freq_mhz = 240,  // Keep CPU at max frequency
      .light_sleep_enable = false
  };
  ESP_ERROR_CHECK(esp_pm_configure(&pm_config));

  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             &pipecat_event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             &pipecat_event_handler, NULL));

  ESP_ERROR_CHECK(esp_netif_init());
  esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
  assert(sta_netif);

  // Optimize network interface for low latency
  esp_netif_set_hostname(sta_netif, "pipecat-device");
  
  // Use default WiFi config and modify what we can
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  
  // Note: rx_buf_num and tx_buf_num are set via menuconfig in newer ESP-IDF
  // These optimizations are handled by the default config
  
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  
  // Set WiFi to maximum performance mode
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE)); // Disable power saving
  
  // Set country for optimal channel selection
  wifi_country_t country = {
      .cc = "US",
      .schan = 1,
      .nchan = 11,
      .max_tx_power = 84, // Maximum legal power
      .policy = WIFI_COUNTRY_POLICY_AUTO
  };
  ESP_ERROR_CHECK(esp_wifi_set_country(&country));

  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(LOG_TAG, "Connecting to WiFi SSID: %s", WIFI_SSID);
  wifi_config_t wifi_config;
  memset(&wifi_config, 0, sizeof(wifi_config));
  strncpy((char *)wifi_config.sta.ssid, (char *)WIFI_SSID,
          sizeof(wifi_config.sta.ssid));
  strncpy((char *)wifi_config.sta.password, (char *)WIFI_PASSWORD,
          sizeof(wifi_config.sta.password));

  // Optimize connection parameters for stability
  wifi_config.sta.scan_method = WIFI_FAST_SCAN;
  wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  wifi_config.sta.threshold.rssi = -70; // Only connect to strong signals
  wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  
  // Set bandwidth to 40MHz for better throughput (if supported by router)
  ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT40));
  
  // Set WiFi protocol to 802.11n for best performance
  ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));

  ESP_ERROR_CHECK(esp_wifi_connect());

  ESP_LOGI(LOG_TAG, "Waiting for WiFi connection...");
  // Reduced polling interval for faster detection
  while (!g_wifi_connected) {
    vTaskDelay(pdMS_TO_TICKS(100)); // Check every 100ms instead of 200ms
  }
  
  // Print connection details for debugging
  wifi_ap_record_t ap_info;
  if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
    ESP_LOGI(LOG_TAG, "Connected to AP: %s, RSSI: %d, Channel: %d", 
             ap_info.ssid, ap_info.rssi, ap_info.primary);
  }
  
  ESP_LOGI(LOG_TAG, "WiFi optimization complete - ready for audio streaming");
}