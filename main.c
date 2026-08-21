#include <Wire.h>
#include <MPU6050_light.h>
#include <BleMouse.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

// --- 使用者設定 ---
const char* ssid = "";
const char* password = "";
String myWriteAPIKey = "";
const char* discord_webhook_url = "";

// --- 參數設定 ---
#define MPU_ADDR 0x68
#define SENSITIVITY_X 10 
#define SENSITIVITY_Y 10
#define SENSITIVITY_SCROLL 40 
#define DEADZONE 3

#define PIN_RIGHT_CLICK 17
#define PIN_LEFT_CLICK 16
#define PIN_SCROLL_BTN 4 

MPU6050 mpu(Wire);
BleMouse bleMouse("Mouse", "ESP32", 100);

volatile int sharedClickCount = 0; 
volatile unsigned long lastActivityTime = 0; 

TaskHandle_t Task1;
SemaphoreHandle_t xMutex;

// ==========================================
//   Core 0 任務: WiFi / Discord / ThingSpeak
// ==========================================
void TaskHTTPCode(void * pvParameters) {
  WiFi.setSleep(false);

  Serial.print("Core 0: Connecting to WiFi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(500 / portTICK_PERIOD_MS);
    Serial.print(".");
  }
  Serial.println(" Connected!");

  // NTP 對時
  configTime(28800, 0, "pool.ntp.org");
  struct tm timeinfo;
  int retrySync = 0;
  while(!getLocalTime(&timeinfo) && retrySync < 10){
    vTaskDelay(500 / portTICK_PERIOD_MS);
    retrySync++;
  }
  if(retrySync < 10) Serial.println("Time Synced!");
  
  unsigned long lastThingSpeakTime = 0;
  unsigned long sessionStartTime = millis(); 
  bool isResting = true;

  for(;;) { 
    // 斷線重連機制
    if(WiFi.status() != WL_CONNECTED){
      Serial.println("WiFi Lost, Reconnecting...");
      WiFi.disconnect();
      WiFi.reconnect();
      vTaskDelay(2000 / portTICK_PERIOD_MS);
    }

    if(WiFi.status() == WL_CONNECTED){
      unsigned long currentMillis = millis();

      // --- Discord 提醒邏輯 ---
      if (currentMillis - lastActivityTime < 30000) { 
        if (isResting) {
          sessionStartTime = currentMillis;
          isResting = false;
          Serial.println("User Active...");
        }

        // 檢查是否超過 1 分鐘
        if (currentMillis - sessionStartTime > 60000) {
          Serial.println("Sending Discord Alert...");
          
          WiFiClientSecure *client = new WiFiClientSecure;
          if(client) {
            client->setInsecure(); // 跳過憑證檢查
            
            client->setBufferSizes(1024, 1024);
            client->setTimeout(15000); // 設定超時

            HTTPClient https;
            String payload = "{\"content\": \"已經連續使用電腦 1 分鐘了，請休息一下！\"}";
            
            // 重試 3 次機制
            for(int i=0; i<3; i++) {
              if (https.begin(*client, discord_webhook_url)) {
                https.addHeader("Content-Type", "application/json");
                vTaskDelay(50 / portTICK_PERIOD_MS); 
                int httpCode = https.POST(payload);
                Serial.print("Discord Status: "); Serial.println(httpCode);
                https.end();
                
                // 204 或 200 代表成功
                if (httpCode > 0) break; 
              }
              vTaskDelay(1000 / portTICK_PERIOD_MS);
            }
            delete client; // 釋放記憶體
          }
          sessionStartTime = currentMillis; 
        }
      } else {
        if (!isResting) {
           isResting = true;
           Serial.println("User Resting...");
        }
      }

      // --- ThingSpeak 上傳邏輯 ---
      if (currentMillis - lastThingSpeakTime > 60000) {
        int clicksToSend = 0;
        if(xSemaphoreTake(xMutex, (TickType_t)10) == pdTRUE) {
          clicksToSend = sharedClickCount;
          sharedClickCount = 0;
          xSemaphoreGive(xMutex); 
        }
        
        HTTPClient http;
        String url = "http://api.thingspeak.com/update?api_key=" + myWriteAPIKey + 
                     "&field1=" + String(clicksToSend);
        http.begin(url);
        http.GET(); 
        http.end();
        lastThingSpeakTime = currentMillis;
      }
    }
    vTaskDelay(200 / portTICK_PERIOD_MS);
  }
}

// ==========================================
//   Core 1: setup
// ==========================================
void setup() {
  Serial.begin(115200);
  
  delay(2000);
  Serial.println("Starting Mouse Program...");

  Wire.begin(21, 22);
  
  pinMode(PIN_LEFT_CLICK, INPUT_PULLUP);
  pinMode(PIN_RIGHT_CLICK, INPUT_PULLUP);
  pinMode(PIN_SCROLL_BTN, INPUT_PULLUP);

  byte status = mpu.begin();
  if (status != 0) {
    Serial.print("MPU Init Failed: "); Serial.println(status);
    while (1) delay(1000);
  }
  Serial.println("MPU Calibrating... (Do not move)");
  delay(1000);
  mpu.calcOffsets();
  Serial.println("MPU Ready!");
  
  bleMouse.begin();
  Serial.println("BLE Mouse Started!");
  
  xMutex = xSemaphoreCreateMutex();

  // 啟動 Core 0 任務 (Stack Size 16000)
  xTaskCreatePinnedToCore(TaskHTTPCode, "TaskHTTP", 16000, NULL, 0, &Task1, 0);                 
}

// ==========================================
//   Core 1: loop (滑鼠邏輯)
// ==========================================
void loop() {
  mpu.update();

  if (bleMouse.isConnected()) {
    float gyroZ = mpu.getGyroZ(); 
    float gyroY = mpu.getGyroY();

    bool isMoving = (abs(gyroZ) > DEADZONE) || (abs(gyroY) > DEADZONE);
    bool isClicking = (digitalRead(PIN_LEFT_CLICK) == LOW) || 
                      (digitalRead(PIN_RIGHT_CLICK) == LOW) ||
                      (digitalRead(PIN_SCROLL_BTN) == LOW);

    if (isMoving || isClicking) {
       lastActivityTime = millis();
    }

    if (digitalRead(PIN_SCROLL_BTN) == LOW) {
      if (abs(gyroY) > DEADZONE) {
        int scrollAmount = (int)(gyroY / SENSITIVITY_SCROLL);
        if (scrollAmount != 0) {
          bleMouse.move(0, 0, scrollAmount);
        }
      }
    } else {
      int moveX = 0, moveY = 0;
      if (abs(gyroZ) > DEADZONE) moveX = (int)(gyroZ / SENSITIVITY_X);
      if (abs(gyroY) > DEADZONE) moveY = (int)(gyroY / SENSITIVITY_Y);
      if (moveX != 0 || moveY != 0) {
        bleMouse.move(moveX, moveY);
      }
    }

    if (digitalRead(PIN_LEFT_CLICK) == LOW) {
      if (!bleMouse.isPressed(MOUSE_LEFT)) {
        bleMouse.press(MOUSE_LEFT);
        if(xSemaphoreTake(xMutex, (TickType_t)5) == pdTRUE) {
           sharedClickCount++;
           xSemaphoreGive(xMutex);
        }
        delay(100); 
      }
    } else {
      if (bleMouse.isPressed(MOUSE_LEFT)) bleMouse.release(MOUSE_LEFT);
    }

    if (digitalRead(PIN_RIGHT_CLICK) == LOW) {
      if (!bleMouse.isPressed(MOUSE_RIGHT)) {
        bleMouse.press(MOUSE_RIGHT);
        delay(100);
      }
    } else {
      if (bleMouse.isPressed(MOUSE_RIGHT)) bleMouse.release(MOUSE_RIGHT);
    }
  }
  delay(10); 
}