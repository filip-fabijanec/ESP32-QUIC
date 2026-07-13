#include "hardware_control.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "rom/ets_sys.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dht.h"

static const char *TAG = "hardware_control";

#define RGB_B_PIN GPIO_NUM_12
#define RGB_G_PIN GPIO_NUM_11 
#define RGB_R_PIN GPIO_NUM_10
#define SPEAKER_PIN GPIO_NUM_13
#define TEMP_HUMIDITY_SENSOR_PIN GPIO_NUM_4
#define BUTTON_PIN GPIO_NUM_14

void hardware_init(void) {
    // Inicijalizacija RGB LED-a
    gpio_reset_pin(RGB_R_PIN);
    gpio_set_direction(RGB_R_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(RGB_R_PIN, 0);

    gpio_reset_pin(RGB_G_PIN);
    gpio_set_direction(RGB_G_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(RGB_G_PIN, 0);

    gpio_reset_pin(RGB_B_PIN);
    gpio_set_direction(RGB_B_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(RGB_B_PIN, 0);
    ESP_LOGI(TAG, "RGB LED inicijaliziran!");

    // Inicijalizacija zvučnika
    gpio_reset_pin(SPEAKER_PIN);
    gpio_set_direction(SPEAKER_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(SPEAKER_PIN, 0);
    ESP_LOGI(TAG, "Zvučnik inicijaliziran!");

    // Inicijalizacija tipke
    gpio_reset_pin(BUTTON_PIN);
    gpio_set_direction(BUTTON_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BUTTON_PIN, GPIO_PULLUP_ONLY);

    // Inicijalizacija DHT11 senzora
    gpio_set_pull_mode(TEMP_HUMIDITY_SENSOR_PIN, GPIO_PULLUP_ONLY);
}

void control_rgb(uint8_t r, uint8_t g, uint8_t b) {
    gpio_set_level(RGB_R_PIN, r > 0 ? 1 : 0);
    gpio_set_level(RGB_G_PIN, g > 0 ? 1 : 0);
    gpio_set_level(RGB_B_PIN, b > 0 ? 1 : 0);
    ESP_LOGI(TAG, "RGB LED postavljen na R:%d G:%d B:%d", r, g, b);
}

void play_speaker_tone(int frequency, int duration_ms) {
    ESP_LOGI(TAG, "Aktiviram zujalicu na %d ms", duration_ms);
    gpio_set_level(SPEAKER_PIN, 1); 
    vTaskDelay(pdMS_TO_TICKS(duration_ms)); 
    gpio_set_level(SPEAKER_PIN, 0);
}

// Očitavanje DHT11 senzora
bool read_temp_humidity(float *temp, float *hum) {
    int16_t raw_temperature = 0;
    int16_t raw_humidity = 0;

    esp_err_t res = dht_read_data(DHT_TYPE_DHT11, TEMP_HUMIDITY_SENSOR_PIN, &raw_humidity, &raw_temperature);

    if (res == ESP_OK) {
        *temp = raw_temperature / 10.0;
        *hum = raw_humidity / 10.0;
        
        ESP_LOGI(TAG, "Očitano: Temp: %.1f °C, Vlaga: %.1f %%", *temp, *hum);
        return true;
    } else {
        ESP_LOGW(TAG, "Greška pri čitanju DHT11 senzora (Kod greške: %d)", res);
        return false;
    }
}

bool is_button_pressed(void) {
    bool pressed = (gpio_get_level(BUTTON_PIN) == 0);
    return pressed;
}