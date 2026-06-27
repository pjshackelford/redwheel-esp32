/**
 * Copenhagen Wheel LED Status Indicator - HIGH SPEED CONNECTION VARIANT
 * Target Board: WEMOS LOLIN32 (Onboard Red LED on GPIO 22)
 * Behavior: Aggressive BLE parameters for rapid connection and Turbo strobe alert.
 */
#include "BLEDevice.h"
#include <vector>

// Passcode
#define PASSCODE "7B5FA797A1995EA718F969E05ABD8F9B"

// Pin definition for WEMOS LOLIN32 onboard Red LED
#define LED_PIN 22 

// Main Service UUIDs
static BLEUUID advertServiceUUID("52756265-6e43-6167-6e69-654350485000");
static BLEUUID serviceUUID("52756265-6e43-6167-6e69-654350485100");
static BLEUUID passwordUUID("52756265-6e43-6167-6e69-654350485101");
static BLEUUID rideModeUUID("52756265-6e43-6167-6e69-65435048500a");

static boolean doConnect = false;
static boolean connected = false;
static boolean doScan = false;

static BLERemoteCharacteristic* pPasswordCharacteristic;
static BLERemoteCharacteristic* pRideModeCharacteristic;
static BLEAdvertisedDevice* myDevice = nullptr;
BLEClient* pClient = nullptr;

// Optimized lightweight hex to byte parsing
std::vector<uint8_t> hexStringToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.length() / 2);
    for (size_t i = 0; i < hex.length(); i += 2) {
        uint16_t byte;
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

// Rapid strobe effect to capture attention
void strobeAlert() {
    Serial.println("🚨 TURBO MODE DETECTED! Strobing LED...");
    for (int i = 0; i < 15; i++) {
        digitalWrite(LED_PIN, LOW);  // Turn Red LED ON (Active-Low)
        delay(50);
        digitalWrite(LED_PIN, HIGH); // Turn Red LED OFF
        delay(50);
    }
}

class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {}
  void onDisconnect(BLEClient* pclient) {
    connected = false;
    doScan = true;
    Serial.println("onDisconnect");
  }
};

bool connectToServer() {
    Serial.print("Forming a connection to ");
    Serial.println(myDevice->getAddress().toString().c_str());
    
    pClient = BLEDevice::createClient();
    pClient->setClientCallbacks(new MyClientCallback());
    
    // Direct connect optimization
    if (!pClient->connect(myDevice)) {
        Serial.println(" - Failed to connect directly");
        return false;
    }
    Serial.println(" - Connected to Copenhagen Wheel");

    BLERemoteService* pRemoteService = pClient->getService(serviceUUID);
    if (pRemoteService == nullptr) {
      pClient->disconnect();
      return false;
    }

    // Authenticate via Password Characteristic
    pPasswordCharacteristic = pRemoteService->getCharacteristic(passwordUUID);
    if (pPasswordCharacteristic != nullptr) {
      std::vector<uint8_t> pw = hexStringToBytes(PASSCODE);
      pPasswordCharacteristic->writeValue(pw.data(), pw.size(), true);
      delay(200); // Reduced delay
    }

    BLERemoteService* pModeService = pClient->getService(advertServiceUUID);
    if (pModeService == nullptr) {
        pModeService = pRemoteService; 
    }

    pRideModeCharacteristic = pModeService->getCharacteristic(rideModeUUID);
    if (pRideModeCharacteristic == nullptr) {
      Serial.println("Failed to find Ride Mode characteristic.");
      return false;
    }

    connected = true;
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
  
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // Start with LED OFF (Active-Low)

  BLEDevice::init("");
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  
  // PERFORMANCE TWEAKS:
  pBLEScan->setActiveScan(true);     // Actively ask for response packets
  pBLEScan->setInterval(40);         // How often to scan (40 * 0.625ms = 25ms)
  pBLEScan->setWindow(40);            // Scan duration (Matching interval = 100% continuous scan duty cycle)
  
  pBLEScan->start(2, false);         // Look rapidly for just 2 seconds initially
}

void loop() {
  if (doConnect == true) {
    if (connectToServer()) {
      Serial.println("Connected and authenticated.");
    }
    doConnect = false;
  }
  
  if (connected && pRideModeCharacteristic != nullptr) {
    if (pRideModeCharacteristic->canRead()) {
      String currentRawMode = pRideModeCharacteristic->readValue(); 
      
      // Speed Optimization: Don't stringstream convert the whole payload. 
      // Just check the first 2 raw bytes for 0x55 and 0x7F ("557f")
      if (currentRawMode.length() >= 2 && 
          (uint8_t)currentRawMode.charAt(0) == 0x55 && 
          (uint8_t)currentRawMode.charAt(1) == 0x7F) {
        strobeAlert();
      } else {
        digitalWrite(LED_PIN, HIGH); 
      }
    }
  } else if (!connected && doScan) {
    // Fast re-scan parameters
    BLEDevice::getScan()->start(1, false);
  }
  
  delay(1500); // Shorter loop pause forsnappier performance
}