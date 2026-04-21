// ==============================================================================
// NODE B: DATA RECEIVER & TELEMETRY TRANSMITTER
// ==============================================================================

const int TX_PIN = 26; // [cite: 1]
const int RX_PIN = 34; // [cite: 1]
const int BAUD_RATE = 20; // [cite: 2]
const int BIT_MS = 50; // [cite: 2]
const int HALF_BIT_MS = 25; // [cite: 3]

// Connection metrics
uint32_t rxMessageStartMs = 0;

// TX Non-Blocking State Machine Variables
char txBuf[128]; // [cite: 5]
int txLen = 0; // [cite: 6]
enum TxMachineState { TX_M_IDLE, TX_M_PREAMBLE, TX_M_DATA, TX_M_NEWLINE }; // [cite: 6]
TxMachineState txMState = TX_M_IDLE; // [cite: 6]
int txPreambleCount = 0; // [cite: 6]
int txDataIndex = 0; // [cite: 6]
int txBitStep = 0; // [cite: 7]
uint32_t txNextBitTime = 0; // [cite: 7]
byte txByteToSend = 0; // [cite: 7]

// RX State Machine Variables
char rxBuf[128]; // [cite: 8]
int rxLen = 0; // [cite: 8]
enum RxState { WAIT_IDLE, WAIT_START, WAIT_HALF, RECV_BITS, RECV_STOP }; // [cite: 8]
RxState rxState = WAIT_IDLE; // [cite: 9]
uint32_t rxNextTime = 0; // [cite: 9]
int rxBitIndex = 0; // [cite: 9]
byte rxByte = 0; // [cite: 9]

// Calibration Variables
int adcMin = 4095; // [cite: 10]
int adcMax = 0; // [cite: 10]
int threshold = 2047; // [cite: 10]
int hysteresis = 10; // [cite: 10]
bool calibFrozen = false; // [cite: 11]

// ==============================================================================
// CALIBRATION & LOGIC HELPERS
// ==============================================================================

void updateCalibration(int raw) {
  if (calibFrozen) return;
  if (raw < adcMin) adcMin = raw; // [cite: 12]
  if (raw > adcMax) adcMax = raw; // [cite: 12]
  
  int swing = adcMax - adcMin; // [cite: 12]
  if (swing >= 50) { // [cite: 13]
    threshold = adcMin + (swing / 2); // [cite: 13]
    hysteresis = max(10, swing / 6); // [cite: 14]
  }
}

int getLogicLevel(int raw) {
  if (raw > threshold + hysteresis) return HIGH; // [cite: 14]
  if (raw < threshold - hysteresis) return LOW; // [cite: 15]
  return -1; // [cite: 15]
}

// ==============================================================================
// STARTUP MONITOR
// ==============================================================================

void startupMonitor() {
  Serial.println("\nStarting ADC Monitor for 10s...");
  uint32_t startMs = millis(); // [cite: 17]
  
  while (millis() - startMs < 10000) { // [cite: 17]
    int raw = analogRead(RX_PIN);
    updateCalibration(raw);

    int bars = map(raw, 0, 4095, 0, 20); // [cite: 18]
    bars = constrain(bars, 0, 20); // [cite: 19]
    char barStr[21]; // [cite: 19]
    for (int i = 0; i < 20; i++) {
      barStr[i] = (i < bars) ? '|' : ' '; // [cite: 19, 20]
    }
    barStr[20] = '\0'; // [cite: 20]

    Serial.printf("ADC=%-4d thr=%-4d hys=%-2d [%s]\n", raw, threshold, hysteresis, barStr); // [cite: 20]
    delay(50); // [cite: 20]
  }

  int swing = adcMax - adcMin; // [cite: 21]
  Serial.printf("\nCalibration Summary: range=%d-%d swing=%d thr=%d hys=%d\n\n", adcMin, adcMax, swing, threshold, hysteresis); // [cite: 22]
}

// ==============================================================================
// TRANSMITTER LOGIC (Automated for Telemetry)
// ==============================================================================

void sendTelemetry(int bytes, uint32_t durationMs) {
  char tBuf[64];
  // Calculate speed: 10 bits per byte transmitted
  float speedBps = (bytes > 0 && durationMs > 0) ? ((bytes * 10.0) / (durationMs / 1000.0)) : 0;
  snprintf(tBuf, sizeof(tBuf), "ACK: %dB | %dms | %.1f bps", bytes, durationMs, speedBps);
  
  memcpy(txBuf, tBuf, strlen(tBuf));
  txLen = strlen(tBuf);
  txBuf[txLen] = '\0';

  Serial.printf("[TX] Auto-Sending Telemetry: \"%s\"\n", txBuf);

  txMState = TX_M_PREAMBLE; // [cite: 26]
  txPreambleCount = 0; // [cite: 26]
  txByteToSend = 0xAA; // [cite: 26]
  txBitStep = 0; // [cite: 26]
  txNextBitTime = millis(); // [cite: 26]
}

void advanceTxByte() {
  if (txMState == TX_M_PREAMBLE) {
    txPreambleCount++; // [cite: 26]
    if (txPreambleCount < 8) { // [cite: 27]
      txByteToSend = 0xAA; // [cite: 27]
    } else {
      txMState = TX_M_DATA; // [cite: 28]
      txDataIndex = 0; // [cite: 28]
      txByteToSend = (txLen > 0) ? txBuf[txDataIndex] : '\n'; // [cite: 28, 29]
      if (txLen == 0) txMState = TX_M_NEWLINE; // [cite: 29]
    }
  } else if (txMState == TX_M_DATA) {
    txDataIndex++; // [cite: 30]
    if (txDataIndex < txLen) { // [cite: 31]
      txByteToSend = txBuf[txDataIndex]; // [cite: 31]
    } else {
      txMState = TX_M_NEWLINE; // [cite: 32]
      txByteToSend = '\n'; // [cite: 32]
    }
  } else if (txMState == TX_M_NEWLINE) {
    txMState = TX_M_IDLE; // [cite: 33]
    Serial.println("[TX] Telemetry Sent.\n"); // [cite: 33]
  }
}

void handleTX() {
  // Automated Bit-banging State Machine (No user input)
  if (txMState != TX_M_IDLE) {
    if (millis() >= txNextBitTime) { // [cite: 42]
      if (txBitStep == 0) { // [cite: 42]
        digitalWrite(TX_PIN, LOW); // [cite: 42]
        txBitStep++; // [cite: 43]
        txNextBitTime += BIT_MS; // [cite: 43]
      } else if (txBitStep >= 1 && txBitStep <= 8) { // [cite: 44]
        int bitVal = (txByteToSend & (1 << (txBitStep - 1))) ? HIGH : LOW; // [cite: 44, 45]
        digitalWrite(TX_PIN, bitVal); // [cite: 45]
        txBitStep++; // [cite: 45]
        txNextBitTime += BIT_MS; // [cite: 45]
      } else if (txBitStep == 9) { // [cite: 45]
        digitalWrite(TX_PIN, HIGH); // [cite: 45]
        txBitStep++; // [cite: 46]
        txNextBitTime += BIT_MS; // [cite: 46]
      } else if (txBitStep == 10) { // [cite: 47]
        txBitStep = 0; // [cite: 47]
        advanceTxByte(); // [cite: 47]
      }
    }
  }
}

// ==============================================================================
// RECEIVER LOGIC
// ==============================================================================

void handleRX() {
  int raw = analogRead(RX_PIN);
  updateCalibration(raw); // [cite: 49]
  int level = getLogicLevel(raw); // [cite: 49]

  switch (rxState) {
    case WAIT_IDLE:
      calibFrozen = false; // [cite: 49]
      if (level == HIGH) rxState = WAIT_START; // [cite: 50]
      break;

    case WAIT_START:
      if (level == LOW) { // [cite: 50]
        rxNextTime = millis() + HALF_BIT_MS; // [cite: 50]
        rxState = WAIT_HALF; // [cite: 51]
      }
      break;
    case WAIT_HALF: // [cite: 52]
      if (millis() >= rxNextTime) { // [cite: 52]
        if (level == LOW) {  // [cite: 52]
          calibFrozen = true; // [cite: 52]
          rxNextTime = millis() + BIT_MS; // [cite: 53]
          rxBitIndex = 0; // [cite: 53]
          rxByte = 0; // [cite: 53]
          rxState = RECV_BITS; // [cite: 53]
        } else {
          rxState = WAIT_IDLE; // [cite: 54]
        }
      }
      break;
    case RECV_BITS: // [cite: 56]
      if (millis() >= rxNextTime) { // [cite: 56]
        int bitVal = (raw > threshold) ? 1 : 0; // [cite: 56, 57]
        if (bitVal) rxByte |= (1 << rxBitIndex); // [cite: 57]
        
        rxBitIndex++; // [cite: 58]
        rxNextTime += BIT_MS; // [cite: 58]
        if (rxBitIndex == 8) rxState = RECV_STOP; // [cite: 59]
      }
      break;
    case RECV_STOP: // [cite: 60]
      if (millis() >= rxNextTime) { // [cite: 60]
        if (rxByte == 0xAA || rxByte == 0x55) { // [cite: 60]
          // Discard preamble
        } else if (rxByte == '\n') { // [cite: 60]
          rxBuf[rxLen] = '\0'; // [cite: 60]
          
          uint32_t duration = millis() - rxMessageStartMs;
          
          Serial.printf("\n[RX] Message Received: \"%s\"\n", rxBuf); // [cite: 61]
          
          // Trigger Telemetry Transmission back to A
          sendTelemetry(rxLen, duration); 
          
          rxLen = 0; // [cite: 61]
        } else if (rxByte >= 0x20 && rxByte <= 0x7E) { // [cite: 62]
          if (rxLen == 0) {
             rxMessageStartMs = millis(); // Mark time when first data byte arrives
          }
          if (rxLen < sizeof(rxBuf) - 1) rxBuf[rxLen++] = (char)rxByte; // [cite: 62]
        }
        calibFrozen = false; // [cite: 63]
        rxState = WAIT_IDLE; // [cite: 63]
      }
      break;
  }
}

// ==============================================================================
// MAIN SETUP & LOOP
// ==============================================================================

void setup() {
  Serial.begin(115200); // [cite: 64]
  
  pinMode(TX_PIN, OUTPUT); // [cite: 64]
  pinMode(RX_PIN, INPUT); // [cite: 65]
  
  analogSetAttenuation(ADC_11db); // [cite: 65]
  digitalWrite(TX_PIN, HIGH); // [cite: 65]

  delay(1500); // [cite: 65]
  while(Serial.available()) Serial.read(); // [cite: 65]

  startupMonitor(); // [cite: 65]

  Serial.println("=== Node B: Data RX & Telemetry TX | GPIO26=TX GPIO34=RX ===");
}

void loop() {
  handleTX();
  handleRX();
}