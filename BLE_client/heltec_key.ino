/**
 * Copenhagen Wheel Status Indicator & Mode Controller (V3.2)
 * Target Board: Heltec WiFi LoRa 32 (V3) [ESP32-S3]
 * Features: 
 *  - OLED Readout & Battery Telemetry
 *  - BLE Authentication & Ride Mode Cycling via PRG Button (GPIO 0)
 *  - Universal 10-Second Inactivity Deep Sleep (Connected & Disconnected)
 */
#include "BLEDevice.h"
#include <vector>
#include <U8g2lib.h>
#include <Wire.h>
#include "WheelPresets.h" // Must contain modePackets[3][20]

// Passcode
#define PASSCODE ""

// Heltec V3 Core Pin Assignments
#define LED_PIN 35          
#define VEXT_PIN 36         
#define OLED_RST 21         
#define OLED_SDA 17         
#define OLED_SCL 18         
#define PRG_BUTTON 0        // Built-in PRG button on Heltec V3

#define SLEEP_TIMEOUT_MS 10000 // Shut down after 10 seconds of total inactivity

// Custom BLE UUIDs
static BLEUUID advertServiceUUID("52756265-6e43-6167-6e69-654350485000");
static BLEUUID serviceUUID("52756265-6e43-6167-6e69-654350485100");
static BLEUUID passwordUUID("52756265-6e43-6167-6e69-654350485101");
static BLEUUID rideModeUUID("52756265-6e43-6167-6e69-65435048500a");
static BLEUUID batteryUUID("52756265-6e43-6167-6e69-65435048500b"); 

static boolean doConnect = false;
static boolean connected = false;
static boolean doScan = false;
static boolean authSuccess = false; 

volatile uint8_t currentBatteryLevel = 0; 
unsigned long lastActiveTime = 0; // Tracks timestamp of last user/BLE activity

static BLERemoteCharacteristic* pPasswordCharacteristic;
static BLERemoteCharacteristic* pRideModeCharacteristic;
static BLERemoteCharacteristic* pBatteryCharacteristic; 
static BLEAdvertisedDevice* myDevice = nullptr;
BLEClient* pClient = nullptr;

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ OLED_RST, /* clock=*/ OLED_SCL, /* data=*/ OLED_SDA);

const char* modeNames[] = {"ECO", "STANDARD", "TURBO"};

void showSmallStatus(String msg) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x8_tf); 
    u8g2.drawStr(0, 60, msg.c_str());
    u8g2.sendBuffer();
}

void showLargeStatus(String mode, uint8_t battery) {
    u8g2.clearBuffer();
    
    u8g2.setFont(u8g2_font_ncenB10_tr);
    u8g2.drawStr(0, 16, mode.c_str());
    
    if (battery > 0) {
        String batStr = String(battery) + "%";
        u8g2.setFont(u8g2_font_fub20_tr); 
        u8g2.drawStr(0, 52, batStr.c_str());
    } else {
        u8g2.setFont(u8g2_font_ncenB10_tr);
        u8g2.drawStr(0, 52, "Reading...");
    }
    
    u8g2.sendBuffer();
}

void executeDeepSleep(String shutdownMsg = "") {
    if (shutdownMsg != "") {
        showSmallStatus(shutdownMsg);
        delay(1500); 
    }
    
    u8g2.clear();
    digitalWrite(VEXT_PIN, HIGH); // Power off OLED
    digitalWrite(LED_PIN, LOW);   // Ensure onboard LED is off
    
    // Enable wake-up via PRG button (pulls LOW when pressed)
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PRG_BUTTON, 0);
    esp_deep_sleep_start();
}

std::vector<uint8_t> hexStringToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.length() / 2);
    for (size_t i = 0; i < hex.length(); i += 2) {
        char high = hex[i];
        char low = hex[i+1];
        int h = (high >= 'A') ? (high - 'A' + 10) : (high - '0');
        int l = (low >= 'A') ? (low - 'A' + 10) : (low - '0');
        if (high >= 'a') h = high - 'a' + 10;
        if (low >= 'a') l = low - 'a' + 10;
        bytes.push_back(static_cast<uint8_t>((h << 4) | l));
    }
    return bytes;
}

// Identify mode index from 20-byte read response (compares first 18 config bytes)
int identifyMode(const String& value) {
    if (value.length() != 20) return -1;
    for (int i = 0; i < 3; i++) {
        if (memcmp(value.c_str(), modePackets[i], 18) == 0) return i;
    }
    return -1;
}

// Write a new mode packet to the wheel (0 = ECO, 1 = STANDARD, 2 = TURBO)
bool setWheelMode(int targetMode) {
    if (!connected || pRideModeCharacteristic == nullptr) return false;
    if (targetMode < 0 || targetMode > 2) return false;

    uint8_t packet[20];
    memcpy(packet, modePackets[targetMode], 20);
    
    pRideModeCharacteristic->writeValue(packet, 20, true);
    delay(300); // Allow wheel processing time

    String actual = pRideModeCharacteristic->readValue();
    int verified = identifyMode(actual);

    return (verified == targetMode);
}

// Check physical PRG button to cycle ride modes and refresh activity timer
void checkButtonCycle() {
    static bool lastButtonState = HIGH;
    bool buttonState = digitalRead(PRG_BUTTON); 

    if (lastButtonState == HIGH && buttonState == LOW) {
        lastActiveTime = millis(); // Reset inactivity timer on button interaction
        
        if (connected && pRideModeCharacteristic != nullptr) {
            String current = pRideModeCharacteristic->readValue();
            int currentMode = identifyMode(current);
            
            // Cycle: ECO (0) -> STANDARD (1) -> TURBO (2) -> ECO (0)
            int nextMode = (currentMode + 1) % 3; 
            if (currentMode < 0) nextMode = 1; 

            showSmallStatus(String("Setting: ") + modeNames[nextMode]);
            
            if (setWheelMode(nextMode)) {
                showLargeStatus(modeNames[nextMode], currentBatteryLevel);
                delay(1000);
            } else {
                showSmallStatus("Mode write failed");
                delay(1000);
            }
        }
        delay(300); // Debounce
    }
    lastButtonState = buttonState;
}

static void batteryNotifyCallback(
  BLERemoteCharacteristic* pBLERemoteCharacteristic,
  uint8_t* pData, size_t length, bool isNotify) {
    if (length >= 8) {
        currentBatteryLevel = pData[7]; 
        lastActiveTime = millis(); // Reset inactivity timer on incoming telemetry
    }
}

class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {}
  void onDisconnect(BLEClient* pclient) {
    connected = false;
    authSuccess = false;
    doScan = true;
  }
};

bool connectToServer() {
    showSmallStatus("Authenticating...");
    pClient = BLEDevice::createClient();
    pClient->setClientCallbacks(new MyClientCallback());
    
    if (!pClient->connect(myDevice)) return false;

    BLERemoteService* pRemoteService = pClient->getService(serviceUUID);
    if (pRemoteService != nullptr) {
        pPasswordCharacteristic = pRemoteService->getCharacteristic(passwordUUID);
        if (pPasswordCharacteristic != nullptr) {
            std::vector<uint8_t> pw = hexStringToBytes(PASSCODE);
            pPasswordCharacteristic->writeValue(pw.data(), pw.size(), true);
            delay(300); 
        }
    }

    BLERemoteService* pModeService = pClient->getService(advertServiceUUID);
    if (pModeService == nullptr) pModeService = pRemoteService; 

    if (pModeService != nullptr) {
        pRideModeCharacteristic = pModeService->getCharacteristic(rideModeUUID);
        pBatteryCharacteristic = pModeService->getCharacteristic(batteryUUID);
        
        if (pBatteryCharacteristic != nullptr && pBatteryCharacteristic->canNotify()) {
            pBatteryCharacteristic->registerForNotify(batteryNotifyCallback);
        }
    }

    connected = true;
    lastActiveTime = millis(); // Reset inactivity timer upon successful connection
    return true;
}

class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.haveServiceUUID() && advertisedDevice.isAdvertisingService(advertServiceUUID)) {
      BLEDevice::getScan()->stop();
      myDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
      doScan = false;
    }
  }
};

void setup() {
  Serial.begin(115200);
  
  pinMode(VEXT_PIN, OUTPUT);
  digitalWrite(VEXT_PIN, LOW); 
  delay(100);                  

  pinMode(OLED_RST, OUTPUT);
  digitalWrite(OLED_RST, LOW);  
  delay(20);
  digitalWrite(OLED_RST, HIGH); 
  delay(50);

  pinMode(PRG_BUTTON, INPUT_PULLUP);

  u8g2.begin();
  u8g2.clear();

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW); 

  showSmallStatus("Scanning for Wheel...");

  BLEDevice::init("");
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);     
  pBLEScan->setInterval(40);         
  pBLEScan->setWindow(40);            
  pBLEScan->start(2, false);         

  lastActiveTime = millis(); // Initialize inactivity timer on boot
}

void loop() {
  // Check for 10 seconds of inactivity (connected or disconnected)
  if (millis() - lastActiveTime > SLEEP_TIMEOUT_MS) {
    executeDeepSleep("Idle 10s. Sleeping.");
  }

  if (doConnect == true) {
    if (connectToServer()) {
      authSuccess = true; 
      showSmallStatus("Fetching telemetry...");
      delay(800); 
    }
    doConnect = false;  
  }
  
  // Handle PRG button interactions
  if (connected) {
      checkButtonCycle();
  }

  // Display mode status
  if (connected && pRideModeCharacteristic != nullptr) {
    if (pRideModeCharacteristic->canRead()) {
      String currentRawMode = pRideModeCharacteristic->readValue(); 
      int modeIdx = identifyMode(currentRawMode);
      
      if (authSuccess) {
          if (modeIdx >= 0 && modeIdx < 3) {
              showLargeStatus(modeNames[modeIdx], currentBatteryLevel);
          } else {
              showLargeStatus("UNKNOWN", currentBatteryLevel);
          }
      }
    }
  } else if (!connected && doScan) {
    BLEDevice::getScan()->start(1, false);
  }
  
  delay(200); 
}
