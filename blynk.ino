BLYNK_WRITE(V0) { 
  int level_from_app = param.asInt(); 
  if (level_from_app < MIST_OFF || level_from_app > MIST_HIGH) return;
  bool was_auto = false;
  bool need_save = false;

  if (xSemaphoreTake(sysStateMutex, 100 / portTICK_PERIOD_MS) == pdTRUE) {
    if (sysState.is_sleeping) {
      Blynk.virtualWrite(V0, (int)sysState.current_level);
      xSemaphoreGive(sysStateMutex);
      return;
    }

    if (sysState.mode == MODE_AUTO) {
      sysState.mode = MODE_MANUAL; 
      was_auto = true;
      need_save = true;
    } else if (sysState.current_level != (MistLevel)level_from_app) {
      need_save = true;
    }
    xSemaphoreGive(sysStateMutex); 
  }
  
  if (need_save) {
    saveSettingsToFlash(0, level_from_app, 0, SAVE_FLAG_MODE | SAVE_FLAG_LEVEL);
  }
  
  MistingCommand blynkCmd;
  blynkCmd.source = was_auto ? CMD_FROM_SYSTEM : CMD_FROM_BLYNK;
  blynkCmd.target_level = (MistLevel)level_from_app;
  xQueueSend(commandQueue, &blynkCmd, 0);
  if (was_auto) {
    Blynk.virtualWrite(V5, 0); 
  }
}

BLYNK_WRITE(V5) {
  int mode_from_app = param.asInt();
  OperatingMode new_mode = (mode_from_app == 1) ? MODE_AUTO : MODE_MANUAL;
  OperatingMode old_mode = MODE_AUTO; 
  bool need_save = false;
  if (xSemaphoreTake(sysStateMutex, 100 / portTICK_PERIOD_MS) == pdTRUE) {
    if (sysState.is_sleeping) {
      Blynk.virtualWrite(V5, sysState.mode == MODE_AUTO ? 1 : 0);
      xSemaphoreGive(sysStateMutex);
      return; 
    }

    old_mode = sysState.mode;
    sysState.mode = new_mode;
    
    if (old_mode != new_mode) {
      need_save = true;
    }
    xSemaphoreGive(sysStateMutex); 
  } else {
    return;
  }

  if (need_save) {
    saveSettingsToFlash(new_mode == MODE_AUTO ? 1 : 0, 0, 0, SAVE_FLAG_MODE);
    MistingCommand resetCmd;
    resetCmd.source = CMD_FROM_SYSTEM; 
    resetCmd.target_level = MIST_OFF;
    xQueueSend(commandQueue, &resetCmd, 0);
  }
}

BLYNK_CONNECTED() {
  OperatingMode current_mode = MODE_AUTO; 
  MistLevel current_level = MIST_OFF;
  bool is_sleeping = false;
  
  if (xSemaphoreTake(sysStateMutex, 100 / portTICK_PERIOD_MS) == pdTRUE) {
    current_mode = sysState.mode;
    current_level = sysState.current_level;
    is_sleeping = sysState.is_sleeping;
    xSemaphoreGive(sysStateMutex);
  }
  
  Blynk.virtualWrite(V5, current_mode == MODE_AUTO ? 1 : 0);
  Blynk.virtualWrite(V0, (int)current_level);
  Blynk.virtualWrite(V8, is_sleeping ? 1 : 0);
}

BLYNK_WRITE(V8) {
  int sleep_val = param.asInt();
  bool is_now_sleeping = (sleep_val == 1);
  bool state_changed = false;
  if (xSemaphoreTake(sysStateMutex, 100 / portTICK_PERIOD_MS) == pdTRUE) {
    if (sysState.is_sleeping != is_now_sleeping) {
      sysState.is_sleeping = is_now_sleeping;
      state_changed = true;
    }
    xSemaphoreGive(sysStateMutex);
  }

  if (state_changed) {
    saveSettingsToFlash(0, 0, is_now_sleeping ? 1 : 0, SAVE_FLAG_SLEEP);
  }
}

void Task_Blynk(void *pvParameters) {
  TickType_t lastSendTick = xTaskGetTickCount(); 
  OperatingMode last_sent_mode = MODE_AUTO;
  MistLevel last_sent_level = MIST_OFF;
  bool last_sent_sleep = false;
  bool force_sync = true;

  Blynk.config(BLYNK_AUTH_TOKEN);
  WiFiManager wm;
  wm.setConfigPortalTimeout(60);
  bool connected = wm.autoConnect("SmartMist_Setup");
  if (connected) {
    Serial.println("\nWiFi OK! Dang ket noi Blynk...");
    Blynk.connect(5000);
  } else {
    Serial.println("\nQuá thời gian chờ nhập Wi-Fi - Chuyển sang chạy OFFLINE!");
  }

  uint32_t wifi_retry_delay = 5000; 
  const uint32_t MAX_WIFI_DELAY = 60000;
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.print("Mất WiFi, đang thử kết nối lại sau ");
      Serial.print(wifi_retry_delay / 1000);
      Serial.println(" giây...");
      
      WiFi.reconnect();
      vTaskDelay(wifi_retry_delay / portTICK_PERIOD_MS); 

      if (wifi_retry_delay < MAX_WIFI_DELAY) {
        wifi_retry_delay *= 2;
        if (wifi_retry_delay > MAX_WIFI_DELAY) wifi_retry_delay = MAX_WIFI_DELAY;
      }
      continue;
    } else {
      wifi_retry_delay = 5000;
    }
    
    if (!Blynk.connected()) {
      Blynk.connect(3000); 
      vTaskDelay(3000 / portTICK_PERIOD_MS);
      force_sync = true; 
      continue;
    }

    Blynk.run();

    if (pdTICKS_TO_MS(xTaskGetTickCount() - lastSendTick) >= 2000) {
      float t = 0.0, h = 0.0;
      int wl = 0, led = 0;
      OperatingMode current_mode = MODE_AUTO;
      MistLevel current_level = MIST_OFF;
      bool current_sleep = false;
      bool gotData = false;

      if (xSemaphoreTake(sysStateMutex, 10 / portTICK_PERIOD_MS) == pdTRUE) {
        t   = sysState.temperature;
        h   = sysState.humidity;
        wl  = sysState.water_level;
        led = sysState.alert_led;
        current_mode = sysState.mode;
        current_level = sysState.current_level;
        current_sleep = sysState.is_sleeping;
        gotData = true; 
        xSemaphoreGive(sysStateMutex);  
      }

      if (gotData) {
        Blynk.virtualWrite(V1, t);
        Blynk.virtualWrite(V2, h);
        Blynk.virtualWrite(V3, wl);
        Blynk.virtualWrite(V4, led);

        if (current_mode != last_sent_mode || force_sync) {
          Blynk.virtualWrite(V5, current_mode == MODE_AUTO ? 1 : 0);
          last_sent_mode = current_mode;
        }
        
        if (current_level != last_sent_level || force_sync) {
          Blynk.virtualWrite(V0, (int)current_level);
          last_sent_level = current_level;
        }
        if (current_sleep != last_sent_sleep || force_sync) {
          Blynk.virtualWrite(V8, current_sleep ? 1 : 0);
          last_sent_sleep = current_sleep;
        }

        force_sync = false;
      }
      lastSendTick = xTaskGetTickCount(); 
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}
