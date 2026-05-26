String onMistingControl(String params) {
  Serial.println(params);
  StaticJsonDocument<200> doc;
  if (deserializeJson(doc, params)) return "{\"status\": \"error\", \"message\": \"Invalid JSON\"}";
  
  bool need_save = false;
  int save_level_val = -1;
  int save_mode_val = 0;
  bool send_cmd = false;
  MistingCommand aiCmd;
  aiCmd.source = CMD_FROM_SYSTEM;
  
  if (xSemaphoreTake(sysStateMutex, 100 / portTICK_PERIOD_MS) == pdTRUE) {
    save_mode_val = (sysState.mode == MODE_AUTO) ? 1 : 0;
    if (sysState.is_sleeping) {
      xSemaphoreGive(sysStateMutex);
      return "{\"status\": \"error\", \"message\": \"Hệ thống đang ngủ . Vui lòng tắt chế độ ngủ trước.\"}";
    }

    if (doc.containsKey("mode")) {
      String mode = doc["mode"];
      if (mode == "manual" && sysState.mode != MODE_MANUAL) {
        sysState.mode = MODE_MANUAL;
        need_save = true;
        save_mode_val = 0;
        if (sysState.current_level == MIST_OFF) {
            save_level_val = 1;
            send_cmd = true;
            aiCmd.target_level = MIST_LOW;
        }
      } else if (mode == "auto" && sysState.mode != MODE_AUTO) {
        sysState.mode = MODE_AUTO;
        need_save = true;
        save_mode_val = 1;
        send_cmd = true;
        aiCmd.target_level = MIST_OFF;
      }
    }

    if (doc.containsKey("level")) { 
      if (sysState.mode == MODE_AUTO) {
        sysState.mode = MODE_MANUAL;
        need_save = true;
        save_mode_val = 0; 
      }
      int level = doc["level"];
      if (sysState.current_level != (MistLevel)level) {
        save_level_val = level;
        need_save = true;
        send_cmd = true;
        aiCmd.target_level = (MistLevel)level;
      }
    }
    xSemaphoreGive(sysStateMutex);
  } else {
    return "{\"status\": \"error\", \"message\": \"Hệ thống đang bận, vui lòng thử lại.\"}";
  }

  if (send_cmd) xQueueSend(commandQueue, &aiCmd, 0);

  if (need_save) {
    uint8_t flags = SAVE_FLAG_MODE;
    if (save_mode_val == 0 && save_level_val != -1) {
        flags |= SAVE_FLAG_LEVEL;
    }
    saveSettingsToFlash(save_mode_val, save_level_val, 0, flags);
  }
  return "{\"status\": \"success\", \"message\": \"Đã cập nhật mức phun sương.\"}";
}

void mcpConnectionCallback(bool isConnected) {
  is_mcp_connected = isConnected;
}

String onSleepControl(String params) {
  Serial.print("XiaoZhi yêu cầu (Ngủ): ");
  Serial.println(params);

  StaticJsonDocument<200> doc;
  if (deserializeJson(doc, params)) return "{\"status\": \"error\", \"message\": \"Invalid JSON\"}";
  if (!doc.containsKey("sleep")) return "{\"status\": \"error\", \"message\": \"Thiếu tham số sleep\"}";
  bool target_sleep = doc["sleep"];
  bool state_changed = false;

  if (xSemaphoreTake(sysStateMutex, 100/portTICK_PERIOD_MS) == pdTRUE) {
    if (sysState.is_sleeping != target_sleep) {
      sysState.is_sleeping = target_sleep;
      state_changed = true;
    }
    xSemaphoreGive(sysStateMutex);  
  }

  if (state_changed) {
    saveSettingsToFlash(0, 0, target_sleep ? 1 : 0, SAVE_FLAG_SLEEP);
  }

  return target_sleep ? "{\"status\": \"success\", \"message\": \"Đã kích hoạt chế độ ngủ đông.\"}" 
                      : "{\"status\": \"success\", \"message\": \"Hệ thống đã thức dậy.\"}";
}

String onReadSensors(String params) {
  Serial.println("XiaoZhi yêu cầu đọc cảm biến...");

  float t = 0.0;
  float h = 0.0;
  int water = 0;
  bool current_sleep_status = false; 
  
  if (xSemaphoreTake(sysStateMutex, 100 / portTICK_PERIOD_MS) == pdTRUE) {
    t = sysState.temperature;
    h = sysState.humidity;
    water = sysState.water_level;
    current_sleep_status = sysState.is_sleeping;
    xSemaphoreGive(sysStateMutex);
  } else {
    return "{\"status\": \"error\", \"message\": \"Hệ thống đang bận, không thể đọc dữ liệu lúc này.\"}";
  }

  StaticJsonDocument<200> doc;
  doc["status"] = "success";
  doc["temperature"] = t;
  doc["humidity"] = h;
  doc["water_level"] = water;
  doc["is_sleeping"] = current_sleep_status;
  String response;
  serializeJson(doc, response);
  
  Serial.print("Đã trả lời XiaoZhi: ");
  Serial.println(response);
  
  return response;
}

//task
void Task_MCP(void *pvParameters) {
  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }

  Serial.println("Khởi tạo kết nối MCP lần đầu...");
  mcpClient.begin(mcp_endpoint, mcpConnectionCallback);
  
  // tool
  mcpClient.registerTool(
    "misting_control", 
    "Điều khiển máy phun sương ", 
    mistingSchema, 
    onMistingControl
  );
  mcpClient.registerTool(
    "read_sensors", 
    "Đọc thông số môi trường hiện tại bao gồm nhiệt độ (C), độ ẩm (%) và lượng nước còn lại trong bình (%) và kiểm tra xem hệ thống đang bật hay tắt", 
    sensorSchema, 
    onReadSensors
  );
  mcpClient.registerTool(
    "sleep_control", 
    "Bật hoặc tắt hệ thống", 
    sleepSchema, 
    onSleepControl
  );

  TickType_t last_reconnect_attempt = 0;
  uint32_t mcp_retry_delay = 5000; 
  const uint32_t MAX_MCP_DELAY = 60000;
  
  for (;;) {
      if (WiFi.status() == WL_CONNECTED) {
        if (!is_mcp_connected) {
          TickType_t now = xTaskGetTickCount();
          if (now - last_reconnect_attempt > pdMS_TO_TICKS(mcp_retry_delay)) {
            Serial.print("[MCP] Đang thử kết nối lại XiaoZhi, delay hiện tại: ");
            Serial.print(mcp_retry_delay / 1000);
            Serial.println("s");
  
            mcpClient.begin(mcp_endpoint, mcpConnectionCallback); 
            last_reconnect_attempt = now;
  
            if (mcp_retry_delay < MAX_MCP_DELAY) {
              mcp_retry_delay *= 2;
              if (mcp_retry_delay > MAX_MCP_DELAY) mcp_retry_delay = MAX_MCP_DELAY;
            }
          }
        } else {
          mcp_retry_delay = 5000;
        }

      mcpClient.loop();
    }
    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
}
