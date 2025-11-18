#include <Arduino.h>
#include "GreenhouseSystem.h"
#include "SensorController.h"
#include "ActuatorController.h"
#include "QRCodeGenerator.h"
#include <WiFi.h>

// Global instances
FirebaseHandler firebase;
SensorController sensors;
ActuatorController actuators;
QRCodeGenerator qrGenerator;

String greenhouseID;

// Timing variables
unsigned long lastSensorRead = 0;
unsigned long lastActuatorControl = 0;
unsigned long lastFirebaseUpdate = 0;
unsigned long lastHeartbeat = 0;
unsigned long lastHistoryUpdate = 0;
unsigned long lastLocalSave = 0;

const unsigned long SENSOR_INTERVAL = 2000;
const unsigned long ACTUATOR_INTERVAL = 5000;
const unsigned long FIREBASE_INTERVAL = 5000;
const unsigned long HEARTBEAT_INTERVAL = 30000;
const unsigned long HISTORY_INTERVAL = 300000;
const unsigned long LOCAL_SAVE_INTERVAL = 60000;

// LED task handle
TaskHandle_t ledTaskHandle = NULL;

// WiFi credentials - configure conforme sua rede
const char* WIFI_SSID = "SUA_REDE_WIFI";
const char* WIFI_PASSWORD = "SUA_SENHA_WIFI";

void ledTask(void * parameter) {
    unsigned long lastBlink = 0;
    int blinkState = 0;
    int blinkPattern = 0;
    unsigned long blinkInterval = 1000;
    
    for(;;) {
        if (!WiFi.isConnected()) {
            blinkPattern = 0;
        } else if (WiFi.status() == WL_CONNECTED) {
            if (firebase.isAuthenticated()) {
                blinkPattern = 3;
            } else {
                blinkPattern = 2;
                blinkInterval = 1000;
            }
        } else {
            blinkPattern = 0;
        }
        
        switch (blinkPattern) {
            case 0:
                digitalWrite(LED_BUILTIN, LOW);
                break;
            case 1:
            case 2:
                if (millis() - lastBlink > blinkInterval) {
                    digitalWrite(LED_BUILTIN, blinkState);
                    blinkState = !blinkState;
                    lastBlink = millis();
                }
                break;
            case 3:
                digitalWrite(LED_BUILTIN, HIGH);
                break;
        }
        
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

void setupLEDTask() {
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);
    
    xTaskCreatePinnedToCore(
        ledTask,
        "LED_Task",
        2048,
        NULL,
        1,
        &ledTaskHandle,
        0
    );
    
    Serial.println("✅ LED task initialized on core " + String(xPortGetCoreID()));
}

void setupSensorsAndActuators() {
    Serial.println("🔧 Initializing sensors and actuators...");
    
    sensors.begin();
    actuators.begin(4, 23, 14, 18, 19, 13);
    
    if (!actuators.loadSetpointsNVS()) {
        Serial.println("⚙️ Using default setpoints");
        actuators.applySetpoints(5000, 20.0, 30.0, 60.0, 80.0, 400, 400, 100);
    }
    
    actuators.setFirebaseHandler(&firebase);
    Serial.println("✅ Sensors and actuators initialized");
}

bool connectToWiFi() {
    Serial.println("🌐 Connecting to WiFi...");
    Serial.println("📶 SSID: " + String(WIFI_SSID));
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    unsigned long startTime = millis();
    const unsigned long timeout = 15000; // 15 seconds timeout
    
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < timeout) {
        delay(500);
        Serial.print(".");
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n✅ WiFi connected!");
        Serial.println("📡 IP: " + WiFi.localIP().toString());
        Serial.println("📶 RSSI: " + String(WiFi.RSSI()) + " dBm");
        return true;
    } else {
        Serial.println("\n❌ WiFi connection failed");
        return false;
    }
}

void setupWiFiAndFirebase() {
    Serial.println("🌐 Starting network configuration...");
    
    // Try WiFi connection
    bool wifiConnected = connectToWiFi();
    
    if (!wifiConnected) {
        Serial.println("⚠️ System will operate in offline mode");
        return;
    }

    // Try to load Firebase credentials from NVS
    String email, firebasePassword;
    bool hasCredentials = firebase.loadFirebaseCredentials(email, firebasePassword);
    
    if (!hasCredentials) {
        Serial.println("❌ No Firebase credentials found in NVS");
        Serial.println("⚠️ System will operate in offline mode");
        return;
    }

    Serial.println("🔥 Starting Firebase authentication...");
    
    // Firebase authentication with timeout
    bool firebaseAuthenticated = false;
    int firebaseAttempts = 0;
    const int MAX_FIREBASE_ATTEMPTS = 2;

    while (!firebaseAuthenticated && firebaseAttempts < MAX_FIREBASE_ATTEMPTS) {
        firebaseAttempts++;
        Serial.printf("🔐 Firebase authentication attempt %d/%d...\n", 
                     firebaseAttempts, MAX_FIREBASE_ATTEMPTS);

        if (firebase.authenticate(email, firebasePassword)) {
            firebaseAuthenticated = true;
            Serial.println("✅ Firebase authentication successful!");
            firebase.sendLocalData(); // Try to send pending data
            break;
        } else {
            Serial.printf("❌ Firebase authentication failed (attempt %d/%d)\n", 
                         firebaseAttempts, MAX_FIREBASE_ATTEMPTS);
            
            if (firebaseAttempts < MAX_FIREBASE_ATTEMPTS) {
                delay(3000);
            }
        }
    }

    if (firebaseAuthenticated) {
        firebase.sendHeartbeat();
        Serial.println("💓 Initial heartbeat sent");
    } else {
        Serial.println("⚠️ Firebase authentication failed - offline mode");
    }
}

void saveDataLocally() {
    if (WiFi.status() != WL_CONNECTED || !firebase.isAuthenticated()) {
        unsigned long timestamp = firebase.getCurrentTimestamp();
        firebase.saveDataLocally(
            sensors.getTemperature(),
            sensors.getHumidity(),
            sensors.getCO2(),
            sensors.getCO(),
            sensors.getLight(),
            sensors.getTVOCs(),
            timestamp
        );
        Serial.println("💾 Data saved locally (offline mode)");
    }
}

void sendDataToHistory() {
    if (WiFi.status() == WL_CONNECTED && firebase.isAuthenticated()) {
        bool sent = firebase.sendDataToHistory(
            sensors.getTemperature(),
            sensors.getHumidity(),
            sensors.getCO2(),
            sensors.getCO(),
            sensors.getLight(),
            sensors.getTVOCs()
        );
        
        if (sent) {
            Serial.println("📊 Data sent to Firebase history");
        } else {
            Serial.println("❌ Failed to send data to history");
        }
    } else {
        saveDataLocally();
    }
}

void verifyConnectionStatus() {
    static unsigned long lastCheck = 0;
    const unsigned long CHECK_INTERVAL = 30000;
    
    if (millis() - lastCheck > CHECK_INTERVAL) {
        lastCheck = millis();
        
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("⚠️ WiFi disconnected!");
            // Don't auto-reconnect for now to avoid blocking
        }
    }
}

void handleSensors() {
    if (millis() - lastSensorRead > SENSOR_INTERVAL) {
        sensors.update();
        lastSensorRead = millis();
    }
}

void handleActuators() {
    if (millis() - lastActuatorControl > ACTUATOR_INTERVAL) {
        actuators.controlAutomatically(
            sensors.getTemperature(),
            sensors.getHumidity(),
            sensors.getLight(),
            sensors.getCO(),
            sensors.getCO2(),
            sensors.getTVOCs(),
            sensors.getWaterLevel()
        );
        lastActuatorControl = millis();
    }
}

void handleFirebase() {
    if (!firebase.isAuthenticated() || WiFi.status() != WL_CONNECTED) {
        return;
    }
    
    if (millis() - lastFirebaseUpdate > FIREBASE_INTERVAL) {
        firebase.sendSensorData(
            sensors.getTemperature(),
            sensors.getHumidity(),
            sensors.getCO2(),
            sensors.getCO(),
            sensors.getLight(),
            sensors.getTVOCs(),
            sensors.getWaterLevel()
        );
        
        firebase.updateActuatorState(
            actuators.getRelayState(1),
            actuators.getRelayState(2),
            actuators.getRelayState(3),
            actuators.getRelayState(4),
            actuators.areLEDsOn(),
            actuators.getLEDsWatts(),
            actuators.isHumidifierOn()
        );
        
        firebase.receiveSetpoints(actuators);
        
        lastFirebaseUpdate = millis();
    }
    
    if (millis() - lastHeartbeat > HEARTBEAT_INTERVAL) {
        firebase.sendHeartbeat();
        lastHeartbeat = millis();
    }
}

void handleHistoryAndLocalData() {
    if (millis() - lastHistoryUpdate > HISTORY_INTERVAL) {
        sendDataToHistory();
        lastHistoryUpdate = millis();
    }
    
    if (millis() - lastLocalSave > LOCAL_SAVE_INTERVAL) {
        if (WiFi.status() != WL_CONNECTED || !firebase.isAuthenticated()) {
            saveDataLocally();
        }
        lastLocalSave = millis();
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n\n🚀 Starting IFungi Greenhouse System...");
    
    // Initialize LED task first
    setupLEDTask();
    
    // Initial configuration
    setupSensorsAndActuators();
    setupWiFiAndFirebase();

    // Generate greenhouse ID
    greenhouseID = "IFUNGI-" + getMacAddress();
    Serial.println("🏷️ Greenhouse ID: " + greenhouseID);
    qrGenerator.generateQRCode(greenhouseID);

    Serial.println("✅ System initialized and ready!");
}

void loop() {
    // Execute periodic tasks
    handleSensors();
    handleActuators();
    handleFirebase();
    handleHistoryAndLocalData();
    verifyConnectionStatus();
    
    // Small delay for stability
    delay(100);
}