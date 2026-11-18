#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Keypad.h>

// SMART LAB SECURITY V4 - TAHAP 9B
// Prototipe demonstrasi: jangan gunakan sebagai satu-satunya proteksi pintu nyata.
// Arduino-ESP32 Core 3.x; library PubSubClient, MFRC522, Keypad.
// Kredensial: isi secara lokal, JANGAN push firmware berisi password ke GitHub.

// ===== JARINGAN =====
const char *WIFI_SSID = "YOUR_WIFI_SSID";
const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char *MQTT_HOST = "YOUR_BROKER_IP"; // IP laptop/server MQTT
const uint16_t MQTT_PORT = 1884;
const char *MQTT_USER = "smartlab";
const char *MQTT_PASSWORD = "YOUR_MQTT_PASSWORD";

const char *TOPIC_TELEMETRY = "smartlab/security/hardware/telemetry";
const char *TOPIC_HEARTBEAT = "smartlab/security/hardware/heartbeat";
const char *TOPIC_EVENT = "smartlab/security/hardware/event";

// ===== INPUT =====
constexpr uint8_t RFID_SS = 10;
constexpr uint8_t RFID_RST = 9;
constexpr uint8_t RFID_SCK = 12;
constexpr uint8_t RFID_MOSI = 11;
constexpr uint8_t RFID_MISO = 13;
constexpr uint8_t DOOR_PIN = 21; // LOW=OPEN, tombol ke GND
constexpr uint8_t PIR_PIN = 38;

// ===== OUTPUT =====
constexpr uint8_t SERVO_PIN = 40;
constexpr uint8_t LED_GREEN = 1;
constexpr uint8_t LED_RED = 2;
constexpr uint8_t RELAY_PIN = 41;
constexpr uint8_t BUZZER_PIN = 42; // melalui driver transistor

// Pengujian relay 9A sebelumnya harus sesuai konfigurasi ini.
constexpr bool RELAY_ACTIVE_LOW = true;
constexpr uint32_t SERVO_FREQ = 50;
constexpr uint8_t SERVO_RES = 14;
constexpr uint16_t SERVO_LOCK_US = 1200;
constexpr uint16_t SERVO_UNLOCK_US = 1800;
constexpr uint32_t UNLOCK_TIMEOUT_MS = 10000;
constexpr uint32_t PIR_WARMUP_MS = 60000;
constexpr uint32_t BUZZER_ON_MS = 250;
constexpr uint32_t BUZZER_PERIOD_MS = 1200;

// Ganti UID dengan kartu resmi uji. UID bukan faktor autentikasi yang kuat.
const char *VALID_UID = "0D:B3:F3:80";
// Ganti dengan PIN lokal minimal 6 digit; contoh sengaja tidak disertakan.
const char *VALID_PIN = "232323";

const byte ROWS = 4, COLS = 4;
char keymap[ROWS][COLS] = {
  {'1','2','3','A'}, {'4','5','6','B'},
  {'7','8','9','C'}, {'*','0','#','D'}
};
byte rowPins[ROWS] = {4,5,6,7};
byte colPins[COLS] = {15,16,17,18};
Keypad keypad = Keypad(makeKeymap(keymap), rowPins, colPins, ROWS, COLS);
MFRC522 rfid(RFID_SS, RFID_RST);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// ===== STATUS =====
enum LocalAction : uint8_t { ACCESS=0, RESET_ALARM, SILENCE_ALARM, TOGGLE_MODE };
LocalAction localAction = ACCESS;
String pinBuffer;
String lastAccess = "NONE";
String alarmReason;
String mode = "NORMAL";
bool servoReady = false, buzzerReady = false;
bool unlocked = false, openedAfterUnlock = false;
bool alarmActive = false, buzzerSilenced = false;
bool pirReady = false, doorStableOpen = false, doorCandidateOpen = false;
bool doorInitialized = false;
bool buzzerOn = false;
uint8_t failedRFID = 0, failedPIN = 0;
uint32_t unlockedAt = 0, pirStart = 0, doorCandidateSince = 0;
uint32_t lastKeyTime = 0, lastCardRead = 0;
uint32_t lastTelemetry = 0, lastHeartbeat = 0;
uint32_t lastWiFiAttempt = 0, lastMQTTAttempt = 0;
uint32_t buzzerCycleStart = 0;
constexpr uint32_t PIN_TIMEOUT_MS = 15000;

bool doorOpenRaw() { return digitalRead(DOOR_PIN) == LOW; }
bool motionDetected() { return pirReady && digitalRead(PIR_PIN) == HIGH; }

void relaySet(bool on) {
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? (on ? LOW : HIGH) : (on ? HIGH : LOW));
}
void buzzerSet(bool on) {
  if (!buzzerReady) return;
  if (buzzerOn == on) return;
  buzzerOn = on;
  ledcWriteTone(BUZZER_PIN, on ? 2000 : 0);
}
void updateOutputs() {
  digitalWrite(LED_GREEN, alarmActive ? LOW : HIGH);
  digitalWrite(LED_RED, alarmActive ? HIGH : LOW);
  relaySet(alarmActive);
  if (!alarmActive || buzzerSilenced) {
    buzzerSet(false);
    return;
  }
  // Bunyi periodik untuk mengurangi beban buzzer.
  uint32_t phase = (millis() - buzzerCycleStart) % BUZZER_PERIOD_MS;
  buzzerSet(phase < BUZZER_ON_MS);
}

void publishEvent(const char *type, const char *detail) {
  // type/detail hanya konstanta internal; tidak mengirim UID/PIN.
  char payload[256];
  snprintf(payload, sizeof(payload),
    "{\"source\":\"HARDWARE\",\"device\":\"ESP32-S3\",\"type\":\"%s\",\"detail\":\"%s\"}",
    type, detail);
  if (mqtt.connected()) mqtt.publish(TOPIC_EVENT, payload, false);
  Serial.printf("EVENT: %s | %s\n", type, detail);
}
void triggerAlarm(const char *reason) {
  if (alarmActive) return;
  alarmActive = true;
  buzzerSilenced = false;
  alarmReason = reason;
  buzzerCycleStart = millis();
  updateOutputs();
  publishEvent("ALARM_TRIGGERED", reason);
}

bool servoPulse(uint16_t pulseUs) {
  if (!servoReady) return false;
  const uint32_t maxDuty = (1UL << SERVO_RES) - 1UL;
  return ledcWrite(SERVO_PIN, (uint32_t)pulseUs * maxDuty / 20000UL);
}
void lockDoor(const char *reason) {
  // DILARANG menggerakkan pengunci saat sensor pintu OPEN.
  if (doorStableOpen || doorOpenRaw()) {
    publishEvent("LOCK_BLOCKED", "Door open");
    return;
  }
  if (!servoPulse(SERVO_LOCK_US)) {
    publishEvent("ACTUATOR_ERROR", "Lock PWM failed");
    return;
  }
  bool wasUnlocked = unlocked;
  unlocked = false;
  openedAfterUnlock = false;
  unlockedAt = 0;
  if (wasUnlocked) publishEvent("DOOR_LOCKED", reason);
}
void unlockDoor(const char *method) {
  if (alarmActive || doorStableOpen || doorOpenRaw()) {
    publishEvent("ACCESS_BLOCKED", "Alarm or door open");
    return;
  }
  if (!servoPulse(SERVO_UNLOCK_US)) {
    publishEvent("ACTUATOR_ERROR", "Unlock PWM failed");
    return;
  }
  unlocked = true;
  unlockedAt = millis();
  openedAfterUnlock = false;
  lastAccess = method;
  failedRFID = 0;
  failedPIN = 0;
  publishEvent("ACCESS_GRANTED", method);
  Serial.println("SERVO COMMAND: UNLOCK");
}
void accessDenied(bool byRFID) {
  if (alarmActive) {
    publishEvent("ACCESS_BLOCKED", "Alarm active");
    return;
  }
  if (byRFID) {
    if (failedRFID < 3) failedRFID++;
    publishEvent("RFID_DENIED", "Unknown card");
    Serial.printf("RFID failed: %u/3\n", failedRFID);
    if (failedRFID >= 3) triggerAlarm("RFID_GAGAL_3X");
  } else {
    if (failedPIN < 3) failedPIN++;
    publishEvent("PIN_DENIED", "Incorrect PIN");
    Serial.printf("PIN failed: %u/3\n", failedPIN);
    if (failedPIN >= 3) triggerAlarm("PIN_GAGAL_3X");
  }
}
void handleRFID() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;
  if (millis() - lastCardRead < 1500) {
    rfid.PICC_HaltA(); rfid.PCD_StopCrypto1(); return;
  }
  lastCardRead = millis();
  String uid;
  for (byte i=0; i<rfid.uid.size; i++) {
    if (i) uid += ':';
    if (rfid.uid.uidByte[i] < 16) uid += '0';
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  if (uid == VALID_UID) unlockDoor("RFID");
  else accessDenied(true);
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

void handleLocalAction() {
  switch (localAction) {
    case ACCESS: unlockDoor("KEYPAD"); break;
    case RESET_ALARM:
      if (doorStableOpen || doorOpenRaw() ||
          (mode == "AFTERHOURS" && motionDetected())) {
        publishEvent("RESET_DENIED", "Door open or motion detected");
      } else {
        // Reset tidak memerlukan aktuator baru: hanya ketika pintu CLOSED.
        alarmActive = false;
        buzzerSilenced = false;
        alarmReason = "";
        failedRFID = failedPIN = 0;
        updateOutputs();
        lockDoor("Local reset");
        publishEvent("ALARM_RESET", "PIN verified locally");
      }
      break;
    case SILENCE_ALARM:
      if (alarmActive) {
        buzzerSilenced = true;
        updateOutputs(); // relay & LED merah tetap aktif
        publishEvent("ALARM_SILENCED", "Buzzer off only");
      } else publishEvent("SILENCE_IGNORED", "No active alarm");
      break;
    case TOGGLE_MODE:
      mode = (mode == "NORMAL") ? "AFTERHOURS" : "NORMAL";
      publishEvent("MODE_CHANGE", mode.c_str());
      if (mode == "AFTERHOURS" && motionDetected() && !unlocked)
        triggerAlarm("INTRUSI_PIR");
      break;
  }
}
void handleKeypad() {
  if (pinBuffer.length() && millis()-lastKeyTime > PIN_TIMEOUT_MS) {
    pinBuffer = "";
    localAction = ACCESS;
    Serial.println("PIN input timeout");
  }
  char k = keypad.getKey();
  if (!k) return;
  lastKeyTime = millis();
  if (k == '*') {
    pinBuffer = ""; localAction = ACCESS;
    Serial.println("Input canceled"); return;
  }
  if ((k == 'A' || k == 'B' || k == 'C') && pinBuffer.isEmpty()) {
    localAction = (k=='A') ? RESET_ALARM : (k=='B') ? SILENCE_ALARM : TOGGLE_MODE;
    Serial.println(k=='A' ? "Enter PIN then # for RESET" :
                   k=='B' ? "Enter PIN then # for SILENCE" :
                            "Enter PIN then # for MODE");
    return;
  }
  if (k == '#') {
    if (pinBuffer.length()) {
      if (pinBuffer == VALID_PIN) handleLocalAction();
      else accessDenied(false);
    }
    pinBuffer = "";
    localAction = ACCESS;
    return;
  }
  if (k >= '0' && k <= '9' && pinBuffer.length() < 12) {
    pinBuffer += k;
    Serial.println("Key entered: *");
  }
}

void handleDoor() {
  bool raw = doorOpenRaw();
  uint32_t now = millis();
  if (!doorInitialized) {
    doorInitialized = true;
    doorStableOpen = doorCandidateOpen = raw;
    doorCandidateSince = now;
    return;
  }
  if (raw != doorCandidateOpen) {
    doorCandidateOpen = raw;
    doorCandidateSince = now;
  }
  if (doorCandidateOpen == doorStableOpen || now - doorCandidateSince < 40) return;
  doorStableOpen = doorCandidateOpen;
  if (doorStableOpen) {
    publishEvent("DOOR_OPEN", "Door sensor opened");
    if (unlocked) openedAfterUnlock = true;
    else triggerAlarm("PINTU_DIBUKA_PAKSA");
  } else {
    publishEvent("DOOR_CLOSED", "Door sensor closed");
    if (unlocked && openedAfterUnlock) lockDoor("Door closed after access");
  }
}
void handleAutoLock() {
  if (!unlocked || doorStableOpen || doorOpenRaw()) return;
  if (!openedAfterUnlock && millis() - unlockedAt >= UNLOCK_TIMEOUT_MS)
    lockDoor("Auto-lock after 10 seconds");
}
void handlePIR() {
  if (!pirReady && millis() - pirStart >= PIR_WARMUP_MS) {
    pirReady = true;
    Serial.println("PIR READY");
  }
  if (pirReady && mode == "AFTERHOURS" && motionDetected() && !unlocked)
    triggerAlarm("INTRUSI_PIR");
}
void connectNetwork() {
  uint32_t now = millis();
  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastWiFiAttempt >= 10000) {
      lastWiFiAttempt = now;
      WiFi.disconnect(); WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      Serial.println("Connecting Wi-Fi...");
    }
    return;
  }
  if (mqtt.connected()) return;
  if (now - lastMQTTAttempt < 5000) return;
  lastMQTTAttempt = now;
  String id = "smartlab-v4-" + WiFi.macAddress();
  id.replace(":", "");
  if (mqtt.connect(id.c_str(), MQTT_USER, MQTT_PASSWORD)) {
    Serial.println("MQTT CONNECTED");
    publishEvent("DEVICE_ONLINE", "ESP32-S3 connected");
    lastTelemetry = millis() - 2000;
    lastHeartbeat = millis() - 5000;
  } else {
    Serial.printf("MQTT FAILED: %d\n", mqtt.state());
  }
}
void sendTelemetry() {
  char payload[512];
  snprintf(payload, sizeof(payload),
    "{\"device\":\"ESP32-S3\",\"source\":\"HARDWARE\","
    "\"door\":\"%s\",\"lock\":\"%s\",\"lock_verified\":false,"
    "\"motion\":%s,\"alarm\":%s,\"mode\":\"%s\","
    "\"buzzer\":%s,\"relay\":%s,\"buzzer_silenced\":%s,"
    "\"pir_ready\":%s,\"failedRFID\":%u,\"failedPIN\":%u,"
    "\"lastAccess\":\"%s\",\"alarm_reason\":\"%s\"}",
    doorStableOpen ? "OPEN" : "CLOSED",
    unlocked ? "UNLOCKED" : "LOCKED",
    motionDetected() ? "true" : "false",
    alarmActive ? "true" : "false",
    mode.c_str(), buzzerOn ? "true" : "false",
    alarmActive ? "true" : "false",
    buzzerSilenced ? "true" : "false",
    pirReady ? "true" : "false",
    failedRFID, failedPIN, lastAccess.c_str(), alarmReason.c_str());
  bool ok = mqtt.publish(TOPIC_TELEMETRY, payload, false);
  Serial.println(ok ? "TELEMETRY SENT" : "TELEMETRY FAILED");
}
void sendHeartbeat() {
  char payload[160];
  snprintf(payload, sizeof(payload),
    "{\"device\":\"ESP32-S3\",\"source\":\"HARDWARE\",\"status\":\"ONLINE\",\"uptime_ms\":%lu}",
    (unsigned long)millis());
  mqtt.publish(TOPIC_HEARTBEAT, payload, false);
}

void setup() {
  Serial.begin(115200);
  delay(1200);
  Serial.println("SMART LAB V4 - STAGE 9B");
  pinMode(DOOR_PIN, INPUT_PULLUP);
  pinMode(PIR_PIN, INPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_RED, LOW);
  // Tentukan kondisi tidak aktif relay sedini mungkin.
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW);
  pinMode(RELAY_PIN, OUTPUT);
  relaySet(false);
  servoReady = ledcAttach(SERVO_PIN, SERVO_FREQ, SERVO_RES);
  buzzerReady = ledcAttach(BUZZER_PIN, 2000, 8);
  if (!servoReady || !buzzerReady) {
    Serial.println("PWM INIT FAILED - STOP");
    while(true) delay(1000);
  }
  buzzerOn = false;
  ledcWriteTone(BUZZER_PIN, 0);
  doorStableOpen = doorCandidateOpen = doorOpenRaw();
  doorInitialized = true;
  doorCandidateSince = millis();
  if (!doorStableOpen && !servoPulse(SERVO_LOCK_US)) {
    Serial.println("LOCK SERVO COMMAND FAILED - STOP");
    while(true) delay(1000);
  }
  if (doorStableOpen) triggerAlarm("DOOR_OPEN_AT_STARTUP");
  else updateOutputs();
  SPI.begin(RFID_SCK, RFID_MISO, RFID_MOSI, RFID_SS);
  rfid.PCD_Init();
  Serial.printf("RFID Version: 0x%02X\n", rfid.PCD_ReadRegister(MFRC522::VersionReg));
  pirStart = millis();
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setKeepAlive(30);
  mqtt.setBufferSize(768);
  Serial.println("STARTED. Keypad: PIN# access; A PIN# reset; B PIN# silence; C PIN# mode; * cancel");
}
void loop() {
  connectNetwork();
  handleDoor();
  handlePIR();
  handleRFID();
  handleKeypad();
  handleAutoLock();
  updateOutputs();
  if (mqtt.connected()) {
    mqtt.loop();
    uint32_t now = millis();
    if (now - lastTelemetry >= 2000) {
      lastTelemetry = now; sendTelemetry();
    }
    if (now - lastHeartbeat >= 5000) {
      lastHeartbeat = now; sendHeartbeat();
    }
  }
  delay(5);
}
