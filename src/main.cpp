#include <SPI.h>
#include <MFRC522.h>
#include <Keypad.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <UniversalTelegramBot.h>
#include <Preferences.h>
#include <LiquidCrystal_I2C.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

// ---------- WiFi (Wokwi simulation with internet access) ----------
const char* ssid = "Wokwi-GUEST";
const char* password = "";

// ---------- Telegram ----------
#define BOT_TOKEN "8946478461:AAGHQPUJXJk2QLaMwsL6mmtZgHRpYK9VpVw"
#define LCD_SDA 2
#define LCD_SCL 0
#define LCD_SLEEP_TIMEOUT 15000UL
WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);
WiFiClientSecure telegramPollClient;
UniversalTelegramBot telegramPollBot(BOT_TOKEN, telegramPollClient);
Preferences preferences;
String telegramChatId;
LiquidCrystal_I2C lcd(0x27, 16, 2);
unsigned long lastLcdActivity = 0;
bool lcdSleeping = false;
const unsigned long OTP_LIFETIME = 300000UL;
String activeOTP;
unsigned long otpExpiresAt = 0;
SemaphoreHandle_t otpMutex = nullptr;

// ---------- RFID ----------
#define RST_PIN 22
#define SS_PIN  21
MFRC522 rfid(SS_PIN, RST_PIN);
byte validUID[4] = {0xDE, 0xAD, 0xBE, 0xEF}; // Replaced during RFID enrollment

// ---------- Keypad ----------
const byte ROWS = 4, COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {13, 12, 14, 27};
byte colPins[COLS] = {26, 25, 33, 32};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);
String enteredPIN = "";
String validPIN;

// ---------- Actuators and sensor ----------
#define RELAY_PIN      4   // Door solenoid through relay
#define LED_PIN        5   // Green LED: door open indicator
#define LED_ALARM_PIN  17  // Red LED: alarm/tamper indicator
#define BUZZER_PIN     15  // Buzzer
#define TAMPER_PIN     16  // Tamper sensor pushbutton

int failedAttempts = 0;
#define TAMPER_CONFIRM_WINDOW 3000UL
#define TAMPER_NOTIFICATION_COOLDOWN 30000UL
unsigned int tamperDetections = 0;
unsigned long firstTamperDetection = 0;
unsigned long lastTamperNotification = 0;
bool tamperNotificationSent = false;
bool tamperButtonActive = false;
enum ResetRequest { RESET_NONE, RESET_PIN, RESET_RFID };
ResetRequest pendingReset = RESET_NONE;
enum ResetVerificationStep { VERIFY_NONE, VERIFY_OLD_PIN, VERIFY_OLD_RFID };
ResetVerificationStep resetVerificationStep = VERIFY_NONE;
String resetPINEntry = "";

void connectWiFi();
void setupChatId();
void setupPIN();
void setupRFID();
void showMaskedPIN(const String& pin);
void lcdShow(const String& line1, const String& line2);
void wakeLcd();
void updateLcdSleep();
void checkRFID();
void checkKeypad();
void checkTamper();
void openDoor();
void registerFailedAttempt();
void triggerAlarm(String reason, bool notifyTelegram = true);
void sendTelegramAlert(String message);
void checkTelegramConnection();
void telegramPollingTask(void* parameter);
bool consumeOTP(const String& candidate);
void requestReset(ResetRequest resetRequest);
void handleResetKey(char key);
void completeReset();

void setup() {
  Serial.begin(115200);
  SPI.begin();
  rfid.PCD_Init();

  pinMode(RELAY_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(LED_ALARM_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(TAMPER_PIN, INPUT_PULLUP);

  Wire.begin(LCD_SDA, LCD_SCL);
  lcd.init();
  lcd.backlight();
  lcdShow("BeeGuard", "Starting...");

  preferences.begin("beeguard", false);
  telegramChatId = preferences.getString("chat_id", "");
  setupChatId();
  validPIN = preferences.getString("pin", "");
  setupPIN();
  setupRFID();

  connectWiFi();
  secured_client.setInsecure(); // Simulation only
  telegramPollClient.setInsecure(); // Simulation only
  checkTelegramConnection();
  lcdShow("Enter", "RFID or PIN");

  otpMutex = xSemaphoreCreateMutex();
  if (otpMutex != nullptr &&
      xTaskCreatePinnedToCore(telegramPollingTask, "TelegramPoll", 8192, nullptr, 1, nullptr, 0) == pdPASS) {
    Serial.println("Telegram OTP polling started.");
  } else {
    Serial.println("Telegram OTP polling unavailable.");
  }
}

void loop() {
  checkRFID();
  checkKeypad();
  checkTamper();
  updateLcdSleep();
}

void connectWiFi() {
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected: " + WiFi.localIP().toString());
}

void setupChatId() {
  if (telegramChatId.length() > 0) return;

  String enteredChatId;
  lcdShow("Setup Chat ID", "Enter keypad");
  Serial.println("Setup: Enter the Telegram Chat ID.");
  Serial.println("Press # to save, * to clear, A for +/-.");

  while (true) {
    char key = keypad.getKey();
    if (!key) {
      delay(10);
      continue;
    }

    if (key == '#') {
      if (enteredChatId.length() > 0 && enteredChatId != "-") {
        telegramChatId = enteredChatId;
        preferences.putString("chat_id", telegramChatId);
        Serial.println("Chat ID saved: " + telegramChatId);
        lcdShow("Chat ID saved", "Setup PIN next");
        return;
      }
      Serial.println("Chat ID is empty or invalid.");
    } else if (key == '*') {
      enteredChatId = "";
      Serial.println("Chat ID cleared.");
    } else if (key == 'A') {
      if (enteredChatId.startsWith("-")) {
        enteredChatId.remove(0, 1);
      } else {
        enteredChatId = "-" + enteredChatId;
      }
      Serial.println("Chat ID: " + enteredChatId);
    } else if (key >= '0' && key <= '9') {
      enteredChatId += key;
      Serial.println("Chat ID: " + enteredChatId);
      lcdShow("Chat ID:", enteredChatId);
    }
  }
}

void setupPIN() {
  if (validPIN.length() == 6) return;

  String enteredSetupPIN;
  lcdShow("Setup PIN 6 digit", "Enter on keypad");
  Serial.println("PIN setup: enter a 6-digit PIN on the keypad.");
  Serial.println("Press # to save or * to clear.");

  while (true) {
    char key = keypad.getKey();
    if (!key) {
      delay(10);
      continue;
    }

    if (key == '#') {
      if (enteredSetupPIN.length() == 6) {
        validPIN = enteredSetupPIN;
        preferences.putString("pin", validPIN);
        Serial.println("PIN saved.");
        lcdShow("PIN saved", "Setup RFID");
        return;
      }
      Serial.println("PIN must contain exactly 6 digits.");
    } else if (key == '*') {
      enteredSetupPIN = "";
      Serial.println("PIN cleared.");
      showMaskedPIN(enteredSetupPIN);
    } else if (key >= '0' && key <= '9') {
      if (enteredSetupPIN.length() < 6) {
        enteredSetupPIN += key;
        Serial.println("PIN digit received (" + String(enteredSetupPIN.length()) + "/6).");
        showMaskedPIN(enteredSetupPIN);
      }
    }
  }
}

void setupRFID() {
  if (preferences.getBytesLength("uid") == sizeof(validUID)) {
    preferences.getBytes("uid", validUID, sizeof(validUID));
    return;
  }

  Serial.println("RFID setup: tap a card to register it.");
  lcdShow("RFID setup", "Tap your card");

  while (true) {
    if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
      delay(10);
      continue;
    }

    if (rfid.uid.size != sizeof(validUID)) {
      Serial.println("UID must be 4 bytes. Try another card.");
      rfid.PICC_HaltA();
      continue;
    }

    for (byte i = 0; i < sizeof(validUID); i++) {
      validUID[i] = rfid.uid.uidByte[i];
    }
    preferences.putBytes("uid", validUID, sizeof(validUID));
    Serial.println("RFID card registered.");
    lcdShow("RFID saved", "Setup complete");
    rfid.PICC_HaltA();
    return;
  }
}

void showMaskedPIN(const String& pin) {
  String maskedPIN;

  for (unsigned int i = 0; i < pin.length(); i++) {
    maskedPIN += (i == pin.length() - 1) ? pin[i] : '*';
  }

  while (maskedPIN.length() < 6) {
    maskedPIN += '_';
  }

  Serial.println("PIN: " + maskedPIN);
  lcdShow("PIN:", maskedPIN);
}

void requestReset(ResetRequest resetRequest) {
  pendingReset = resetRequest;
  resetPINEntry = "";
  resetVerificationStep = VERIFY_OLD_PIN;

  Serial.println("Reset requested. Enter the current 6-digit PIN, then press #.");
  if (resetRequest == RESET_PIN) {
    lcdShow("Enter old PIN", "Then press #");
  } else {
    lcdShow("Enter PIN", "Then press #");
  }
}

void handleResetKey(char key) {
  if (key == '*') {
    pendingReset = RESET_NONE;
    resetVerificationStep = VERIFY_NONE;
    resetPINEntry = "";
    Serial.println("Reset cancelled.");
    lcdShow("Ready", "RFID or PIN");
    return;
  }

  if (resetVerificationStep != VERIFY_OLD_PIN) return;

  if (key == '#') {
    if (resetPINEntry.length() != 6) {
      Serial.println("Enter exactly 6 digits for the current PIN.");
      return;
    }

    if (resetPINEntry != validPIN) {
      Serial.println("Old PIN verification failed. Reset cancelled.");
      pendingReset = RESET_NONE;
      resetVerificationStep = VERIFY_NONE;
      resetPINEntry = "";
      lcdShow("Verification", "failed");
      return;
    }

    Serial.println("Old PIN verified.");
    resetVerificationStep = VERIFY_OLD_RFID;
    resetPINEntry = "";
    if (pendingReset == RESET_PIN) {
      Serial.println("PIN verified. Tap the current RFID card.");
      lcdShow("PIN verified", "Tap RFID");
    } else {
      Serial.println("PIN verified. Tap the current RFID card.");
      lcdShow("PIN verified", "Tap old RFID");
    }
  } else if (key >= '0' && key <= '9' && resetPINEntry.length() < 6) {
    resetPINEntry += key;
    showMaskedPIN(resetPINEntry);
  }
}

void completeReset() {
  ResetRequest resetRequest = pendingReset;
  pendingReset = RESET_NONE;
  resetVerificationStep = VERIFY_NONE;
  resetPINEntry = "";

  if (resetRequest == RESET_PIN) {
    preferences.remove("pin");
    validPIN = "";
    enteredPIN = "";
    Serial.println("Two-step verification passed. Enter a new PIN.");
    setupPIN();
  } else if (resetRequest == RESET_RFID) {
    preferences.remove("uid");
    Serial.println("Two-step verification passed. Tap a new RFID card.");
    setupRFID();
  }

  lcdShow("Reset complete", "Ready");
}

void lcdShow(const String& line1, const String& line2) {
  wakeLcd();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1.substring(0, 16));
  lcd.setCursor(0, 1);
  lcd.print(line2.substring(0, 16));
}

void wakeLcd() {
  if (lcdSleeping) {
    lcd.display();
    lcd.backlight();
    lcdSleeping = false;
  }
  lastLcdActivity = millis();
}

void updateLcdSleep() {
  if (!lcdSleeping && millis() - lastLcdActivity >= LCD_SLEEP_TIMEOUT) {
    lcd.noDisplay();
    lcd.noBacklight();
    lcdSleeping = true;
  }
}

void checkRFID() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  wakeLcd();

  if (resetVerificationStep == VERIFY_OLD_RFID) {
    bool oldCardMatches = (rfid.uid.size == sizeof(validUID));
    for (byte i = 0; i < sizeof(validUID) && oldCardMatches; i++) {
      if (rfid.uid.uidByte[i] != validUID[i]) oldCardMatches = false;
    }

    rfid.PICC_HaltA();
    if (oldCardMatches) {
      Serial.println("Current RFID verified.");
      if (pendingReset == RESET_RFID) {
        lcdShow("Old RFID valid", "Resetting...");
      } else {
        lcdShow("RFID verified", "Resetting...");
      }
      completeReset();
    } else {
      Serial.println("Old RFID verification failed. Reset cancelled.");
      pendingReset = RESET_NONE;
      resetVerificationStep = VERIFY_NONE;
      lcdShow("Verification", "failed");
    }
    return;
  }

  if (pendingReset != RESET_NONE) {
    rfid.PICC_HaltA();
    Serial.println("Reset in progress. Enter the current PIN first.");
    if (pendingReset == RESET_PIN) {
      lcdShow("Reset in progress", "Enter old PIN");
    } else {
      lcdShow("Reset in progress", "Enter PIN");
    }
    return;
  }

  bool match = (rfid.uid.size == 4);
  for (byte i = 0; i < 4 && match; i++) {
    if (rfid.uid.uidByte[i] != validUID[i]) match = false;
  }

  if (match) {
    Serial.println("RFID valid.");
    lcdShow("RFID valid", "Opening door...");
    openDoor();
  } else {
    Serial.println("RFID not recognized");
    lcdShow("RFID denied", "Try again");
    registerFailedAttempt();
  }
  rfid.PICC_HaltA();
}

void checkKeypad() {
  char key = keypad.getKey();
  if (!key) return;

  wakeLcd();

  if (pendingReset != RESET_NONE) {
    handleResetKey(key);
    return;
  }

  if (key == 'A') {
    requestReset(RESET_PIN);
  } else if (key == 'B') {
    requestReset(RESET_RFID);
  } else if (key == '#') { // Confirm PIN
    if (consumeOTP(enteredPIN)) {
      Serial.println("One-time Telegram code accepted.");
      lcdShow("OTP accepted", "Opening door...");
      openDoor();
    } else if (enteredPIN == validPIN) {
      lcdShow("PIN correct", "Opening door...");
      openDoor();
    } else {
      Serial.println("Incorrect PIN.");
      lcdShow("PIN incorrect", "Try again");
      registerFailedAttempt();
    }
    enteredPIN = "";
  } else if (key == '*') {
    enteredPIN = ""; // Clear input
    showMaskedPIN(enteredPIN);
  } else if (key >= '0' && key <= '9' && enteredPIN.length() < 6) {
    enteredPIN += key;
    showMaskedPIN(enteredPIN);
  }
}

void openDoor() {
  Serial.println("Access granted. Opening door.");
  lcdShow("Access granted", "Door open");
  digitalWrite(RELAY_PIN, HIGH);
  digitalWrite(LED_PIN, HIGH); // Green LED on
  failedAttempts = 0;
  delay(5000); // Keep the door open for 5 seconds
  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(LED_PIN, LOW);
  bot.sendMessage(telegramChatId, "Door opened successfully (authorized access).", "");
  lcdShow("Ready", "RFID or PIN");
}

void registerFailedAttempt() {
  failedAttempts++;
  if (failedAttempts >= 3) {
    triggerAlarm("Three consecutive failed access attempts detected.");
    failedAttempts = 0;
  }
}

void checkTamper() {
  bool tamperDetected = digitalRead(TAMPER_PIN) == LOW;

  if (!tamperDetected) {
    tamperButtonActive = false;
    return;
  }

  if (tamperButtonActive) return;
  tamperButtonActive = true;
  wakeLcd();

  unsigned long now = millis();
  if (tamperDetections == 0 || now - firstTamperDetection > TAMPER_CONFIRM_WINDOW) {
    tamperDetections = 0;
    firstTamperDetection = now;
  }

  tamperDetections++;
  const String reason = "Abnormal door vibration detected (possible break-in).";

  if (tamperDetections == 1) {
    Serial.println("First vibration detected. Local alarm active; waiting for confirmation.");
    triggerAlarm(reason, false);
  } else {
    bool cooldownExpired = !tamperNotificationSent ||
                           now - lastTamperNotification >= TAMPER_NOTIFICATION_COOLDOWN;
    if (cooldownExpired) {
      Serial.println("Repeated vibration confirmed. Sending Telegram alert.");
      triggerAlarm(reason, true);
      lastTamperNotification = millis();
      tamperNotificationSent = true;
    } else {
      Serial.println("Repeated vibration detected during Telegram cooldown.");
      triggerAlarm(reason, false);
    }
    tamperDetections = 0;
  }
}

void triggerAlarm(String reason, bool notifyTelegram) {
  Serial.println("ALARM: " + reason);
  lcdShow("!!! ALARM !!!", "Access denied");
  if (notifyTelegram) {
    sendTelegramAlert("[BeeGuard ALERT]\n" + reason + "\n(Camera snapshot here)");
  }
  digitalWrite(LED_ALARM_PIN, HIGH); // Red LED on
  digitalWrite(BUZZER_PIN, HIGH);
  delay(1500);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_ALARM_PIN, LOW);
}

void sendTelegramAlert(String message) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Telegram failed: WiFi is not connected.");
    return;
  }

  if (bot.sendMessage(telegramChatId, message, "")) {
    Serial.println("Telegram alarm notification sent.");
  } else {
    Serial.println("Telegram failed to send the alarm notification.");
  }
}

void checkTelegramConnection() {
  String token = BOT_TOKEN;
  if (token.indexOf(':') < 1) {
    Serial.println("Telegram failed: BOT_TOKEN is not a complete API token.");
    Serial.println("Expected format: 123456789:AA...secret-token...");
    return;
  }

  HTTPClient http;
  http.setTimeout(10000);
  String url = "https://api.telegram.org/bot" + token + "/getMe";
  if (!http.begin(secured_client, url)) {
    Serial.println("Telegram failed: could not initialize HTTPS request.");
    return;
  }

  int status = http.GET();
  String body = http.getString();
  http.end();

  if (status < 0) {
    Serial.println("Telegram HTTPS request failed: " + HTTPClient::errorToString(status));
    return;
  }

  DynamicJsonDocument response(1024);
  DeserializationError parseError = deserializeJson(response, body);
  if (parseError) {
    Serial.printf("Telegram returned an unreadable response (HTTP %d).\n", status);
    return;
  }

  if (status != HTTP_CODE_OK || !(response["ok"] | false)) {
    Serial.printf("Telegram API rejected getMe (HTTP %d): %s\n", status,
                  response["description"] | "No error description.");
    return;
  }

  bot.userName = response["result"]["username"].as<String>();
  Serial.println("Telegram connected to bot @" + bot.userName);
  if (telegramChatId.length() == 0) {
    Serial.println("Telegram bot is valid, but Chat ID is empty; startup message skipped.");
    return;
  }

  if (bot.sendMessage(telegramChatId,
                      "BeeGuard online. System ready. Send /otp to request a one-time door code.", "")) {
    Serial.println("Telegram startup message sent.");
  } else {
    Serial.println("Telegram bot is valid, but startup message failed; check the Chat ID and bot conversation.");
  }
}

void telegramPollingTask(void* parameter) {
  (void)parameter;
  long lastUpdateId = 0;

  while (true) {
    if (WiFi.status() != WL_CONNECTED || telegramChatId.length() == 0) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    HTTPClient http;
    String url = "https://api.telegram.org/bot";
    url += BOT_TOKEN;
    url += "/getUpdates?offset=";
    url += String(lastUpdateId + 1);
    url += "&limit=10&timeout=5";
    http.setTimeout(12000);

    if (http.begin(telegramPollClient, url)) {
      int status = http.GET();
      if (status == HTTP_CODE_OK) {
        String body = http.getString();
        DynamicJsonDocument response(4096);
        if (deserializeJson(response, body) == DeserializationError::Ok) {
          JsonArray updates = response["result"].as<JsonArray>();
          for (JsonObject update : updates) {
            long updateId = update["update_id"] | 0L;
            if (updateId > lastUpdateId) lastUpdateId = updateId;

            JsonObject message = update["message"];
            if (message.isNull()) continue;

            String chatId = message["chat"]["id"].as<String>();
            String text = message["text"] | "";
            if (chatId != telegramChatId ||
                (text != "/otp" && !text.startsWith("/otp@"))) continue;

            uint32_t code = esp_random() % 1000000UL;
            String generatedOTP = String(code);
            while (generatedOTP.length() < 6) generatedOTP = "0" + generatedOTP;

            if (xSemaphoreTake(otpMutex, portMAX_DELAY) == pdTRUE) {
              activeOTP = generatedOTP;
              otpExpiresAt = millis() + OTP_LIFETIME;
              xSemaphoreGive(otpMutex);
            }

            telegramPollBot.sendMessage(
                telegramChatId,
                "Your one-time door code is " + generatedOTP +
                    ". Enter it on the keypad and press # within 5 minutes. Requesting another code replaces this one.",
                "");
          }
        }
      }
      http.end();
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

bool consumeOTP(const String& candidate) {
  if (otpMutex == nullptr || xSemaphoreTake(otpMutex, portMAX_DELAY) != pdTRUE) return false;

  if (activeOTP.length() == 0) {
    xSemaphoreGive(otpMutex);
    return false;
  }

  if ((long)(millis() - otpExpiresAt) >= 0) {
    activeOTP = "";
    xSemaphoreGive(otpMutex);
    return false;
  }

  if (candidate != activeOTP) {
    xSemaphoreGive(otpMutex);
    return false;
  }

  activeOTP = "";
  otpExpiresAt = 0;
  xSemaphoreGive(otpMutex);
  return true;
}
