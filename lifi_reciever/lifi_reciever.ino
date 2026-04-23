// ==============================================================================
// NODE B: DATA RECEIVER & TELEMETRY TRANSMITTER (FreeRTOS Fixed)
// ==============================================================================

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>


const char* ssid = "Varshith's iPhone";
const char* password = "Password";
const char* mqtt_server = "broker.hivemq.com"; // Replace with your MQTT broker IP
const int mqtt_port = 1883;


const char* serverName = "http://api.thingspeak.com/update";
String apiKey = "T9MVUEBYM8GCSP97";

WiFiClient espClient;
PubSubClient client(espClient);

// Add these to track total throughput
volatile int totalMessagesTx = 0;
volatile int totalMessagesRx = 0;

const int TX_PIN = 25; 
const int RX_PIN = 34; 
const int BIT_MS = 50; 
const int HALF_BIT_MS = 25;

uint32_t rxMessageStartMs = 0;
char txBuf[128]; int txLen = 0;
enum TxMachineState { TX_M_IDLE, TX_M_PREAMBLE, TX_M_DATA, TX_M_NEWLINE };
TxMachineState txMState = TX_M_IDLE;
int txPreambleCount = 0; int txDataIndex = 0; int txBitStep = 0;
uint32_t txNextBitTime = 0; byte txByteToSend = 0;

char rxBuf[128]; int rxLen = 0;
enum RxState { WAIT_IDLE, WAIT_START, WAIT_HALF, RECV_BITS, RECV_STOP };
RxState rxState = WAIT_IDLE;
uint32_t rxNextTime = 0; int rxBitIndex = 0; byte rxByte = 0;

int adcMin = 4095; int adcMax = 0; int threshold = 2047; int hysteresis = 10;
bool calibFrozen = true;

// --- Helpers & Logic (unchanged) ---
void updateCalibration(int raw) {
  if (calibFrozen) return;
  if (raw < adcMin) adcMin = raw; 
  if (raw > adcMax) adcMax = raw;
  int swing = adcMax - adcMin;
  if (swing >= 50) { 
    threshold = adcMin + (swing / 2);
    // FORCE a smaller hysteresis (fixed at 50 or 5% of swing)
    hysteresis = 50; 
  }
}

int getLogicLevel(int raw) {
  if (raw > threshold + hysteresis) return HIGH;
  if (raw < threshold - hysteresis) return LOW;
  return -1;
}


// ==============================================================================
// 3. THE FreeRTOS MQTT TASK (Runs on Core 0)
// ==============================================================================
void thingSpeakTask(void *pvParameters) {
  for (;;) {
    // Only attempt if WiFi is up and LiFi is not busy
    if (WiFi.status() == WL_CONNECTED) {
      
      HTTPClient http;
      String url = String(serverName) + "?api_key=" + apiKey;
      url += "&field1=" + String(adcMax);
      url += "&field2=" + String(threshold);
      url += "&field3=" + String(adcMax - adcMin);
      url += "&field4=" + String(hysteresis);
      url += "&field5=" + String(totalMessagesTx);
      url += "&field6=" + String(totalMessagesRx);
      url += "&field7=" + String(WiFi.RSSI());
      url += "&field8=" + String(ESP.getFreeHeap());

      http.begin(url);
      // ThingSpeak works best with a keep-alive header set to false for simple updates
      http.addHeader("Connection", "close"); 
      
      int httpResponseCode = http.GET();
      
      if (httpResponseCode > 0) {
        Serial.printf("[ThingSpeak] Update Successful. Code: %d\n", httpResponseCode);
      } else {
        Serial.printf("[ThingSpeak] Update Failed. Error: %s\n", http.errorToString(httpResponseCode).c_str());
      }
      
      http.end(); // CRITICAL: This must be called to free up the socket for the next loop
    } else {
      if (WiFi.status() != WL_CONNECTED) Serial.println("[WIFI] Lost connection, waiting...");
    }

    // ThingSpeak Free Tier has a 15s limit. 16s is safer to avoid "403 Forbidden"
    vTaskDelay(pdMS_TO_TICKS(16000)); 
  }
}
void sendTelemetry(int bytes, uint32_t durationMs) {
  float speedBps = (bytes > 0 && durationMs > 0) ? ((bytes * 10.0) / (durationMs / 1000.0)) : 0;
  txLen = snprintf(txBuf, sizeof(txBuf), "ACK: %dB | %dms | %.1f bps", bytes, durationMs, speedBps);
  Serial.printf("[TX] Auto-Telemetry: \"%s\"\n", txBuf);
  txMState = TX_M_PREAMBLE; txPreambleCount = 0; txByteToSend = 0xAA;
  txBitStep = 0; txNextBitTime = millis();
}

void runLiFiTX() {
  if (txMState != TX_M_IDLE && millis() >= txNextBitTime) {
    if (txBitStep == 0) { digitalWrite(TX_PIN, LOW); txBitStep++; txNextBitTime += BIT_MS; }
    else if (txBitStep <= 8) {
      digitalWrite(TX_PIN, (txByteToSend & (1 << (txBitStep - 1))) ? HIGH : LOW);
      txBitStep++; txNextBitTime += BIT_MS;
    } else if (txBitStep == 9) { digitalWrite(TX_PIN, HIGH); txBitStep++; txNextBitTime += BIT_MS; }
    else if (txBitStep == 10) { 
        txBitStep = 0;
        if (txMState == TX_M_PREAMBLE) {
            if (++txPreambleCount < 8) txByteToSend = 0xAA;
            else { txMState = TX_M_DATA; txDataIndex = 0; txByteToSend = txBuf[0]; }
        } else if (txMState == TX_M_DATA) {
            if (++txDataIndex < txLen) txByteToSend = txBuf[txDataIndex];
            else { txMState = TX_M_NEWLINE; txByteToSend = '\n'; totalMessagesTx++;}
        } else { txMState = TX_M_IDLE; Serial.println("[TX] Sent."); }
    }
  }
}

void runLiFiRX() {
int raw = analogRead(RX_PIN);
  updateCalibration(raw);
  
  // Force print every 2 seconds regardless of state
  int level2 = getLogicLevel(raw); // Get the level first
  static uint32_t lastDebug = 0;
  if (millis() - lastDebug > 2000) {
    //Serial.printf("[DEBUG] Raw: %d | Thr: %d | Hys: %d | Level: %d | State: %d\n", 
                  //raw, threshold, hysteresis, level2, rxState);
    lastDebug = millis();
  }

  int level = getLogicLevel(raw);
  switch (rxState) {
    case WAIT_IDLE: calibFrozen = false; if (level == HIGH) rxState = WAIT_START; break;
    case WAIT_START: if (level == LOW) { rxNextTime = millis() + HALF_BIT_MS; rxState = WAIT_HALF; } break;
    case WAIT_HALF: if (millis() >= rxNextTime) {
        if (level == LOW) { calibFrozen = true; rxNextTime = millis() + BIT_MS; rxBitIndex = 0; rxByte = 0; rxState = RECV_BITS; }
        else rxState = WAIT_IDLE;
      } break;
    case RECV_BITS: if (millis() >= rxNextTime) {
        if (raw > threshold) rxByte |= (1 << rxBitIndex);
        rxBitIndex++; rxNextTime += BIT_MS;
        if (rxBitIndex == 8) rxState = RECV_STOP;
      } break;
    case RECV_STOP: if (millis() >= rxNextTime) {
        if (rxByte != 0xAA && rxByte != 0x55) {
          if (rxByte == '\n') { 
            rxBuf[rxLen] = '\0'; // Null-terminate the string
            
            // --- ADD THIS LINE TO PRINT TO SERIAL ---
            Serial.printf("\n[RECV] Final Message: \"%s\" (%d bytes)\n", rxBuf, rxLen);
            // -----------------------------------------

            sendTelemetry(rxLen, millis() - rxMessageStartMs); 
            rxLen = 0; 
            totalMessagesRx++;
          }
          else if (rxByte >= 0x20 && rxByte <= 0x7E) {
            if (rxLen == 0) rxMessageStartMs = millis();
            if (rxLen < sizeof(rxBuf) - 1) rxBuf[rxLen++] = (char)rxByte;
          }
        }
        calibFrozen = false; 
        rxState = WAIT_IDLE;
      } break;
  }
}

void txTask(void *p) { for(;;) { runLiFiTX(); vTaskDelay(1); } }
void rxTask(void *p) { for(;;) { runLiFiRX(); vTaskDelay(1); } }

void setup() {
  Serial.begin(115200);
  delay(1000); // Give the serial monitor a second to connect
  Serial.println("\n=== Node B Booting ===");

  pinMode(TX_PIN, OUTPUT); 
  pinMode(RX_PIN, INPUT);
  analogSetAttenuation(ADC_11db); 
  digitalWrite(TX_PIN, HIGH);
  
  // --- ADDED WIFI CONNECTION LOGIC ---
  Serial.print("[WIFI] Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[WIFI] Connected! IP: " + WiFi.localIP().toString());
  // -----------------------------------

  delay(500);

  // Add the ThingSpeak task
  xTaskCreatePinnedToCore(
    thingSpeakTask,    
    "ThingSpeak_Task", 
    8192,              
    NULL,              
    1,                 
    NULL,              
    0                  
  );

  // Create LiFi Tasks
  xTaskCreatePinnedToCore(txTask, "LiFi_TX", 8192, NULL, 10, NULL, 1);
  xTaskCreatePinnedToCore(rxTask, "LiFi_RX", 8192, NULL, 10, NULL, 1);
  
  Serial.println("=== Node B Ready ===");
}

void loop() { vTaskDelay(1000); }