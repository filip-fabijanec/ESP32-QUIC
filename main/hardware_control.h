#ifndef HARDWARE_CONTROL_H
#define HARDWARE_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

// Inicijalizacija svih hardverskih pinova i periferija
void hardware_init(void);

// Kontrola aktuatora (Poziva se iz MQTT povratne funkcije (callback))
void control_relay(bool state);
void control_rgb(uint8_t r, uint8_t g, uint8_t b);
void play_speaker_tone(int frequency, int duration_ms);

// Očitavanje senzora (Poziva se iz glavne petlje)
// Vraća true ako je očitavanje bilo uspješno
bool read_temp_humidity(float *temp, float *hum);
int read_light_sensor(void);
bool is_button_pressed(void);

#endif // HARDWARE_CONTROL_H