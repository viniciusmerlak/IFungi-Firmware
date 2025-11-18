#include <Arduino.h>
#include "GreenhouseSystem.h"
#include "SensorController.h"
#include "ActuatorController.h"
#include "QRCodeGenerator.h"
#include <WiFiManager.h>

// Global instances
WiFiManager wifiManager;
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

void ledTask(void * parameter) {
    unsigned long lastBlink = 0;
    int blinkState = 0;
    int blinkPattern = 0;
    unsigned long blinkInterval = 1000;
    
    for(;;) {
        if (!WiFi.isConnected()) {
            blinkPattern = 0;
        } else if (WiFi.getMode() == WIFI_AP) {
            blinkPattern = 1;
            blinkInterval = 500;
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

void setupWiFiAndFirebase() {
    Serial.println("🌐 Starting network configuration...");
    
    wifiManager.setConfigPortalTimeout(180);
    wifiManager.setConnectTimeout(30);
    wifiManager.setDebugOutput(true);
    wifiManager.setSaveConfigCallback([]() {
        Serial.println("✅ Configuration saved via web portal");
    });

    WiFiManagerParameter custom_email("email", "Firebase Email", "", 40);
    WiFiManagerParameter custom_password("password", "Firebase Password", "", 40, "type=\"password\"");
    
    wifiManager.addParameter(&custom_email);
    wifiManager.addParameter(&custom_password);

    Serial.println("📡 Connecting to WiFi...");
    
    bool wifiConnected = false;
    int wifiAttempts = 0;
    const int MAX_WIFI_ATTEMPTS = 2;

    while (!wifiConnected && wifiAttempts < MAX_WIFI_ATTEMPTS) {
        if (wifiManager.autoConnect("IFungi-Config", "config1234")) {
            wifiConnected = true;
            Serial.println("✅ WiFi connected!");
            Serial.println("📡 IP: " + WiFi.localIP().toString());
            break;
        } else {
            wifiAttempts++;
            Serial.printf("❌ WiFi connection failed (attempt %d/%d)\n", wifiAttempts, MAX_WIFI_ATTEMPTS);
            
            if (wifiAttempts < MAX_WIFI_ATTEMPTS) {
                Serial.println("🔄 Retrying in 5 seconds...");
                delay(5000);
                WiFi.disconnect(true);
                delay(1000);
                WiFi.mode(WIFI_STA);
                delay(1000);
            }
        }
    }

    if (!wifiConnected) {
        Serial.println("💥 All WiFi connection attempts failed");
        Serial.println("🔄 Restarting in 5 seconds...");
        delay(5000);
        ESP.restart();
        return;
    }

    if (WiFi.RSSI() < -80) {
        Serial.println("⚠️ Weak WiFi signal (RSSI: " + String(WiFi.RSSI()) + " dBm)");
    } else {
        Serial.println("📶 WiFi signal OK (RSSI: " + String(WiFi.RSSI()) + " dBm)");
    }

    bool firebaseConfigured = false;
    bool usingNewCredentials = false;
    String email, firebasePassword;

    if (strlen(custom_email.getValue()) > 0 && strlen(custom_password.getValue()) > 0) {
        Serial.println("🆕 New Firebase credentials provided via portal");
        email = String(custom_email.getValue());
        firebasePassword = String(custom_password.getValue());
        usingNewCredentials = true;
        
        Preferences preferences;
        if (preferences.begin("firebase-creds", false)) {
            preferences.putString("email", email);
            preferences.putString("password", firebasePassword);
            preferences.end();
            Serial.println("💾 New credentials saved to NVS");
        }
    } else if (firebase.loadFirebaseCredentials(email, firebasePassword)) {
        Serial.println("📁 Using saved Firebase credentials from NVS");
        usingNewCredentials = false;
    } else {
        Serial.println("❌ No Firebase credentials available");
        Serial.println("🌐 Please configure via web portal:");
        Serial.println("   http://" + WiFi.localIP().toString());
        return;
    }

    Serial.println("🔥 Starting Firebase authentication...");
    
    bool firebaseAuthenticated = false;
    int firebaseAttempts = 0;
    const int MAX_FIREBASE_ATTEMPTS = 3;

    while (!firebaseAuthenticated && firebaseAttempts < MAX_FIREBASE_ATTEMPTS) {
        firebaseAttempts++;
        Serial.printf("🔐 Firebase authentication attempt %d/%d...\n", 
                     firebaseAttempts, MAX_FIREBASE_ATTEMPTS);

        if (firebase.authenticate(email, firebasePassword)) {
            firebaseAuthenticated = true;
            Serial.println("✅ Firebase authentication successful!");
            
            // Greenhouse verification is now handled inside authenticate()
            // Attempt to send pending local data
            firebase.sendLocalData();
            break;
        } else {
            Serial.printf("❌ Firebase authentication failed (attempt %d/%d)\n", 
                         firebaseAttempts, MAX_FIREBASE_ATTEMPTS);
            
            if (firebaseAttempts == 1) {
                Serial.println("💡 Possible causes:");
                Serial.println("   - Invalid/expired credentials");
                Serial.println("   - Internet connection issue");
                Serial.println("   - Firebase server unavailable");
            }
            
            if (firebaseAttempts < MAX_FIREBASE_ATTEMPTS) {
                Serial.println("🔄 Retrying in 3 seconds...");
                delay(3000);
            }
        }
    }

    if (!firebaseAuthenticated) {
        Serial.println("💥 Critical: Could not authenticate with Firebase");
        
        if (usingNewCredentials) {
            Serial.println("🗑️ Removing invalid credentials from NVS...");
            Preferences preferences;
            if (preferences.begin("firebase-creds", false)) {
                preferences.clear();
                preferences.end();
                Serial.println("✅ Invalid credentials removed");
            }
        }
        
        Serial.println("🌐 Please reconfigure credentials via web portal:");
        Serial.println("   http://" + WiFi.localIP().toString());
        Serial.println("⚠️ System will operate in offline mode");
        return;
    }

    Serial.println("🔍 Final system status check...");
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("✅ WiFi: CONNECTED");
    } else {
        Serial.println("❌ WiFi: DISCONNECTED");
    }
    
    if (firebase.isAuthenticated()) {
        Serial.println("✅ Firebase: AUTHENTICATED");
    } else {
        Serial.println("❌ Firebase: NOT AUTHENTICATED");
    }

    Serial.println("🎉 Network and Firebase configuration completed!");
    
    if (firebase.isAuthenticated()) {
        firebase.sendHeartbeat();
        Serial.println("💓 Initial heartbeat sent");
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
        Serial.println("📴 Offline mode - data will be saved locally");
        saveDataLocally();
    }
}

void verifyConnectionStatus() {
    static unsigned long lastCheck = 0;
    const unsigned long CHECK_INTERVAL = 30000;
    
    if (millis() - lastCheck > CHECK_INTERVAL) {
        lastCheck = millis();
        
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("⚠️ WiFi disconnected! Attempting reconnect...");
            WiFi.reconnect();
            
            int reconnectAttempts = 0;
            while (WiFi.status() != WL_CONNECTED && reconnectAttempts < 5) {
                delay(1000);
                reconnectAttempts++;
            }
            
            if (WiFi.status() == WL_CONNECTED) {
                Serial.println("✅ WiFi reconnected");
                // Attempt to send pending local data after reconnection
                if (firebase.isAuthenticated()) {
                    firebase.sendLocalData();
                }
            } else {
                Serial.println("❌ WiFi reconnection failed");
            }
        }
        
        // Use refreshTokenIfNeeded instead of the removed refreshToken()
        if (firebase.isAuthenticated() && !Firebase.ready()) {
            Serial.println("⚠️ Firebase disconnected! Refreshing token...");
            firebase.refreshTokenIfNeeded();
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
        // Send sensor data to Firebase
        firebase.sendSensorData(
            sensors.getTemperature(),
            sensors.getHumidity(),
            sensors.getCO2(),
            sensors.getCO(),
            sensors.getLight(),
            sensors.getTVOCs(),
            sensors.getWaterLevel()
        );
        
        // Update actuator states in Firebase
        firebase.updateActuatorState(
            actuators.getRelayState(1),
            actuators.getRelayState(2),
            actuators.getRelayState(3),
            actuators.getRelayState(4),
            actuators.areLEDsOn(),
            actuators.getLEDsWatts(),
            actuators.isHumidifierOn()
        );
        
        // Receive setpoints from Firebase
        firebase.receiveSetpoints(actuators);
        
        lastFirebaseUpdate = millis();
    }
    
    // Send heartbeat periodically
    if (millis() - lastHeartbeat > HEARTBEAT_INTERVAL) {
        firebase.sendHeartbeat();
        lastHeartbeat = millis();
    }
}

void handleHistoryAndLocalData() {
    // Send data to history at defined interval
    if (millis() - lastHistoryUpdate > HISTORY_INTERVAL) {
        sendDataToHistory();
        lastHistoryUpdate = millis();
    }
    
    // Save data locally periodically (backup)
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
    delay(10);
}