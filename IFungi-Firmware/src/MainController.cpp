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

const unsigned long SENSOR_READ_INTERVAL = 2000;
const unsigned long ACTUATOR_CONTROL_INTERVAL = 5000;
const unsigned long FIREBASE_UPDATE_INTERVAL = 5000;
const unsigned long HEARTBEAT_INTERVAL = 30000;
const unsigned long HISTORY_UPDATE_INTERVAL = 300000;
const unsigned long LOCAL_SAVE_INTERVAL = 60000;

// LED task handle
TaskHandle_t ledTaskHandle = NULL;

void ledTask(void * parameter) {
    unsigned long lastBlinkTime = 0;
    int blinkState = 0;
    int blinkPattern = 0;
    unsigned long blinkInterval = 1000;
    
    for(;;) {
        // Update pattern based on connection status
        if (!WiFi.isConnected()) {
            blinkPattern = 0; // Off
        } else if (WiFi.getMode() == WIFI_AP) {
            blinkPattern = 1; // Fast blink (AP mode)
            blinkInterval = 500;
        } else if (WiFi.status() == WL_CONNECTED) {
            if (firebase.isAuthenticated()) {
                blinkPattern = 3; // Solid (connected and authenticated)
            } else {
                blinkPattern = 2; // Slow blink (connected but not authenticated)
                blinkInterval = 1000;
            }
        } else {
            blinkPattern = 0; // Off
        }
        
        // Execute blink pattern
        switch (blinkPattern) {
            case 0: // Off
                digitalWrite(LED_BUILTIN, LOW);
                break;
                
            case 1: // Fast blink (AP mode)
            case 2: // Slow blink (not authenticated)
                if (millis() - lastBlinkTime > blinkInterval) {
                    digitalWrite(LED_BUILTIN, blinkState);
                    blinkState = !blinkState;
                    lastBlinkTime = millis();
                }
                break;
                
            case 3: // Solid (connected and authenticated)
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
    
    Serial.println("✅ LED task initialized");
}

void setupSensorsAndActuators() {
    Serial.println("🔧 Initializing sensors and actuators...");
    
    // Initialize sensors
    sensors.begin();
    
    // Initialize actuators with pin configuration
    // LED Pin: 4, Relays: 23, 14, 18, 19, Servo: 13
    actuators.begin(4, 23, 14, 18, 19, 13);
    
    // Load setpoints from NVS or use defaults
    if (!actuators.loadSetpointsNVS()) {
        Serial.println("⚙️ Using default setpoints");
        actuators.applySetpoints(5000, 20.0, 30.0, 60.0, 80.0, 400, 400, 100);
    }
    
    // Connect actuators to Firebase handler
    actuators.setFirebaseHandler(&firebase);
    
    Serial.println("✅ Sensors and actuators initialized");
}

void setupWiFiAndFirebase() {
    Serial.println("🌐 Iniciando configuração de rede...");
    
    // Configuração do WiFiManager
    wifiManager.setConfigPortalTimeout(180); // 3 minutos para configurar
    wifiManager.setConnectTimeout(30); // 30 segundos para conectar
    wifiManager.setDebugOutput(true);
    wifiManager.setSaveConfigCallback([]() {
        Serial.println("✅ Configuração salva via portal web");
    });

    // Parâmetros customizados para Firebase
    WiFiManagerParameter custom_email("email", "Email Firebase", "", 40);
    WiFiManagerParameter custom_password("password", "Senha Firebase", "", 40, "type=\"password\"");
    
    wifiManager.addParameter(&custom_email);
    wifiManager.addParameter(&custom_password);

    // Tenta conectar automaticamente ou inicia portal de configuração
    Serial.println("📡 Tentando conectar ao WiFi...");
    
    bool wifiConnected = false;
    int wifiAttempts = 0;
    const int MAX_WIFI_ATTEMPTS = 2;

    while (!wifiConnected && wifiAttempts < MAX_WIFI_ATTEMPTS) {
        if (wifiManager.autoConnect("IFungi-Config", "config1234")) {
            wifiConnected = true;
            Serial.println("✅ WiFi conectado!");
            Serial.println("📡 IP: " + WiFi.localIP().toString());
            break;
        } else {
            wifiAttempts++;
            Serial.printf("❌ Falha na conexão WiFi (tentativa %d/%d)\n", wifiAttempts, MAX_WIFI_ATTEMPTS);
            
            if (wifiAttempts < MAX_WIFI_ATTEMPTS) {
                Serial.println("🔄 Tentando novamente em 5 segundos...");
                delay(5000);
                
                // Reset WiFi entre tentativas
                WiFi.disconnect(true);
                delay(1000);
                WiFi.mode(WIFI_STA);
                delay(1000);
            }
        }
    }

    if (!wifiConnected) {
        Serial.println("💥 Todas as tentativas de conexão WiFi falharam");
        Serial.println("🔄 Reiniciando em 5 segundos...");
        delay(5000);
        ESP.restart();
        return;
    }

    // Verifica qualidade da conexão WiFi
    if (WiFi.RSSI() < -80) {
        Serial.println("⚠️ Sinal WiFi fraco (RSSI: " + String(WiFi.RSSI()) + " dBm)");
    } else {
        Serial.println("📶 Sinal WiFi OK (RSSI: " + String(WiFi.RSSI()) + " dBm)");
    }

    // Processa credenciais do Firebase
    bool firebaseConfigured = false;
    bool usingNewCredentials = false;
    String email, firebasePassword;

    // Verifica se novas credenciais foram fornecidas via portal
    if (strlen(custom_email.getValue()) > 0 && strlen(custom_password.getValue()) > 0) {
        Serial.println("🆕 Novas credenciais Firebase fornecidas via portal");
        email = String(custom_email.getValue());
        firebasePassword = String(custom_password.getValue());
        usingNewCredentials = true;
        
        // Salva as novas credenciais
        Preferences preferences;
        if (preferences.begin("firebase-creds", false)) {
            preferences.putString("email", email);
            preferences.putString("password", firebasePassword);
            preferences.end();
            Serial.println("💾 Novas credenciais salvas no NVS");
        }
    } 
    // Se não há novas credenciais, tenta carregar as salvas
    else if (firebase.loadFirebaseCredentials(email, firebasePassword)) {
        Serial.println("📁 Usando credenciais Firebase salvas no NVS");
        usingNewCredentials = false;
    } 
    else {
        Serial.println("❌ Nenhuma credencial Firebase disponível");
        Serial.println("🌐 Por favor, acesse o portal web para configurar:");
        Serial.println("   http://" + WiFi.localIP().toString());
        Serial.println("   Ou reinicie e conecte ao AP 'IFungi-Config'");
        return;
    }

    // Autenticação no Firebase
    Serial.println("🔥 Iniciando autenticação no Firebase...");
    
    bool firebaseAuthenticated = false;
    int firebaseAttempts = 0;
    const int MAX_FIREBASE_ATTEMPTS = 3;

    while (!firebaseAuthenticated && firebaseAttempts < MAX_FIREBASE_ATTEMPTS) {
        firebaseAttempts++;
        Serial.printf("🔐 Tentativa %d/%d de autenticação Firebase...\n", 
                     firebaseAttempts, MAX_FIREBASE_ATTEMPTS);

        if (firebase.authenticate(email, firebasePassword)) {
            firebaseAuthenticated = true;
            Serial.println("✅ Autenticação Firebase bem-sucedida!");
            
            // 🔥 CORREÇÃO: Método correto é verifyGreenhouse() não verificarEstufa()
            firebase.verifyGreenhouse();
            
            // 🔥 CORREÇÃO: Método correto é sendLocalData() não enviarDadosLocais()
            firebase.sendLocalData();
            break;
        } else {
            Serial.printf("❌ Falha na autenticação Firebase (tentativa %d/%d)\n", 
                         firebaseAttempts, MAX_FIREBASE_ATTEMPTS);
            
            // Análise de possíveis erros
            if (firebaseAttempts == 1) {
                Serial.println("💡 Possíveis causas:");
                Serial.println("   - Credenciais inválidas/expiradas");
                Serial.println("   - Problema de conexão com a internet");
                Serial.println("   - Servidor Firebase indisponível");
            }
            
            if (firebaseAttempts < MAX_FIREBASE_ATTEMPTS) {
                Serial.println("🔄 Nova tentativa em 3 segundos...");
                delay(3000);
            }
        }
    }

    if (!firebaseAuthenticated) {
        Serial.println("💥 Falha crítica: Não foi possível autenticar no Firebase");
        
        if (usingNewCredentials) {
            Serial.println("🗑️ Removendo credenciais inválidas do NVS...");
            Preferences preferences;
            if (preferences.begin("firebase-creds", false)) {
                preferences.clear();
                preferences.end();
                Serial.println("✅ Credenciais inválidas removidas");
            }
        }
        
        Serial.println("🌐 Por favor, reconfigure as credenciais via portal web:");
        Serial.println("   http://" + WiFi.localIP().toString());
        Serial.println("⚠️ O sistema funcionará em modo offline até a configuração");
        
        // Não reinicia - permite operação offline
        return;
    }

    // Verificação final do estado
    Serial.println("🔍 Verificando estado final do sistema...");
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("✅ WiFi: CONECTADO");
    } else {
        Serial.println("❌ WiFi: DESCONECTADO");
    }
    
    if (firebase.isAuthenticated()) {
        Serial.println("✅ Firebase: AUTENTICADO");
    } else {
        Serial.println("❌ Firebase: NÃO AUTENTICADO");
    }

    Serial.println("🎉 Configuração de rede e Firebase concluída!");
    
    // Envia heartbeat inicial
    if (firebase.isAuthenticated()) {
        // 🔥 CORREÇÃO: Método correto é sendHeartbeat() não enviarHeartbeat()
        firebase.sendHeartbeat();
        Serial.println("💓 Heartbeat inicial enviado");
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
            Serial.println("⚠️ WiFi disconnected! Attempting to reconnect...");
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
        
        if (firebase.isAuthenticated() && !Firebase.ready()) {
            Serial.println("⚠️ Firebase disconnected! Attempting to reconnect...");
            firebase.refreshTokenIfNeeded();
        }
    }
}

void handleSensors() {
    if (millis() - lastSensorRead > SENSOR_READ_INTERVAL) {
        sensors.update();
        lastSensorRead = millis();
    }
}

void handleActuators() {
    if (millis() - lastActuatorControl > ACTUATOR_CONTROL_INTERVAL) {
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
    
    if (millis() - lastFirebaseUpdate > FIREBASE_UPDATE_INTERVAL) {
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
        
        // Check for commands and setpoints from Firebase
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
    if (millis() - lastHistoryUpdate > HISTORY_UPDATE_INTERVAL) {
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