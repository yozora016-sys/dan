#include "config.h" 
#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include "DHT.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <BlynkSimpleEsp32.h>
#include <WiFiManager.h>
#include <WebSocketMCP.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <math.h>

Preferences preferences;
volatile bool is_mcp_connected = false;

//schema mcp
String sensorSchema = R"({    
  "type": "object",
  "properties": {},
  "required": []
})";

String mistingSchema = R"({
  "type": "object",
  "properties": {
    "mode": { "type": "string", "enum": ["auto", "manual"] },
    "level": { "type": "integer", "enum": [0, 1, 2] }
  },
  "required": [] 
})";

String sleepSchema = R"({
  "type": "object",
  "properties": {
    "sleep": { "type": "boolean" }
  },
  "required": ["sleep"] 
})";

// token mcp
WebSocketMCP mcpClient;
const char* mcp_endpoint = "";

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
DHT dht(DHTPIN, DHTTYPE);

typedef enum { MODE_MANUAL = 0, MODE_AUTO = 1 } OperatingMode;
typedef enum { MIST_OFF = 0, MIST_LOW = 1, MIST_HIGH = 2 } MistLevel;

//struct state
struct SystemState {
  float temperature;
  float humidity;
  int water_level;
  float dew_point;
  OperatingMode mode; 
  MistLevel current_level; 
  int alert_led;
  TickType_t last_sensor_update; 
  bool is_sleeping;
};
SystemState sysState = {0, 0, 100,0, MODE_AUTO, MIST_OFF, 0, 0,false};

//struct cmd
typedef enum { CMD_FROM_BLYNK, CMD_FROM_AI, CMD_FROM_AUTO, CMD_FROM_SYSTEM } CommandSource;
struct MistingCommand {
  CommandSource source;
  MistLevel target_level; 
};

//ipc
QueueHandle_t commandQueue;
SemaphoreHandle_t sysStateMutex; 
EventGroupHandle_t systemEventGroup;
SemaphoreHandle_t flashMutex;

void Task_Sensors(void *pvParameters);       
void Task_Display(void *pvParameters);       
void Task_Misting_Ctrl(void *pvParameters);
void Task_Blynk(void *pvParameters);
void Task_Button(void *pvParameters);
void Task_MCP(void *pvParameters);

TaskHandle_t sensorTaskHandle = NULL;
TaskHandle_t displayTaskHandle = NULL;
TaskHandle_t buttonTaskHandle = NULL;

// save flash
void saveSettingsToFlash(int mode, int level, int sleep_state, uint8_t save_flags) {
    if (flashMutex != NULL && xSemaphoreTake(flashMutex, portMAX_DELAY) == pdTRUE) {
        if (save_flags & SAVE_FLAG_MODE)  preferences.putInt("sys_mode", mode);
        if (save_flags & SAVE_FLAG_LEVEL) preferences.putInt("sys_level", level);
        if (save_flags & SAVE_FLAG_SLEEP) preferences.putInt("sys_sleep", sleep_state);
        
        xSemaphoreGive(flashMutex); 
    }
}

void setup() {
  Serial.begin(115200);
  
  // khoi tao
  dht.begin();
  analogSetPinAttenuation(SIGNAL_PIN, ADC_11db);
  
  pinMode(MOSFET_PIN, OUTPUT);
  digitalWrite(MOSFET_PIN, LOW);
  pinMode(POWER_PIN, OUTPUT);
  digitalWrite(POWER_PIN, LOW);
  pinMode(LED_WARNING_PIN, OUTPUT);
  digitalWrite(LED_WARNING_PIN, LOW);

  pinMode(BUTTON_MODE_PIN, INPUT_PULLUP);
  pinMode(BUTTON_SLEEP_PIN, INPUT_PULLUP);
  
  Wire.begin(21, 22);   
  delay(200); 
  u8g2.begin();

  //doc flash
  preferences.begin("mist_app", false); 
  
  int saved_mode = preferences.getInt("sys_mode", 1);
  int saved_level = preferences.getInt("sys_level", 0); 
  
  sysState.is_sleeping = (preferences.getInt("sys_sleep", 0) == 1);
  sysState.mode = (saved_mode == 1) ? MODE_AUTO : MODE_MANUAL;
  
  if (sysState.is_sleeping) {
    sysState.current_level = MIST_OFF;
  } else if (sysState.mode == MODE_MANUAL) {
    sysState.current_level = (MistLevel)saved_level;
  } else {
    sysState.current_level = MIST_OFF; 
  }

  commandQueue = xQueueCreate(10, sizeof(MistingCommand)); 
  sysStateMutex = xSemaphoreCreateMutex();
  systemEventGroup = xEventGroupCreate();
  flashMutex = xSemaphoreCreateMutex();

  if (commandQueue == NULL || sysStateMutex == NULL || systemEventGroup == NULL || flashMutex == NULL) {
    ESP.restart();
  }
  
  xEventGroupSetBits(systemEventGroup, BIT_WATER_OK); 

  //core 1
  xTaskCreatePinnedToCore(Task_Sensors,      "Sensors",   6144, NULL, 3, &sensorTaskHandle, 1);
  xTaskCreatePinnedToCore(Task_Display,      "Display",   6144, NULL, 1, &displayTaskHandle, 1);
  xTaskCreatePinnedToCore(Task_Misting_Ctrl, "MistCtrl",  4096, NULL, 2, NULL, 1); 
  xTaskCreatePinnedToCore(Task_Button, "ButtonTask", 2048, NULL, 4, NULL, 1);
  //core 0
  xTaskCreatePinnedToCore(Task_MCP, "Task_MCP", 8192, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(Task_Blynk, "BlynkIoT",  8192, NULL, 1, NULL, 0);
}

void loop() {
  vTaskDelete(NULL); 
}
