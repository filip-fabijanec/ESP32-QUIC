#include "mqtt_quic_transport.h"
#include "esp_log.h"
#include "ngtcp2_sample.h"
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "MQTT_QUIC";

// Global transport interface
TransportInterface_t xTransportInterface = {0};

SemaphoreHandle_t g_quic_mutex = NULL;

// Forward declarations for ngtcp2 client functions
extern struct client g_client;
extern int quic_client_write_safe(const uint8_t *data, size_t datalen);
extern int quic_client_read_safe(uint8_t *buffer, size_t buffer_size, size_t *bytes_read);
extern bool quic_client_is_connected(void);

// Time function required by MQTT
uint32_t mqtt_get_time_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/**
 * @brief Decode MQTT variable byte integer (remaining length)
 */
static int decode_mqtt_remaining_length(const uint8_t *data, size_t data_len, 
                                      uint32_t *remaining_length, size_t *bytes_used) {
    if (data_len < 2) {
        return -1; // Not enough data to decode
    }
    
    *remaining_length = 0;
    *bytes_used = 0;
    uint32_t multiplier = 1;
    
    for (size_t i = 1; i < data_len && i < 5; i++) { // Skip first byte (packet type)
        uint8_t byte = data[i];
        *remaining_length += (byte & 0x7F) * multiplier;
        (*bytes_used)++;
        
        if ((byte & 0x80) == 0) {
            return 0;
        }
        
        multiplier *= 128;
        if (multiplier > 128 * 128 * 128) {
            return -1; // Malformed remaining length
        }
    }
    
    return -1; // Incomplete or malformed
}

/**
 * @brief Determine if we have enough data to know the complete MQTT packet length
 */
static bool determine_mqtt_packet_length(NetworkContext_t *context) {
    if (context->packet_length_determined) {
        return true;
    }
    
    if (context->send_buffer_len < 2) {
        return false; 
    }
    
    uint32_t remaining_length;
    size_t bytes_used;
    
    if (decode_mqtt_remaining_length(context->send_buffer, context->send_buffer_len,
                                   &remaining_length, &bytes_used) == 0) {
        context->expected_packet_length = 1 + bytes_used + remaining_length; 
        context->packet_length_determined = true;
        
        if (context->send_buffer[0] == 0x10) {
            context->is_mqtt_connect_packet = true;
        }
        
        return true;
    }
    
    return false;
}

/**
 * @brief Send the complete MQTT packet over QUIC
 */
static int send_complete_mqtt_packet(NetworkContext_t *context) {
    if (context->send_buffer_len == 0 || context->send_buffer_len > sizeof(context->send_buffer)) {
        ESP_LOGE(TAG, "Invalid packet length: %zu", context->send_buffer_len);
        return -1;
    }
    
    if (!quic_client_is_connected()) {
        ESP_LOGE(TAG, "QUIC client is not connected, cannot send data");
        return -1;
    }
    
    vTaskDelay(pdMS_TO_TICKS(10));
    
    int result = -1;
    
    if (g_quic_mutex != NULL) {
        xSemaphoreTake(g_quic_mutex, portMAX_DELAY);
        result = quic_client_write_safe(context->send_buffer, context->send_buffer_len);
        xSemaphoreGive(g_quic_mutex);
    } else {
        result = quic_client_write_safe(context->send_buffer, context->send_buffer_len);
    }
    
    if (result != 0) {
        ESP_LOGE(TAG, "Failed to send complete MQTT packet over QUIC, error %d", result);
        return -1;
    }
    
    ESP_LOGI(TAG, "Successfully sent complete MQTT packet (%zu bytes) over QUIC", context->send_buffer_len);
    return 0;
}

static void reset_send_buffer(NetworkContext_t *context) {
    context->send_buffer_len = 0;
    context->expected_packet_length = 0;
    context->packet_length_determined = false;
    context->is_mqtt_connect_packet = false;
}

int32_t mqtt_quic_transport_send(NetworkContext_t *pNetworkContext, const void *pBuffer, size_t bytesToSend)
{
    if (pNetworkContext == NULL || pBuffer == NULL) return -1;
    if (bytesToSend == 0) return 0;

    const uint8_t *data = (const uint8_t *)pBuffer;
    
    if (pNetworkContext->send_buffer_len + bytesToSend > sizeof(pNetworkContext->send_buffer)) {
        ESP_LOGE(TAG, "Send buffer overflow!");
        return -1;
    }
    
    memcpy(pNetworkContext->send_buffer + pNetworkContext->send_buffer_len, data, bytesToSend);
    pNetworkContext->send_buffer_len += bytesToSend;
    
    if (!pNetworkContext->packet_length_determined) {
        determine_mqtt_packet_length(pNetworkContext);
    }
    
    if (pNetworkContext->packet_length_determined && 
        pNetworkContext->send_buffer_len >= pNetworkContext->expected_packet_length) {
        
        int result = send_complete_mqtt_packet(pNetworkContext);
        reset_send_buffer(pNetworkContext);
        
        if (result != 0) return -1;
    }
    
    return (int32_t)bytesToSend;
}

int32_t mqtt_quic_transport_recv(NetworkContext_t *pNetworkContext, void *pBuffer, size_t bytesToRecv)
{
    if (pNetworkContext == NULL || pBuffer == NULL) return -1;
    
    size_t bytesReceived = 0;
    
    if (!quic_client_is_connected()) {
        return 0;
    }
    
    int result = -1;
    
    if (g_quic_mutex != NULL) {
        xSemaphoreTake(g_quic_mutex, portMAX_DELAY);
        result = quic_client_read_safe((uint8_t *)pBuffer, bytesToRecv, &bytesReceived);
        xSemaphoreGive(g_quic_mutex);
    } else {
        result = quic_client_read_safe((uint8_t *)pBuffer, bytesToRecv, &bytesReceived);
    }
    
    if (result != 0) {
        if (result == -2) return 0; // No data
        return -1;
    }
    
    return (int32_t)bytesReceived;
}

BaseType_t mqtt_quic_transport_init(NetworkContext_t *pNetworkContext,
                                  const ServerInfo_t *pServerInfo,
                                  const MQTTQUICConfig_t *pMqttQuicConfig)
{
    if (pNetworkContext == NULL || pServerInfo == NULL || pMqttQuicConfig == NULL) {
        return pdFAIL;
    }

    if (g_quic_mutex == NULL) {
        g_quic_mutex = xSemaphoreCreateMutex();
    }

    ESP_LOGI(TAG, "Initializing MQTT-over-QUIC transport");
    
    pNetworkContext->pServerInfo = pServerInfo;
    pNetworkContext->pMqttQuicConfig = pMqttQuicConfig;
    
    reset_send_buffer(pNetworkContext);
    
    return pdPASS;
}