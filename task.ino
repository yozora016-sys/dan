float calculateDewPoint(float t, float h) {
  float a = 17.271;
  float b = 237.7;
  float alpha = ((a * t) / (b + t)) + log(h / 100.0);
  float td = (b * alpha) / (a - alpha);
  return td;
}

//task doc cam bien
void Task_Sensors(void *pvParameters) {
  int value = 0;
  int percentage = 0;
  static bool water_ok = true;
  static int lowWaterCount = 0;

  for (;;) {
    bool is_sleeping = false;
    
    if (xSemaphoreTake(sysStateMutex, 10 / portTICK_PERIOD_MS) == pdTRUE) {
      is_sleeping = sysState.is_sleeping;
      xSemaphoreGive(sysStateMutex);
    }

    if (is_sleeping) {
      vTaskDelay(1000 / portTICK_PERIOD_MS);
      continue;
    }

    //tdo, do am
    float h = dht.readHumidity();
    float t = dht.readTemperature();
    if (isnan(h) || isnan(t)) {
      vTaskDelay(5000 / portTICK_PERIOD_MS);
      Serial.println("cam bien loi");
      continue;
    }

    float dp = calculateDewPoint(t, h);
    
    //muc nuoc tb
    digitalWrite(POWER_PIN, HIGH);
    vTaskDelay(100 / portTICK_PERIOD_MS);
    long avg = 0;
    for (int i = 0; i < 10; i++) {
      int j = analogRead(SIGNAL_PIN);
      avg += j;
      vTaskDelay(10 / portTICK_PERIOD_MS);
      Serial.println("adc");
      Serial.println(j);
    }
    value = avg / 10;
    digitalWrite(POWER_PIN, LOW);
    
    percentage = map(value, MIN_LEVEL, MAX_LEVEL, 0, 100);
    percentage = constrain(percentage, 0, 100);
    
    if (percentage < THRESHOLD_WATER_OFF ) {
      lowWaterCount++;
      if (lowWaterCount >= 3) {
        water_ok = false;
      }
    } else {
      lowWaterCount = 0;
      if (percentage >= THRESHOLD_WATER_OFF) {
        water_ok = true;
      }
    }

    if (water_ok) {
      xEventGroupSetBits(systemEventGroup, BIT_WATER_OK);
    } else {
      xEventGroupClearBits(systemEventGroup,BIT_WATER_OK);
    }
    
    int led_app_status = 0;
    if (percentage < THRESHOLD_WATER_WARN) {
      led_app_status = 1;
      digitalWrite(LED_WARNING_PIN,HIGH);
    } else {
      led_app_status = 0;
      digitalWrite(LED_WARNING_PIN,LOW);
    }

    //update struct
    if (xSemaphoreTake(sysStateMutex, 100 / portTICK_PERIOD_MS) == pdTRUE) {
      sysState.temperature = t;
      sysState.humidity = h;
      sysState.water_level = percentage;
      sysState.dew_point = dp;
      sysState.alert_led = led_app_status; 
      sysState.last_sensor_update = xTaskGetTickCount();  
      xSemaphoreGive(sysStateMutex);
    }
    vTaskDelay(5000 / portTICK_PERIOD_MS);
  }
}

//task display
void Task_Display(void *pvParameters) {
  int blinkCounter = 0;
  bool showWarning = true;
  bool was_sleeping = false; 

  for (;;) {
    float t = 0, h = 0;
    int percentage = 100;
    OperatingMode current_mode = MODE_AUTO;
    MistLevel current_level = MIST_OFF;
    bool is_sleeping = false;

    if (xSemaphoreTake(sysStateMutex, 10 / portTICK_PERIOD_MS) == pdTRUE) {
      t = sysState.temperature;
      h = sysState.humidity;
      percentage = sysState.water_level;
      current_mode = sysState.mode;
      current_level = sysState.current_level;
      is_sleeping = sysState.is_sleeping;
      xSemaphoreGive(sysStateMutex);
    }

    if (is_sleeping) {
      if (!was_sleeping) {
        u8g2.setPowerSave(1);
        was_sleeping = true;
      }
      vTaskDelay(500 / portTICK_PERIOD_MS);
      continue; 
    } else {
      if (was_sleeping) {
        u8g2.setPowerSave(0); 
        was_sleeping = false;
      }
    }
    blinkCounter++;
    if (blinkCounter >= 5) {
        showWarning = !showWarning;
        blinkCounter = 0;
    }

    u8g2.clearBuffer();

    if (percentage < THRESHOLD_WATER_WARN && showWarning) { 
      u8g2.setFont(u8g2_font_logisoso16_tr);
      u8g2.drawStr(20, 35, "LOW WATER!");
    } else {
      u8g2.setFont(u8g2_font_6x12_tr);
      if (current_mode == MODE_AUTO) {
        u8g2.drawStr(15, 10, "SMART MIST [AUTO]");
      } else {
        u8g2.drawStr(15, 10, "SMART MIST [MAN]");
      }
      u8g2.setCursor(5, 25); u8g2.print("T:"); u8g2.print(t); u8g2.print("C");
      u8g2.setCursor(70, 25); u8g2.print("H:"); u8g2.print(h); u8g2.print("%");

      u8g2.setCursor(5, 38); u8g2.print("Water");
      int barWidth = map(percentage, 0, 100, 0, 80);
      u8g2.drawFrame(5, 42, 80, 8);
      u8g2.drawBox(5, 42, barWidth, 8);
      u8g2.setCursor(90, 49); u8g2.print(percentage); u8g2.print("%");
      u8g2.setCursor(5, 60); u8g2.print("Mist:");
      if (current_level == MIST_HIGH) u8g2.print(" HIGH");
      else if (current_level == MIST_LOW) u8g2.print(" LOW");
      else u8g2.print(" OFF");
    }
    u8g2.sendBuffer();
    vTaskDelay(100 / portTICK_PERIOD_MS); 
  }
}


void Task_Misting_Ctrl(void *pvParameters) {
  MistingCommand receivedCmd;
  MistLevel active_level = MIST_OFF;
  bool is_mosfet_on = false;
  TickType_t last_switch_time = xTaskGetTickCount();

  //fsm
  enum SystemFSMState {
    STATE_SLEEP,      //sleep
    STATE_FAILSAFE,    //het nc
    STATE_AUTO,       //auto
    STATE_MANUAL      //man 
  };
  SystemFSMState currentState = STATE_AUTO;

  //khoi phuc muc phun
  if (xSemaphoreTake(sysStateMutex, 100 / portTICK_PERIOD_MS) == pdTRUE) {
    if (sysState.mode == MODE_MANUAL && sysState.current_level != MIST_OFF) {
      active_level = sysState.current_level;
    }
    xSemaphoreGive(sysStateMutex);
  }

  for (;;) {
    bool got_cmd = (xQueueReceive(commandQueue, &receivedCmd, 100 / portTICK_PERIOD_MS) == pdPASS);
    OperatingMode req_mode = MODE_AUTO;
    bool is_sleeping = false;
    float current_h = 0, current_t = 0, current_dp = 0;
    int current_wl = 0;

    // lay bien
    if (xSemaphoreTake(sysStateMutex, 10 / portTICK_PERIOD_MS) == pdTRUE) {
      req_mode = sysState.mode;
      is_sleeping = sysState.is_sleeping; 
      current_h = sysState.humidity;
      current_wl = sysState.water_level;
      current_t = sysState.temperature; 
      current_dp = sysState.dew_point;
      xSemaphoreGive(sysStateMutex);
    }

    //tinh dp
    float delta_T = current_t - current_dp;

    EventBits_t uxBits = xEventGroupWaitBits(systemEventGroup, BIT_WATER_OK, pdFALSE, pdTRUE, 0);
    bool hasWater = ((uxBits & BIT_WATER_OK) != 0);

    MistLevel req_manual_level = active_level;

    //bo qua lenh tu app neu dang auto
    if (got_cmd) {
      bool is_blynk_cmd  = (receivedCmd.source == CMD_FROM_BLYNK);
      if (!(req_mode == MODE_AUTO && is_blynk_cmd)) {
        req_manual_level = receivedCmd.target_level;
      }
    }

    //chuyen tt
    if (is_sleeping) {
      currentState = STATE_SLEEP;
    } 
    else if (!hasWater) { 
      currentState = STATE_FAILSAFE;
    } 
    else if (req_mode == MODE_AUTO) {
      currentState = STATE_AUTO;
    } 
    else {
      currentState = STATE_MANUAL;
    }
    
    MistLevel next_level = active_level;

    //thuc thi
    switch (currentState) {
      case STATE_SLEEP:
      case STATE_FAILSAFE:
        next_level = MIST_OFF;
        break;

      case STATE_MANUAL:
        next_level = req_manual_level; 
        break;
      case STATE_AUTO:
        if (delta_T < 3.0) {
          next_level = MIST_OFF;  
        } 
        else if (delta_T < 15.0) {
          next_level = MIST_LOW;   
        } 
        else {
          next_level = MIST_HIGH;   
        }
        break;
    }

    //neu newlv thi uddate
    if (active_level != next_level) {
      active_level = next_level;
      last_switch_time = xTaskGetTickCount();
      is_mosfet_on = (active_level != MIST_OFF);
    }

    TickType_t now = xTaskGetTickCount();
    uint32_t elapsed_ms = pdTICKS_TO_MS(now - last_switch_time);

    //logic bat tat
    if (active_level == MIST_HIGH) {
      is_mosfet_on = true;
    } else if (active_level == MIST_OFF) {
      is_mosfet_on = false;
    } else if (active_level == MIST_LOW) {
      if (is_mosfet_on && elapsed_ms >= 5000) {
        is_mosfet_on = false;
        last_switch_time = now;
      } else if (!is_mosfet_on && elapsed_ms >= 5000) {
        is_mosfet_on = true;
        last_switch_time = now;
      }
    }

    //thuc thi
    digitalWrite(MOSFET_PIN, is_mosfet_on ? HIGH : LOW);
    if (xSemaphoreTake(sysStateMutex, 10 / portTICK_PERIOD_MS) == pdTRUE) {
      sysState.current_level = active_level;
      xSemaphoreGive(sysStateMutex);
    }
  }
}

volatile uint32_t last_mode_irq_time = 0;
volatile uint32_t last_sleep_irq_time = 0;

void IRAM_ATTR isr_button_mode() {
  uint32_t current_time = millis(); 
  if (current_time - last_mode_irq_time > 200) { 
    last_mode_irq_time = current_time;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    
    if (buttonTaskHandle != NULL) {
      xTaskNotifyFromISR(buttonTaskHandle, BIT_BTN_MODE, eSetBits, &xHigherPriorityTaskWoken);
    }
    if (xHigherPriorityTaskWoken) {
      portYIELD_FROM_ISR();
    }
  }
}

void IRAM_ATTR isr_button_sleep() {
  uint32_t current_time = millis();
  if (current_time - last_sleep_irq_time > 200) {
    last_sleep_irq_time = current_time;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    
    if (buttonTaskHandle != NULL) {
      xTaskNotifyFromISR(buttonTaskHandle, BIT_BTN_SLEEP, eSetBits, &xHigherPriorityTaskWoken);
    }
    if (xHigherPriorityTaskWoken) {
      portYIELD_FROM_ISR();
    }
  }
}

// task nut bam
void Task_Button(void *pvParameters) {
  buttonTaskHandle = xTaskGetCurrentTaskHandle();
  attachInterrupt(digitalPinToInterrupt(BUTTON_MODE_PIN), isr_button_mode, FALLING);
  attachInterrupt(digitalPinToInterrupt(BUTTON_SLEEP_PIN), isr_button_sleep, FALLING);
  uint32_t notifiedValue = 0;

  for (;;) {
    xTaskNotifyWait(0x00, ULONG_MAX, &notifiedValue, portMAX_DELAY);
    bool is_system_sleeping = false;
    if (xSemaphoreTake(sysStateMutex, 10 / portTICK_PERIOD_MS) == pdTRUE) {
      is_system_sleeping = sysState.is_sleeping;
      xSemaphoreGive(sysStateMutex);
    }

    //nut mode
    if ((notifiedValue & BIT_BTN_MODE) != 0) {
      if (!is_system_sleeping) { 
        bool need_save_mode = false;
        OperatingMode new_mode = MODE_AUTO; 
        MistLevel new_level = MIST_OFF;
        if (xSemaphoreTake(sysStateMutex, 100 / portTICK_PERIOD_MS) == pdTRUE) {
          if (sysState.mode == MODE_AUTO) {
            new_mode = MODE_MANUAL;
            new_level = MIST_OFF;
          } else {
            if (sysState.current_level == MIST_OFF) { new_mode = MODE_MANUAL;
            new_level = MIST_LOW; }
            else if (sysState.current_level == MIST_LOW) { new_mode = MODE_MANUAL;
            new_level = MIST_HIGH; }
            else { new_mode = MODE_AUTO;
            new_level = MIST_OFF; }
          }

          if (sysState.mode != new_mode || sysState.current_level != new_level) {
            need_save_mode = true;
          }
          sysState.mode = new_mode;
          xSemaphoreGive(sysStateMutex);
        }

        if (need_save_mode) {
          saveSettingsToFlash(new_mode == MODE_AUTO ? 1 : 0, (int)new_level, 0, SAVE_FLAG_MODE | SAVE_FLAG_LEVEL);

          if (new_mode == MODE_MANUAL) {
            MistingCommand btnCmd;
            btnCmd.source = CMD_FROM_SYSTEM;
            btnCmd.target_level = new_level;
            xQueueSend(commandQueue, &btnCmd, 0); 
          } else {
            MistingCommand resetCmd;
            resetCmd.source = CMD_FROM_SYSTEM;
            resetCmd.target_level = MIST_OFF;
            xQueueSend(commandQueue, &resetCmd, 0);
          }
        }
      }
    }
    //on off
    if ((notifiedValue & BIT_BTN_SLEEP) != 0) {
      bool is_now_sleeping = false;
      bool state_changed = false;
      
      if (xSemaphoreTake(sysStateMutex, 100 / portTICK_PERIOD_MS) == pdTRUE) {
        sysState.is_sleeping = !sysState.is_sleeping;
        is_now_sleeping = sysState.is_sleeping;
        state_changed = true;
          
        xSemaphoreGive(sysStateMutex);
      }
      if (state_changed) {
        saveSettingsToFlash(0, 0, is_now_sleeping ? 1 : 0, SAVE_FLAG_SLEEP);
      }
    }
  }
}
