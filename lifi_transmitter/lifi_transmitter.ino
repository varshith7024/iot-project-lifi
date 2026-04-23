// ==============================================================================
// NODE A: DATA TRANSMITTER & TELEMETRY RECEIVER (FreeRTOS Fixed)
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
String apiKey = "HETB3NQ3IGLB9RZU";

const int TX_PIN = 25; // [cite: 90]
const int RX_PIN = 34; // [cite: 90]
const int BIT_MS = 50; // 
const int HALF_BIT_MS = 25; // [cite: 92]

WiFiClient espClient;
PubSubClient client(espClient);

// Add these to track total throughput
volatile int totalMessagesTx = 0;
volatile int totalMessagesRx = 0;

// Global Buffers & States
char inputBuf[128]; 
int inputLen = 0; 
uint32_t lastCharTime = 0;
char txBuf[128]; 
int txLen = 0;
enum TxMachineState { TX_M_IDLE, TX_M_PREAMBLE, TX_M_DATA, TX_M_NEWLINE };
TxMachineState txMState = TX_M_IDLE;
int txPreambleCount = 0; 
int txDataIndex = 0; 
int txBitStep = 0;
uint32_t txNextBitTime = 0; 
byte txByteToSend = 0; 

char rxBuf[128];
int rxLen = 0; 
enum RxState { WAIT_IDLE, WAIT_START, WAIT_HALF, RECV_BITS, RECV_STOP };
RxState rxState = WAIT_IDLE; 
uint32_t rxNextTime = 0; 
int rxBitIndex = 0;
byte rxByte = 0; 

int adcMin = 4095; 
int adcMax = 0;
int threshold = 2047; // [cite: 100]
int hysteresis = 10; 
bool calibFrozen = false;

// --- Logic Helpers (unchanged from original) ---
void updateCalibration(int raw) {
  if (calibFrozen) return;
  if (raw < adcMin) adcMin = raw; 
  if (raw > adcMax) adcMax = raw;
  int swing = adcMax - adcMin;
  if (swing >= 50) { 
    threshold = adcMin + (swing / 2); // [cite: 105]
    hysteresis = max(10, swing / 6);
  }
}

int getLogicLevel(int raw) {
  if (raw > threshold + hysteresis) return HIGH; // [cite: 107]
  if (raw < threshold - hysteresis) return LOW; 
  return -1;
}

void thingSpeakTask(void *pvParameters) {
  for (;;) {
    // Only attempt if WiFi is up and LiFi is not busy
    if (WiFi.status() == WL_CONNECTED) {
      
      HTTPClient http;
      String url = String(serverName) + "?api_key=" + apiKey;
      url += "&field1=" + String(analogRead(RX_PIN));
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
        //Serial.printf("[ThingSpeak] Update Successful. Code: %d\n", httpResponseCode);
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

// --- Original Monitor Logic ---
void startupMonitor() {
  Serial.println("\nStarting ADC Monitor for 10s (press any key to skip)...");
  uint32_t startMs = millis(); 
  while (millis() - startMs < 10000) { 
    if (Serial.available()) { while (Serial.available()) Serial.read(); break; }
    int raw = analogRead(RX_PIN);
    updateCalibration(raw);
    int bars = constrain(map(raw, 0, 4095, 0, 20), 0, 20);
    char barStr[21]; 
    for (int i = 0; i < 20; i++) barStr[i] = (i < bars) ? '|' : ' '; 
    barStr[20] = '\0';
    Serial.printf("ADC=%-4d thr=%-4d hys=%-2d [%s]\n", raw, threshold, hysteresis, barStr);
    delay(50); 
  }
  Serial.printf("\nCalibration Summary: range=%d-%d swing=%d thr=%d hys=%d\n\n", adcMin, adcMax, adcMax-adcMin, threshold, hysteresis);
}

// --- LiFi Protocol Logic ---
void startTransmission() {
  memcpy(txBuf, inputBuf, inputLen);
  txLen = inputLen; 
  inputLen = 0;
  inputBuf[0] = '\0'; 
  Serial.printf("\n[TX] Sending: \"%s\"\n", txBuf);
  txMState = TX_M_PREAMBLE; 
  txPreambleCount = 0;
  txByteToSend = 0xAA; 
  txBitStep = 0;
  txNextBitTime = millis(); 
}

void advanceTxByte() {
  if (txMState == TX_M_PREAMBLE) {
    if (++txPreambleCount < 8) txByteToSend = 0xAA;
    else {
      txMState = TX_M_DATA; txDataIndex = 0;
      txByteToSend = (txLen > 0) ? txBuf[txDataIndex] : '\n';
      if (txLen == 0) txMState = TX_M_NEWLINE;
    }
  } else if (txMState == TX_M_DATA) {
    if (++txDataIndex < txLen) txByteToSend = txBuf[txDataIndex];
    else { txMState = TX_M_NEWLINE; txByteToSend = '\n'; }
  } else if (txMState == TX_M_NEWLINE) {
    txMState = TX_M_IDLE;
    totalMessagesTx++;
    Serial.println("\n[TX] Done.\n> ");
  }
}

// Task for Bit-Banging the LED [cite: 147]
void runLiFiTX() {
  if (txMState != TX_M_IDLE && millis() >= txNextBitTime) {
    if (txBitStep == 0) { digitalWrite(TX_PIN, LOW); txBitStep++; txNextBitTime += BIT_MS; }
    else if (txBitStep <= 8) {
      digitalWrite(TX_PIN, (txByteToSend & (1 << (txBitStep - 1))) ? HIGH : LOW);
      txBitStep++; txNextBitTime += BIT_MS;
    } else if (txBitStep == 9) { digitalWrite(TX_PIN, HIGH); txBitStep++; txNextBitTime += BIT_MS; }
    else if (txBitStep == 10) { txBitStep = 0; advanceTxByte(); }
  }
}

// Task for reading the LDR [cite: 159]
void runLiFiRX() {
  int raw = analogRead(RX_PIN);
  updateCalibration(raw); 
  int level = getLogicLevel(raw); 
  switch (rxState) {
    case WAIT_IDLE: calibFrozen = false; if (level == HIGH) rxState = WAIT_START; break;
    case WAIT_START: if (level == LOW) { rxNextTime = millis() + HALF_BIT_MS; rxState = WAIT_HALF; } break;
    case WAIT_HALF: if (millis() >= rxNextTime) { 
        if (level == LOW) { calibFrozen = true; rxNextTime = millis() + BIT_MS; rxBitIndex = 0; rxByte = 0; rxState = RECV_BITS; }
        else rxState = WAIT_IDLE;
      } break;
    case RECV_BITS: if (millis() >= rxNextTime) {
        if ((raw > threshold)) rxByte |= (1 << rxBitIndex);
        rxBitIndex++; rxNextTime += BIT_MS;
        if (rxBitIndex == 8) rxState = RECV_STOP;
      } break;
    case RECV_STOP: if (millis() >= rxNextTime) {
        if (rxByte != 0xAA && rxByte != 0x55) {
          if (rxByte == '\n') { rxBuf[rxLen] = '\0'; Serial.printf("\n[TELEMETRY] Node B: \"%s\"\n> %s", rxBuf, inputBuf); rxLen = 0; totalMessagesRx++; }
          else if (rxByte >= 0x20 && rxByte <= 0x7E && rxLen < sizeof(rxBuf)-1) rxBuf[rxLen++] = (char)rxByte;
        }
        calibFrozen = false; rxState = WAIT_IDLE;
      } break;
  }
}

// --- FreeRTOS Tasks ---
void txTask(void *p) { for(;;) { runLiFiTX(); vTaskDelay(1); } }
void rxTask(void *p) { for(;;) { runLiFiRX(); vTaskDelay(1); } }

void setup() {
  Serial.begin(115200);
  pinMode(TX_PIN, OUTPUT); pinMode(RX_PIN, INPUT);
  analogSetAttenuation(ADC_11db); digitalWrite(TX_PIN, HIGH);
  delay(1500);

  // --- ADD THIS WIFI BLOCK ---
  Serial.print("\n[WIFI] Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  
  // Optional: Wait for connection here so you don't start tasks without internet
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[WIFI] Connected! IP: " + WiFi.localIP().toString());
  // ---------------------------

  startupMonitor();

  xTaskCreatePinnedToCore(thingSpeakTask, "ThingSpeak_Task", 8192, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(txTask, "LiFi_TX", 8192, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(rxTask, "LiFi_RX", 8192, NULL, 3, NULL, 1);
  
  Serial.print("=== Node A Ready ===\n> ");
}

void loop() {
  // Handle Serial Input here to keep the Serial Monitor responsive 
  if (Serial.available()) {
    char c = Serial.read();
    if ((c == '\n' || c == '\r') && inputLen > 0 && txMState == TX_M_IDLE) startTransmission();
    else if ((c == 8 || c == 127) && inputLen > 0) { inputLen--; inputBuf[inputLen] = '\0'; Serial.print("\b \b"); }
    else if (c >= 0x20 && c <= 0x7E && inputLen < sizeof(inputBuf)-1) {
      inputBuf[inputLen++] = c; inputBuf[inputLen] = '\0';
      Serial.print(c); lastCharTime = millis();
    }
  }
  if (inputLen > 0 && millis() - lastCharTime > 800 && txMState == TX_M_IDLE) startTransmission();
  vTaskDelay(10); // Small delay to let background tasks breathe
}