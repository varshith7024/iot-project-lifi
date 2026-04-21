// ==============================================================================
// NODE A: DATA TRANSMITTER & TELEMETRY RECEIVER
// ==============================================================================

const int TX_PIN = 25; // [cite: 67]
const int RX_PIN = 35; // [cite: 67]
const int BAUD_RATE = 20; // [cite: 68]
const int BIT_MS = 50; // [cite: 69]
const int HALF_BIT_MS = 25; // [cite: 70]

// User Input Variables
char inputBuf[128]; // [cite: 70]
int inputLen = 0; // [cite: 70]
uint32_t lastCharTime = 0; // [cite: 71]

// TX Non-Blocking State Machine Variables
char txBuf[128]; // [cite: 71]
int txLen = 0; // [cite: 71]
enum TxMachineState { TX_M_IDLE, TX_M_PREAMBLE, TX_M_DATA, TX_M_NEWLINE }; // [cite: 72]
TxMachineState txMState = TX_M_IDLE; // [cite: 72]
int txPreambleCount = 0; // [cite: 72]
int txDataIndex = 0; // [cite: 72]
int txBitStep = 0; // [cite: 73]
uint32_t txNextBitTime = 0; // [cite: 73]
byte txByteToSend = 0; // [cite: 73]

// RX State Machine Variables
char rxBuf[128]; // [cite: 74]
int rxLen = 0; // [cite: 74]
enum RxState { WAIT_IDLE, WAIT_START, WAIT_HALF, RECV_BITS, RECV_STOP }; // [cite: 74]
RxState rxState = WAIT_IDLE; // [cite: 75]
uint32_t rxNextTime = 0; // [cite: 75]
int rxBitIndex = 0; // [cite: 75]
byte rxByte = 0; // [cite: 75]

// Calibration Variables
int adcMin = 4095; // [cite: 76]
int adcMax = 0; // [cite: 76]
int threshold = 2047; // [cite: 76]
int hysteresis = 10; // [cite: 76]
bool calibFrozen = false; // [cite: 77]

// ==============================================================================
// CALIBRATION & LOGIC HELPERS
// ==============================================================================

void updateCalibration(int raw) {
  if (calibFrozen) return;
  if (raw < adcMin) adcMin = raw; // [cite: 78]
  if (raw > adcMax) adcMax = raw; // [cite: 78]
  
  int swing = adcMax - adcMin; // [cite: 78]
  if (swing >= 50) { // [cite: 79]
    threshold = adcMin + (swing / 2); // [cite: 79]
    hysteresis = max(10, swing / 6); // [cite: 80]
  }
}

int getLogicLevel(int raw) {
  if (raw > threshold + hysteresis) return HIGH; // [cite: 80]
  if (raw < threshold - hysteresis) return LOW; // [cite: 81]
  return -1; // [cite: 81]
}

// ==============================================================================
// STARTUP MONITOR
// ==============================================================================

void startupMonitor() {
  Serial.println("\nStarting ADC Monitor for 10s (press any key to skip)...");
  uint32_t startMs = millis(); // [cite: 83]
  
  while (millis() - startMs < 10000) { // [cite: 83]
    if (Serial.available()) {
      while (Serial.available()) Serial.read(); // [cite: 83]
      break; // [cite: 84]
    }

    int raw = analogRead(RX_PIN);
    updateCalibration(raw);

    int bars = map(raw, 0, 4095, 0, 20); // [cite: 84]
    bars = constrain(bars, 0, 20); // [cite: 85]
    char barStr[21]; // [cite: 85]
    for (int i = 0; i < 20; i++) {
      barStr[i] = (i < bars) ? '|' : ' '; // [cite: 85, 86]
    }
    barStr[20] = '\0'; // [cite: 86]

    Serial.printf("ADC=%-4d thr=%-4d hys=%-2d [%s]\n", raw, threshold, hysteresis, barStr); // [cite: 86]
    delay(50); // [cite: 86]
  }

  int swing = adcMax - adcMin; // [cite: 87]
  Serial.printf("\nCalibration Summary: range=%d-%d swing=%d thr=%d hys=%d\n", adcMin, adcMax, swing, threshold, hysteresis); // [cite: 88]
  if (swing < 50) { // [cite: 89]
    Serial.println("WARNING: Signal swing < 50 counts. Move LEDs closer or block ambient light!"); // [cite: 89]
  }
  Serial.println();
}

// ==============================================================================
// TRANSMITTER LOGIC
// ==============================================================================

void startTransmission() {
  memcpy(txBuf, inputBuf, inputLen); // [cite: 90]
  txLen = inputLen; // [cite: 90]
  inputLen = 0; // [cite: 91]
  inputBuf[0] = '\0'; // [cite: 91]

  Serial.println(); // [cite: 91]
  Serial.printf("[TX] Sending: \"%s\"\n", txBuf); // [cite: 91]

  txMState = TX_M_PREAMBLE; // [cite: 91]
  txPreambleCount = 0; // [cite: 91]
  txByteToSend = 0xAA; // [cite: 91]
  txBitStep = 0; // [cite: 92]
  txNextBitTime = millis(); // [cite: 92]
}

void advanceTxByte() {
  if (txMState == TX_M_PREAMBLE) {
    txPreambleCount++; // [cite: 92]
    if (txPreambleCount < 8) { // [cite: 93]
      txByteToSend = 0xAA; // [cite: 93]
    } else {
      txMState = TX_M_DATA; // [cite: 94]
      txDataIndex = 0; // [cite: 94]
      txByteToSend = (txLen > 0) ? txBuf[txDataIndex] : '\n'; // [cite: 94, 95]
      if (txLen == 0) txMState = TX_M_NEWLINE; // [cite: 95]
    }
  } else if (txMState == TX_M_DATA) {
    txDataIndex++; // [cite: 96]
    if (txDataIndex < txLen) { // [cite: 97]
      txByteToSend = txBuf[txDataIndex]; // [cite: 97]
    } else {
      txMState = TX_M_NEWLINE; // [cite: 98]
      txByteToSend = '\n'; // [cite: 98]
    }
  } else if (txMState == TX_M_NEWLINE) {
    txMState = TX_M_IDLE; // [cite: 99]
    Serial.println("\n[TX] Done."); // [cite: 99]
    Serial.print("> "); // [cite: 99]
    if (inputLen > 0) Serial.print(inputBuf); // [cite: 100]
  }
}

void handleTX() {
  if (Serial.available()) {
    char c = Serial.read(); // [cite: 100]
    if (c == '\n' || c == '\r') {
      if (inputLen > 0 && txMState == TX_M_IDLE) { // [cite: 104]
        startTransmission(); // [cite: 104]
      }
    } else if (c == 8 || c == 127) { // [cite: 105]
      if (inputLen > 0) { // [cite: 105]
        inputLen--; // [cite: 105]
        inputBuf[inputLen] = '\0'; // [cite: 106]
        Serial.print("\b \b");  // [cite: 106]
      }
    } else if (c >= 0x20 && c <= 0x7E) { // [cite: 106]
      if (inputLen < sizeof(inputBuf) - 1) { // [cite: 106]
        inputBuf[inputLen++] = c; // [cite: 106]
        inputBuf[inputLen] = '\0'; // [cite: 107]
        Serial.print(c); // [cite: 107]
        lastCharTime = millis(); // [cite: 107]
      }
    }
  }

  if (inputLen > 0 && millis() - lastCharTime > 800 && txMState == TX_M_IDLE) { // [cite: 107]
    startTransmission(); // [cite: 107]
  }

  if (txMState != TX_M_IDLE) {
    if (millis() >= txNextBitTime) { // [cite: 108]
      if (txBitStep == 0) { // [cite: 108]
        digitalWrite(TX_PIN, LOW); // [cite: 108]
        txBitStep++; // [cite: 109]
        txNextBitTime += BIT_MS; // [cite: 109]
      } else if (txBitStep >= 1 && txBitStep <= 8) { // [cite: 110]
        int bitVal = (txByteToSend & (1 << (txBitStep - 1))) ? HIGH : LOW; // [cite: 110, 111]
        digitalWrite(TX_PIN, bitVal); // [cite: 111]
        txBitStep++; // [cite: 111]
        txNextBitTime += BIT_MS; // [cite: 111]
      } else if (txBitStep == 9) { // [cite: 111]
        digitalWrite(TX_PIN, HIGH); // [cite: 111]
        txBitStep++; // [cite: 112]
        txNextBitTime += BIT_MS; // [cite: 112]
      } else if (txBitStep == 10) { // [cite: 113]
        txBitStep = 0; // [cite: 113]
        advanceTxByte(); // [cite: 113]
      }
    }
  }
}

// ==============================================================================
// RECEIVER LOGIC
// ==============================================================================

void handleRX() {
  int raw = analogRead(RX_PIN);
  updateCalibration(raw); // [cite: 115]
  int level = getLogicLevel(raw); // [cite: 115]

  switch (rxState) {
    case WAIT_IDLE:
      calibFrozen = false; // [cite: 115]
      if (level == HIGH) rxState = WAIT_START; // [cite: 116]
      break;

    case WAIT_START:
      if (level == LOW) { // [cite: 116]
        rxNextTime = millis() + HALF_BIT_MS; // [cite: 116]
        rxState = WAIT_HALF; // [cite: 117]
      }
      break;
    case WAIT_HALF: // [cite: 118]
      if (millis() >= rxNextTime) { // [cite: 118]
        if (level == LOW) {  // [cite: 118]
          calibFrozen = true; // [cite: 118]
          rxNextTime = millis() + BIT_MS; // [cite: 119]
          rxBitIndex = 0; // [cite: 119]
          rxByte = 0; // [cite: 119]
          rxState = RECV_BITS; // [cite: 119]
        } else {
          rxState = WAIT_IDLE; // [cite: 120]
        }
      }
      break;
    case RECV_BITS: // [cite: 122]
      if (millis() >= rxNextTime) { // [cite: 122]
        int bitVal = (raw > threshold) ? 1 : 0; // [cite: 122, 123]
        if (bitVal) rxByte |= (1 << rxBitIndex); // [cite: 123]
        
        rxBitIndex++; // [cite: 124]
        rxNextTime += BIT_MS; // [cite: 124]
        if (rxBitIndex == 8) rxState = RECV_STOP; // [cite: 125]
      }
      break;
    case RECV_STOP: // [cite: 126]
      if (millis() >= rxNextTime) { // [cite: 126]
        if (rxByte == 0xAA || rxByte == 0x55) { // [cite: 126]
          // Discard preamble
        } else if (rxByte == '\n') { // [cite: 126]
          rxBuf[rxLen] = '\0'; // [cite: 126]
          Serial.println(); // [cite: 127]
          // UPDATED: Now identifies incoming text as Telemetry from B
          Serial.printf("[TELEMETRY] Node B: \"%s\"\n", rxBuf); 
          Serial.print("> "); // [cite: 127]
          if (inputLen > 0) Serial.print(inputBuf); // [cite: 127]
          rxLen = 0; // [cite: 127]
        } else if (rxByte >= 0x20 && rxByte <= 0x7E) { // [cite: 128]
          if (rxLen < sizeof(rxBuf) - 1) rxBuf[rxLen++] = (char)rxByte; // [cite: 128]
        }
        calibFrozen = false; // [cite: 129]
        rxState = WAIT_IDLE; // [cite: 129]
      }
      break;
  }
}

// ==============================================================================
// MAIN SETUP & LOOP
// ==============================================================================

void setup() {
  Serial.begin(115200); // [cite: 130]
  
  pinMode(TX_PIN, OUTPUT); // [cite: 130]
  pinMode(RX_PIN, INPUT); // [cite: 131]
  
  analogSetAttenuation(ADC_11db); // [cite: 131]
  digitalWrite(TX_PIN, HIGH); // [cite: 131]

  delay(1500); // [cite: 131]
  while(Serial.available()) Serial.read(); // [cite: 131]

  startupMonitor(); // [cite: 131]

  Serial.println("=== Node A: Data TX & Telemetry RX | GPIO25=TX GPIO35=RX ===");
  Serial.print("> "); // [cite: 132]
}

void loop() {
  handleTX();
  handleRX();
}