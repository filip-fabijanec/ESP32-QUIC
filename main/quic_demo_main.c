/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"

extern SemaphoreHandle_t g_quic_mutex;

#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_quictls.h>

#include <openssl/ssl.h>
#include <openssl/rand.h>
#include <openssl/err.h>

#include "ngtcp2_sample.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_task_wdt.h"

#include "protocol_examples_common.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"

#include "core_mqtt.h"
#include "core_mqtt_state.h"
#include "mqtt_quic_transport.h"

#include "hardware_control.h"

#include "driver/gpio.h"
#define LED_PIN GPIO_NUM_4

extern struct client g_client;

static const char *TAG = "quic_demo_main";

static uint8_t gbuffer[8192];  // Buffer for MQTT messages

static volatile bool g_send_ack = false;
static volatile bool g_send_sensors = false;

// MQTT application callback
static void eventCallback(MQTTContext_t *pContext,
                          MQTTPacketInfo_t *pPacketInfo,
                          MQTTDeserializedInfo_t *pDeserializedInfo)
{
    ESP_LOGI(TAG, "MQTT Event: Packet Type=%d", pPacketInfo->type);
    
    switch (pPacketInfo->type) {
        case MQTT_PACKET_TYPE_CONNACK:
            ESP_LOGI(TAG, "=== MQTT CONNACK RECEIVED ===");
            if (pPacketInfo->remainingLength >= 2) {
                // CONNACK has 2 bytes: Connect Acknowledge Flags + Connect Return Code
                uint8_t flags = pPacketInfo->pRemainingData[0];
                uint8_t returnCode = pPacketInfo->pRemainingData[1];
                
                bool sessionPresent = (flags & 0x01) != 0;
                ESP_LOGI(TAG, "CONNACK - Session Present: %s", sessionPresent ? "true" : "false");
                ESP_LOGI(TAG, "CONNACK - Return Code: %d", returnCode);
                
                if (returnCode == 0) {
                    ESP_LOGI(TAG, "✓ MQTT Connection Successfully Established!");
                } else {
                    ESP_LOGE(TAG, "✗ MQTT Connection Failed with return code: %d", returnCode);
                }
            } else {
                ESP_LOGW(TAG, "CONNACK packet received but insufficient data");
            }
            break;
            
        case MQTT_PACKET_TYPE_PUBLISH:
            ESP_LOGI(TAG, "=== MQTT PUBLISH RECEIVED ===");
            if (pDeserializedInfo && pDeserializedInfo->pPublishInfo) {

                const char *topic = (const char *)pDeserializedInfo->pPublishInfo->pTopicName;
                uint16_t topicLen = pDeserializedInfo->pPublishInfo->topicNameLength;
                const char *payload = (const char *)pDeserializedInfo->pPublishInfo->pPayload;
                uint16_t payloadLen = pDeserializedInfo->pPublishInfo->payloadLength;
                
                // Kontrola RGB-a
                if (strncmp(topic, "esp32/rgb", topicLen) == 0) {
                    if (strncmp(payload, "R", payloadLen) == 0) control_rgb(255, 0, 0);
                    else if (strncmp(payload, "G", payloadLen) == 0) control_rgb(0, 255, 0);
                    else if (strncmp(payload, "B", payloadLen) == 0) control_rgb(0, 0, 255);
                    else if (strncmp(payload, "OFF", payloadLen) == 0) control_rgb(0, 0, 0);
                    
                    // Samo digni zastavicu za ACK
                    g_send_ack = true;
                }

                // Kontrola zvučnika
                else if (strncmp(topic, "esp32/speaker", topicLen) == 0) {
                    if (strncmp(payload, "GREEN", payloadLen) == 0) {
                        play_speaker_tone(1000, 200);
                    } 
                    else if (strncmp(payload, "RED", payloadLen) == 0) {
                        play_speaker_tone(1000, 400);
                        vTaskDelay(pdMS_TO_TICKS(200));
                        play_speaker_tone(1000, 400);
                    } 
                    else if (strncmp(payload, "BLUE", payloadLen) == 0) {
                        for (int i = 0; i < 3; i++) {
                            play_speaker_tone(1000, 100);
                            if (i < 2) vTaskDelay(pdMS_TO_TICKS(100));
                        }
                    }
                    
                    // Samo digni zastavicu za ACK
                    g_send_ack = true;
                }
                
                // Zahtjev za očitavanje senzora (Pull)
                else if (strncmp(topic, "esp32/sensors/request", topicLen) == 0) {
                    // Samo digni zastavicu za senzore
                    g_send_sensors = true;
                }
            }
            break;
            
        case MQTT_PACKET_TYPE_PUBACK:
            ESP_LOGI(TAG, "=== MQTT PUBACK RECEIVED ===");
            if (pDeserializedInfo) {
                ESP_LOGI(TAG, "PUBACK - Packet ID: %d", pDeserializedInfo->packetIdentifier);
            }
            break;
            
        case MQTT_PACKET_TYPE_SUBACK:
            ESP_LOGI(TAG, "=== MQTT SUBACK RECEIVED ===");
            if (pDeserializedInfo) {
                ESP_LOGI(TAG, "SUBACK - Packet ID: %d", pDeserializedInfo->packetIdentifier);
            }
            // Parse SUBACK status codes from raw data if needed
            if (pPacketInfo->remainingLength >= 3) {
                ESP_LOGI(TAG, "SUBACK - Status codes available in raw data");
            }
            break;
            
        case MQTT_PACKET_TYPE_UNSUBACK:
            ESP_LOGI(TAG, "=== MQTT UNSUBACK RECEIVED ===");
            if (pDeserializedInfo) {
                ESP_LOGI(TAG, "UNSUBACK - Packet ID: %d", pDeserializedInfo->packetIdentifier);
            }
            break;
            
        case MQTT_PACKET_TYPE_PINGRESP:
            ESP_LOGI(TAG, "=== MQTT PINGRESP RECEIVED ===");
            break;
            
        default:
            ESP_LOGI(TAG, "=== UNKNOWN MQTT PACKET TYPE: %d ===", pPacketInfo->type);
            break;
    }
    
    // Log packet details for debugging
    ESP_LOGI(TAG, "Packet Details - Remaining Length: %zu, Type: 0x%02x", 
             pPacketInfo->remainingLength, pPacketInfo->type);
}

// Combined task that handles both QUIC and MQTT
void combined_quic_mqtt_task(void *pvParameters)
{
    ServerInfo_t *serverInfo = (ServerInfo_t *)pvParameters;
    if (!serverInfo) {
        ESP_LOGE(TAG, "No server info provided");
        vTaskDelete(NULL);
        return;
    }

    // Vanjska petlja za automatski reconnect
    while (1) {
        ESP_LOGI(TAG, "Starting combined QUIC+MQTT task (or Reconnecting...)");
        ESP_LOGI(TAG, "Free heap at task start: %lu bytes", esp_get_free_heap_size());
        
        // Convert port to string for QUIC config
        static char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", serverInfo->port);
        
        // Prepare QUIC client configuration
        quic_client_config_t quic_config = {
            .hostname = serverInfo->pHostName,
            .port = port_str,
            .alpn = serverInfo->pAlpn
        };

        ESP_LOGI(TAG, "Initializing QUIC client with %s:%s", quic_config.hostname, quic_config.port);
        ESP_LOGI(TAG, "Free heap before QUIC init: %lu bytes", esp_get_free_heap_size());
        
        // Initialize QUIC client (non-blocking)
        if (quic_client_init_with_config(&quic_config) != 0) {
            ESP_LOGE(TAG, "Failed to initialize QUIC client. Waiting 5s before retry...");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        ESP_LOGI(TAG, "QUIC client initialized, waiting for connection...");
        ESP_LOGI(TAG, "Free heap after QUIC init: %lu bytes", esp_get_free_heap_size());
        
        // Wait for QUIC connection to be established
        int connection_attempts = 0;
        const int max_attempts = 200; // 20 seconds at 100ms intervals
        
        while (!quic_client_is_connected() && connection_attempts < max_attempts) {
            // Process QUIC events
            int quic_process_result = 0;
        
            if (g_quic_mutex != NULL) {
                xSemaphoreTake(g_quic_mutex, portMAX_DELAY);
                quic_process_result = quic_client_process();
                xSemaphoreGive(g_quic_mutex);
            } else {
                quic_process_result = quic_client_process();
            }

            if (quic_process_result != 0) {
                ESP_LOGW(TAG, "QUIC client process failed - Breaking inner loop...");
                break; 
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            connection_attempts++;
            
            // Reset watchdog periodically
            if (connection_attempts % 5 == 0) {
                // Just delay to prevent watchdog trigger
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            
            if (connection_attempts % 20 == 0) {
                ESP_LOGI(TAG, "Still waiting for QUIC connection... (%d/20s)", connection_attempts/20);
            }
        }

        if (!quic_client_is_connected()) {
            ESP_LOGE(TAG, "Failed to establish QUIC connection after %d attempts", max_attempts);
            quic_client_cleanup();
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        ESP_LOGI(TAG, "QUIC connection established! Waiting a bit more for stability...");

        // Wait a bit more to ensure connection is stable
        vTaskDelay(pdMS_TO_TICKS(1000));
        connection_attempts = 0;
        while(!quic_client_local_stream_avail())
        {
            vTaskDelay(pdMS_TO_TICKS(100));
            ESP_LOGI(TAG, "Still waiting for QUIC streams... ");
        }
        
        // MQTT client setup
        MQTTContext_t mqttContext;
        MQTTStatus_t mqttStatus;
        NetworkContext_t networkContext;
        MQTTQUICConfig_t mqttQuicConfig = {
            .timeoutMs = 5000,
            .nonBlocking = false
        };
        
        // Initialize the transport layer
        BaseType_t transportStatus = mqtt_quic_transport_init(&networkContext, serverInfo, &mqttQuicConfig);
        if (transportStatus != pdPASS) {
            ESP_LOGE(TAG, "Failed to initialize transport");
            quic_client_cleanup();
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        
        // Set up the transport interface structure for core MQTT
        extern TransportInterface_t xTransportInterface;
        xTransportInterface.pNetworkContext = &networkContext;
        xTransportInterface.recv = mqtt_quic_transport_recv;
        xTransportInterface.send = mqtt_quic_transport_send;
        
        // Initialize MQTT library
        MQTTFixedBuffer_t networkBuffer;
        networkBuffer.pBuffer = gbuffer;
        networkBuffer.size = sizeof(gbuffer);
        
        ESP_LOGD(TAG, "Free heap before MQTT init: %lu bytes", esp_get_free_heap_size());
        
        extern uint32_t mqtt_get_time_ms(void);
        mqttStatus = MQTT_Init(&mqttContext,
                               &xTransportInterface,
                               mqtt_get_time_ms,
                               eventCallback,
                               &networkBuffer);
                               
        if (mqttStatus != MQTTSuccess) {
            ESP_LOGE(TAG, "Failed to initialize MQTT, error %d", mqttStatus);
            quic_client_cleanup();
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        
        ESP_LOGI(TAG, "MQTT initialized, connecting to broker...");

        // Connect to the MQTT broker
        MQTTConnectInfo_t connectInfo;
        memset(&connectInfo, 0, sizeof(connectInfo));
        connectInfo.cleanSession = false;
        connectInfo.pClientIdentifier = "esp32_quic_client";
        connectInfo.clientIdentifierLength = strlen("esp32_quic_client");
        // Postavljanje maksimalnog vremena neaktivnosti radi sprječavanja neželjenog prekida veze
        connectInfo.keepAliveSeconds = 30;
        
        ESP_LOGI(TAG, "Calling MQTT_Connect with timeout...");
        
        bool sessionPresent = false;
        
        // Use a shorter timeout for MQTT connect to prevent watchdog
        mqttStatus = MQTT_Connect(&mqttContext, &connectInfo, NULL, 5000, &sessionPresent);
        
        ESP_LOGI(TAG, "MQTT_Connect returned: %d, sessionPresent: %s", mqttStatus, sessionPresent ? "true" : "false");
        if (mqttStatus != MQTTSuccess) {
            ESP_LOGE(TAG, "Failed to connect to MQTT broker, error %d", mqttStatus);
            quic_client_cleanup();
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        
        ESP_LOGI(TAG, "Connected to MQTT broker over QUIC!");
        
        // Give some time for CONNACK to be processed
        ESP_LOGI(TAG, "Waiting for CONNACK processing...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        
        // Preplate na teme
        MQTTSubscribeInfo_t subscribeInfo[4];
        
        subscribeInfo[0].qos = MQTTQoS0;
        subscribeInfo[0].pTopicFilter = "esp32/quic/test";
        subscribeInfo[0].topicFilterLength = strlen("esp32/quic/test");

        subscribeInfo[1].qos = MQTTQoS0;
        subscribeInfo[1].pTopicFilter = "esp32/rgb";
        subscribeInfo[1].topicFilterLength = strlen("esp32/rgb");

        subscribeInfo[2].qos = MQTTQoS0;
        subscribeInfo[2].pTopicFilter = "esp32/speaker";
        subscribeInfo[2].topicFilterLength = strlen("esp32/speaker");

        subscribeInfo[3].qos = MQTTQoS0;
        subscribeInfo[3].pTopicFilter = "esp32/sensors/request";
        subscribeInfo[3].topicFilterLength = strlen("esp32/sensors/request");
        
        mqttStatus = MQTT_Subscribe(&mqttContext, subscribeInfo, 4, 2); // Paket ID = 2
        
        // Publish a startup message
        MQTTPublishInfo_t publishInfo;
        memset(&publishInfo, 0, sizeof(publishInfo));
        publishInfo.qos = MQTTQoS0;
        publishInfo.pTopicName = "esp32/quic/test";
        publishInfo.topicNameLength = strlen("esp32/quic/test");
        publishInfo.pPayload = "Hello from ESP32 over MQTT+QUIC!";
        publishInfo.payloadLength = strlen("Hello from ESP32 over MQTT+QUIC!");
        
        mqttStatus = MQTT_Publish(&mqttContext, &publishInfo, 0);
        if (mqttStatus != MQTTSuccess) {
            ESP_LOGE(TAG, "Failed to publish message, error %d", mqttStatus);
        } else {
            ESP_LOGI(TAG, "Published message to esp32/quic/test");
        }
        
        // Unutarnja radna petlja
        ESP_LOGI(TAG, "Entering main processing loop...");
        int loop_count = 0;
        char sensor_payload[128]; 
        bool last_button_state = false;
        uint32_t last_button_press_time = 0;

        while (1) {
            // Osnovna pauza (20ms)
            vTaskDelay(pdMS_TO_TICKS(20)); 
            loop_count++;

            int inner_quic_process_result = 0;
            
            if (g_quic_mutex != NULL) {
                xSemaphoreTake(g_quic_mutex, portMAX_DELAY);
                inner_quic_process_result = quic_client_process();
                xSemaphoreGive(g_quic_mutex);
            } else {
                inner_quic_process_result = quic_client_process();
            }

            if (inner_quic_process_result != 0) {
                ESP_LOGW(TAG, "QUIC client process failed - Breaking inner loop...");
                break;
            }

            // Provjera dolazne poruke
            mqttStatus = MQTT_ProcessLoop(&mqttContext);
            if (mqttStatus != MQTTSuccess) {
                ESP_LOGW(TAG, "MQTT loop failed, error %d", mqttStatus);
                if(mqttStatus == MQTTKeepAliveTimeout) break; 
            }

            if (g_send_ack) {
                MQTTPublishInfo_t ackInfo = {
                    .qos = MQTTQoS0,
                    .pTopicName = "esp32/ack",
                    .topicNameLength = strlen("esp32/ack"),
                    .pPayload = "OK",
                    .payloadLength = 2
                };
                MQTT_Publish(&mqttContext, &ackInfo, 0);
                g_send_ack = false; 
            }

            // Slanje senzora na zahtjev
            if (g_send_sensors) {
                float temp, hum;
                if (read_temp_humidity(&temp, &hum)) {
                    snprintf(sensor_payload, sizeof(sensor_payload), 
                             "{\"temperature\": %.1f, \"humidity\": %.1f}", 
                             temp, hum);
                    
                    MQTTPublishInfo_t pubInfo = {
                        .qos = MQTTQoS0,
                        .pTopicName = "esp32/sensors",
                        .topicNameLength = strlen("esp32/sensors"),
                        .pPayload = sensor_payload,
                        .payloadLength = strlen(sensor_payload)
                    };
                    MQTT_Publish(&mqttContext, &pubInfo, 0);
                    ESP_LOGI(TAG, "Poslani podaci senzora na zahtjev.");
                }
                g_send_sensors = false; 
            }

            // Tipka 
            bool current_button_state = is_button_pressed();
            uint32_t current_time = esp_timer_get_time() / 1000; 

            if (current_button_state && !last_button_state) {
                if (current_time - last_button_press_time > 200) { 
                    last_button_press_time = current_time;
                    last_button_state = true;
                    
                    MQTTPublishInfo_t pubInfoBtn = {
                        .qos = MQTTQoS0,
                        .pTopicName = "esp32/button",
                        .topicNameLength = strlen("esp32/button"),
                        .pPayload = "TRIGGER_AI",
                        .payloadLength = strlen("TRIGGER_AI")
                    };
                    MQTT_Publish(&mqttContext, &pubInfoBtn, 0); 
                    ESP_LOGI(TAG, "Gumb pritisnut");
                }
            } else if (!current_button_state) {
                last_button_state = false;
            }

            // Provjera je li QUIC još živ
            if (!quic_client_is_connected()) {
                ESP_LOGW(TAG, "QUIC connection lost");
                break;
            }
        }

        // Cleanup nakon pucanja veze
        ESP_LOGI(TAG, "Cleaning up QUIC and waiting 5s before reconnect...");
        quic_client_cleanup();
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void wifi_init(void)
{
    printf("init wifi...\n");
    // System initialization
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(example_connect());

    wifi_ap_record_t ap_info;
    ESP_ERROR_CHECK(esp_wifi_sta_get_ap_info(&ap_info));
    ESP_LOGI(TAG, "--- Access Point Information ---");
    ESP_LOG_BUFFER_HEX("MAC Address", ap_info.bssid, sizeof(ap_info.bssid));
    ESP_LOG_BUFFER_CHAR("SSID", ap_info.ssid, sizeof(ap_info.ssid));
    ESP_LOGI(TAG, "Primary Channel: %d", ap_info.primary);
    ESP_LOGI(TAG, "RSSI: %d", ap_info.rssi);
    printf("init wifi done!\n");

}
void app_main(void)
{
    ESP_LOGI(TAG, "Initializing...");

    hardware_init();
    
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Connect to WiFi
    ESP_LOGI(TAG, "Connecting to WiFi...");
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(example_connect());
    
    ESP_LOGI(TAG, "WiFi connected, starting combined QUIC+MQTT task...");
    
    // Log memory status before starting
    ESP_LOGI(TAG, "Free heap before task creation: %lu bytes", esp_get_free_heap_size());
    
    // Create server info for QUIC client
    static ServerInfo_t serverInfo = {
        .pHostName = "192.168.1.121", 
        .port = 14567,
        .pAlpn = "mqtt"
    };
    
    // Run the combined QUIC+MQTT task with smaller stack
    xTaskCreate(combined_quic_mqtt_task, "quic_mqtt_task", 28*1024, &serverInfo, 5, NULL);

    while (1) {
         vTaskDelay(10000 / portTICK_PERIOD_MS); // Yield to other tasks
    }
}

// Fix za wolfsll linker error
#include "wolfssl/wolfcrypt/settings.h"
#include "wolfssl/ssl.h"

WOLFSSL_SESSION* TlsSessionCacheGetAndRdLock(const unsigned char* id, int len) {
    (void)id;
    (void)len;
    return NULL; 
}

void TlsSessionCacheUnlockRow(const unsigned char* id) {
    (void)id;
}