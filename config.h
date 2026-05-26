// config.h
#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

#define BIT_BTN_MODE  (1 << 0)
#define BIT_BTN_SLEEP (1 << 1)

// blynk
#define BLYNK_TEMPLATE_ID   ""
#define BLYNK_TEMPLATE_NAME ""
#define BLYNK_AUTH_TOKEN    ""

//dht11
#define DHTPIN 4
#define DHTTYPE DHT11 

#define BUTTON_MODE_PIN 13

#define BUTTON_SLEEP_PIN 14

//water sensor
#define POWER_PIN 17
#define SIGNAL_PIN 36
#define MIN_LEVEL 0    
#define MAX_LEVEL 1000  
#define LED_WARNING_PIN 27      

//mosfet
#define MOSFET_PIN 18 

//nguong
#define THRESHOLD_WATER_WARN 5    // warning
#define THRESHOLD_WATER_OFF  5

#define BIT_WATER_OK (1 << 0)

#define SAVE_FLAG_MODE  0b001 // 0
#define SAVE_FLAG_LEVEL 0b010 // 1
#define SAVE_FLAG_SLEEP 0b100 // 2

extern SemaphoreHandle_t flashMutex;
extern void saveSettingsToFlash(int mode, int level, int sleep_state, uint8_t save_flags);

#endif
