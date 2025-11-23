/**
 * @file MainController.cpp
 * @brief Controlador principal do sistema IFungi Greenhouse
 * @author Seu Nome
 * @date 2024
 * @version 1.0
 * 
 * @details Este arquivo implementa o controlador principal que gerencia todos os componentes
 * do sistema de estufa automatizada, incluindo sensores, atuadores, comunicação WiFi/Firebase,
 * e tarefas em tempo real.
 * 
 * @note O sistema opera baseado em máquina de estados com temporizadores para garantir
 * eficiência e responsividade.
 */

#include <Arduino.h>
#include "GreenhouseSystem.h"
#include "SensorController.h"
#include "ActuatorController.h"
#include "QRCodeGenerator.h"
#include <WiFiManager.h>

// =============================================================================
// VARIÁVEIS GLOBAIS DO SISTEMA
// =============================================================================

/// @brief Gerenciador de conexão WiFi
WiFiManager wifiManager;

/// @brief Handler para comunicação com Firebase
FirebaseHandler firebase;

/// @brief Controlador dos sensores ambientais
SensorController sensors;

/// @brief Controlador dos atuadores (relés, LEDs, servo)
ActuatorController actuators;

/// @brief Gerador de QR Code para identificação da estufa
QRCodeGenerator qrGenerator;

/// @brief ID único da estufa baseado no MAC address
String greenhouseID;

// =============================================================================
// VARIÁVEIS DE TEMPORIZAÇÃO
// =============================================================================

/// @brief Timestamp da última leitura de sensores
unsigned long lastSensorRead = 0;

/// @brief Timestamp do último controle de atuadores  
unsigned long lastActuatorControl = 0;

/// @brief Timestamp da última atualização no Firebase
unsigned long lastFirebaseUpdate = 0;

/// @brief Timestamp do último heartbeat
unsigned long lastHeartbeat = 0;

/// @brief Timestamp do último histórico salvo
unsigned long lastHistoryUpdate = 0;

/// @brief Timestamp do último salvamento local
unsigned long lastLocalSave = 0;

/**
 * @def SENSOR_READ_INTERVAL
 * @brief Intervalo para leitura de sensores (2 segundos)
 */
const unsigned long SENSOR_READ_INTERVAL = 2000;

/**
 * @def ACTUATOR_CONTROL_INTERVAL  
 * @brief Intervalo para controle de atuadores (5 segundos)
 */
const unsigned long ACTUATOR_CONTROL_INTERVAL = 5000;

/**
 * @def FIREBASE_UPDATE_INTERVAL
 * @brief Intervalo para atualização no Firebase (5 segundos)
 */
const unsigned long FIREBASE_UPDATE_INTERVAL = 5000;

/**
 * @def HEARTBEAT_INTERVAL
 * @brief Intervalo para envio de heartbeat (30 segundos)
 */
const unsigned long HEARTBEAT_INTERVAL = 30000;

/**
 * @def HISTORY_UPDATE_INTERVAL
 * @brief Intervalo para salvamento de histórico (5 minutos)
 */
const unsigned long HISTORY_UPDATE_INTERVAL = 300000;

/**
 * @def LOCAL_SAVE_INTERVAL
 * @brief Intervalo para salvamento local (1 minuto)
 */
const unsigned long LOCAL_SAVE_INTERVAL = 60000;

// =============================================================================
// VARIÁVEIS PARA TAREFAS E MODO DEBUG
// =============================================================================

/// @brief Handle da tarefa FreeRTOS para controle do LED
TaskHandle_t ledTaskHandle = NULL;

/// @brief Estado anterior do modo debug para detecção de mudanças
bool lastDebugMode = false;

/// @brief Timestamp da última verificação do modo debug
unsigned long lastDebugCheck = 0;

/**
 * @def DEBUG_CHECK_INTERVAL
 * @brief Intervalo para verificação do modo debug (2 segundos)
 */
const unsigned long DEBUG_CHECK_INTERVAL = 2000;

// =============================================================================
// FUNÇÕES DE CONTROLE DO MODO DEBUG E CALIBRAÇÃO
// =============================================================================

/**
 * @brief Gerencia o modo de debug e calibração do sistema
 * 
 * @details Esta função verifica periodicamente o estado do modo debug no Firebase
 * e aplica as configurações correspondentes. Quando o modo debug está ativo,
 * permite controle manual dos atuadores e operações de desenvolvimento.
 * 
 * @note Executada a cada 2 segundos para verificar mudanças no modo debug
 * 
 * @see ActuatorController::setDebugMode()
 * @see FirebaseHandler::getDebugMode()
 * @see FirebaseHandler::getDevModeSettings()
 * @see FirebaseHandler::getManualActuatorStates()
 */
void handleDebugAndCalibration() {
    // Verificação periódica do modo debug
    if (millis() - lastDebugCheck > DEBUG_CHECK_INTERVAL) {
        lastDebugCheck = millis();
        
        bool currentDebugMode = false;
        
        // Obtém estado atual do modo debug do Firebase
        if (firebase.isAuthenticated() && firebase.isFirebaseReady()) {
            currentDebugMode = firebase.getDebugMode();
        }
        
        // Detecta e processa mudanças no modo debug
        if (currentDebugMode != lastDebugMode) {
            actuators.setDebugMode(currentDebugMode);
            lastDebugMode = currentDebugMode;
            
            Serial.println(currentDebugMode ? "🔧 DEBUG MODE ENABLED" : "🔧 DEBUG MODE DISABLED");
            
            // Ao sair do modo debug, sincroniza estados com Firebase
            if (!currentDebugMode && firebase.isAuthenticated()) {
                delay(500); // Aguarda conclusão de escritas pendentes
                firebase.updateActuatorState(
                    actuators.getRelayState(1),
                    actuators.getRelayState(2),
                    actuators.getRelayState(3),
                    actuators.getRelayState(4),
                    actuators.areLEDsOn(),
                    actuators.getLEDsWatts(),
                    actuators.isHumidifierOn()
                );
                Serial.println("🔄 Updated actuator states after exiting debug mode");
            }
        }
        
        // Processa configurações quando modo debug está ativo
        if (currentDebugMode && firebase.isAuthenticated() && firebase.isFirebaseReady()) {
            // Lê configurações do modo de desenvolvimento
            bool analogRead, digitalWrite, pwm;
            int pin, pwmValue;
            firebase.getDevModeSettings(analogRead, digitalWrite, pin, pwm, pwmValue);
            actuators.setDevModeSettings(analogRead, digitalWrite, pin, pwm, pwmValue);
            
            // Cache para detecção de mudanças nos estados manuais
            static bool lastRelay1 = false, lastRelay2 = false, 
                        lastRelay3 = false, lastRelay4 = false;
            static bool lastLedsOn = false;
            static int lastLedsIntensity = 0;
            static bool lastHumidifierOn = false;
            
            // Lê estados manuais atuais do Firebase
            bool relay1, relay2, relay3, relay4, ledsOn, humidifierOn;
            int ledsIntensity;
            firebase.getManualActuatorStates(relay1, relay2, relay3, relay4, 
                                           ledsOn, ledsIntensity, humidifierOn);
            
            // Verifica se houve mudanças reais nos valores
            bool hasChanges = (relay1 != lastRelay1) || (relay2 != lastRelay2) || 
                             (relay3 != lastRelay3) || (relay4 != lastRelay4) ||
                             (ledsOn != lastLedsOn) || (ledsIntensity != lastLedsIntensity) ||
                             (humidifierOn != lastHumidifierOn);
            
            // Aplica mudanças se detectadas
            if (hasChanges) {
                Serial.println("🔄 Manual states changed, applying...");
                actuators.setManualStates(relay1, relay2, relay3, relay4, 
                                        ledsOn, ledsIntensity, humidifierOn);
                
                // Atualiza cache com novos valores
                lastRelay1 = relay1;
                lastRelay2 = relay2;
                lastRelay3 = relay3;
                lastRelay4 = relay4;
                lastLedsOn = ledsOn;
                lastLedsIntensity = ledsIntensity;
                lastHumidifierOn = humidifierOn;
            }
        }
        
        // Executa operações do modo de desenvolvimento
        actuators.handleDevMode();
    }
}

// =============================================================================
// TAREFA DO LED INDICADOR
// =============================================================================

/**
 * @brief Tarefa FreeRTOS para controle do LED indicador de status
 * 
 * @param parameter Parâmetros da tarefa (não utilizado)
 * 
 * @details Controla o LED built-in com diferentes padrões de piscada conforme
 * o estado de conexão do sistema. Os padrões são:
 * - Desconectado: LED apagado
 * - Modo AP: Piscada rápida (500ms)
 * - WiFi conectado: Piscada lenta (1000ms) 
 * - Firebase autenticado: LED fixo aceso
 * 
 * @note Esta tarefa roda continuamente com prioridade baixa
 * 
 * @see setupLEDTask()
 */
void ledTask(void * parameter) {
    unsigned long lastBlinkTime = 0;
    int blinkState = 0;
    int blinkPattern = 0;
    unsigned long blinkInterval = 1000;
    
    // Loop infinito da tarefa
    for(;;) {
        // Define padrão baseado no estado de conexão
        if (!WiFi.isConnected()) {
            blinkPattern = 0; // LED apagado (desconectado)
        } else if (WiFi.getMode() == WIFI_AP) {
            blinkPattern = 1; // Piscada rápida (modo AP)
            blinkInterval = 500;
        } else if (WiFi.status() == WL_CONNECTED) {
            if (firebase.isAuthenticated()) {
                blinkPattern = 3; // LED fixo (conectado e autenticado)
            } else {
                blinkPattern = 2; // Piscada lenta (conectado mas não autenticado)
                blinkInterval = 1000;
            }
        } else {
            blinkPattern = 0; // LED apagado (estado desconhecido)
        }
        
        // Executa o padrão de piscada definido
        switch (blinkPattern) {
            case 0: // Desligado
                digitalWrite(LED_BUILTIN, LOW);
                break;
                
            case 1: // Piscada rápida (modo AP)
            case 2: // Piscada lenta (não autenticado)
                if (millis() - lastBlinkTime > blinkInterval) {
                    digitalWrite(LED_BUILTIN, blinkState);
                    blinkState = !blinkState;
                    lastBlinkTime = millis();
                }
                break;
                
            case 3: // Fixo aceso (conectado e autenticado)
                digitalWrite(LED_BUILTIN, HIGH);
                break;
        }
        
        // Pequena pausa para evitar uso excessivo de CPU
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

/**
 * @brief Inicializa a tarefa do LED indicador
 * 
 * @details Configura o pino do LED e cria a tarefa FreeRTOS responsável
 * pelo controle do LED indicador de status.
 * 
 * @note A tarefa é executada no core 0 com prioridade 1
 * 
 * @see ledTask()
 */
void setupLEDTask() {
    // Configura pino do LED como saída
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);
    
    // Cria tarefa FreeRTOS
    xTaskCreatePinnedToCore(
        ledTask,           // Função da tarefa
        "LED_Task",        // Nome da tarefa
        2048,              // Stack size
        NULL,              // Parâmetros
        1,                 // Prioridade
        &ledTaskHandle,    // Handle da tarefa
        0                  // Core (0)
    );
    
    Serial.println("✅ LED task initialized");
}

// =============================================================================
// INICIALIZAÇÃO DE SENSORES E ATUADORES
// =============================================================================

/**
 * @brief Inicializa sensores e atuadores do sistema
 * 
 * @details Realiza a inicialização de todos os componentes de hardware:
 * - Sensores DHT22, CCS811, LDR, MQ-7 e sensor de água
 * - Atuadores: relés, LEDs PWM, servo motor
 * - Carrega setpoints da NVS ou usa valores padrão
 * - Conecta atuadores ao handler do Firebase
 * 
 * @note Chamada uma vez durante o setup()
 * 
 * @see SensorController::begin()
 * @see ActuatorController::begin()
 * @see ActuatorController::loadSetpointsNVS()
 * @see ActuatorController::applySetpoints()
 * @see ActuatorController::setFirebaseHandler()
 */
void setupSensorsAndActuators() {
    Serial.println("🔧 Initializing sensors and actuators...");
    
    // Inicialização dos sensores
    sensors.begin();
    
    // Inicialização dos atuadores com configuração de pinos
    // LED Pin: 4, Relés: 23, 14, 18, 19, Servo: 13
    actuators.begin(4, 23, 14, 18, 19, 13);
    
    // Carrega setpoints do NVS ou usa valores padrão
    if (!actuators.loadSetpointsNVS()) {
        Serial.println("⚙️ Using default setpoints");
        actuators.applySetpoints(5000, 20.0, 30.0, 60.0, 80.0, 400, 400, 100);
    }
    
    // Conecta atuadores ao handler do Firebase para sincronização
    actuators.setFirebaseHandler(&firebase);
    
    Serial.println("✅ Sensors and actuators initialized");
}

// =============================================================================
// CONFIGURAÇÃO DE REDE E FIREBASE
// =============================================================================

/**
 * @brief Configura conexão WiFi e autenticação Firebase
 * 
 * @details Gerencia todo o processo de conexão de rede e autenticação:
 * - Configura WiFiManager com portal de configuração
 * - Tenta conexão automática com fallback para modo AP
 * - Processa credenciais Firebase (novas ou salvas)
 * - Realiza autenticação com tentativas e fallback offline
 * - Verifica qualidade do sinal WiFi
 * - Envia heartbeat inicial
 * 
 * @note Inclui mecanismos de retry e recuperação de falhas
 * 
 * @warning Em caso de falha crítica na autenticação, o sistema opera em modo offline
 * 
 * @see WiFiManager
 * @see FirebaseHandler::authenticate()
 * @see FirebaseHandler::verifyGreenhouse()
 * @see FirebaseHandler::sendLocalData()
 * @see FirebaseHandler::sendHeartbeat()
 */
void setupWiFiAndFirebase() {
    Serial.println("🌐 Iniciando configuração de rede...");
    
    // Configuração do WiFiManager
    wifiManager.setConfigPortalTimeout(180); // 3 minutos para configurar
    wifiManager.setConnectTimeout(30); // 30 segundos para conectar
    wifiManager.setDebugOutput(true);
    wifiManager.setSaveConfigCallback([]() {
        Serial.println("✅ Configuração salva via portal web");
    });

    // Parâmetros customizados para credenciais Firebase
    WiFiManagerParameter custom_email("email", "Email Firebase", "", 40);
    WiFiManagerParameter custom_password("password", "Senha Firebase", "", 40, "type=\"password\"");
    
    wifiManager.addParameter(&custom_email);
    wifiManager.addParameter(&custom_password);

    // Tentativa de conexão automática ou inicia portal de configuração
    Serial.println("📡 Tentando conectar ao WiFi...");
    
    bool wifiConnected = false;
    int wifiAttempts = 0;
    const int MAX_WIFI_ATTEMPTS = 2;

    // Loop de tentativas de conexão WiFi
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

    // Fallback se todas as tentativas falharem
    if (!wifiConnected) {
        Serial.println("💥 Todas as tentativas de conexão WiFi falharam");
        Serial.println("🔄 Reiniciando em 5 segundos...");
        delay(5000);
        ESP.restart();
        return;
    }

    // Verificação da qualidade do sinal WiFi
    if (WiFi.RSSI() < -80) {
        Serial.println("⚠️ Sinal WiFi fraco (RSSI: " + String(WiFi.RSSI()) + " dBm)");
    } else {
        Serial.println("📶 Sinal WiFi OK (RSSI: " + String(WiFi.RSSI()) + " dBm)");
    }

    // Processamento das credenciais do Firebase
    bool firebaseConfigured = false;
    bool usingNewCredentials = false;
    String email, firebasePassword;

    // Verifica se novas credenciais foram fornecidas via portal
    if (strlen(custom_email.getValue()) > 0 && strlen(custom_password.getValue()) > 0) {
        Serial.println("🆕 Novas credenciais Firebase fornecidas via portal");
        email = String(custom_email.getValue());
        firebasePassword = String(custom_password.getValue());
        usingNewCredentials = true;
        
        // Salva as novas credenciais no NVS
        Preferences preferences;
        if (preferences.begin("firebase-creds", false)) {
            preferences.putString("email", email);
            preferences.putString("password", firebasePassword);
            preferences.end();
            Serial.println("💾 Novas credenciais salvas no NVS");
        }
    } 
    // Tenta carregar credenciais salvas no NVS
    else if (firebase.loadFirebaseCredentials(email, firebasePassword)) {
        Serial.println("📁 Usando credenciais Firebase salvas no NVS");
        usingNewCredentials = false;
    } 
    // Nenhuma credencial disponível
    else {
        Serial.println("❌ Nenhuma credencial Firebase disponível");
        Serial.println("🌐 Por favor, acesse o portal web para configurar:");
        Serial.println("   http://" + WiFi.localIP().toString());
        Serial.println("   Ou reinicie e conecte ao AP 'IFungi-Config'");
        return;
    }

    // Processo de autenticação no Firebase
    Serial.println("🔥 Iniciando autenticação no Firebase...");
    
    bool firebaseAuthenticated = false;
    int firebaseAttempts = 0;
    const int MAX_FIREBASE_ATTEMPTS = 3;

    // Loop de tentativas de autenticação Firebase
    while (!firebaseAuthenticated && firebaseAttempts < MAX_FIREBASE_ATTEMPTS) {
        firebaseAttempts++;
        Serial.printf("🔐 Tentativa %d/%d de autenticação Firebase...\n", 
                     firebaseAttempts, MAX_FIREBASE_ATTEMPTS);

        if (firebase.authenticate(email, firebasePassword)) {
            firebaseAuthenticated = true;
            Serial.println("✅ Autenticação Firebase bem-sucedida!");
            
            // Verifica e configura a estufa no Firebase
            firebase.verifyGreenhouse();
            
            // Envia dados locais pendentes
            firebase.sendLocalData();
            break;
        } else {
            Serial.printf("❌ Falha na autenticação Firebase (tentativa %d/%d)\n", 
                         firebaseAttempts, MAX_FIREBASE_ATTEMPTS);
            
            // Análise de possíveis causas no primeiro erro
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

    // Fallback para modo offline se autenticação falhar
    if (!firebaseAuthenticated) {
        Serial.println("💥 Falha crítica: Não foi possível autenticar no Firebase");
        
        // Remove credenciais inválidas do NVS
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

    // Verificação final do estado do sistema
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
        firebase.sendHeartbeat();
        Serial.println("💓 Heartbeat inicial enviado");
    }
}

// =============================================================================
// FUNÇÕES DE GERENCIAMENTO DE DADOS
// =============================================================================

/**
 * @brief Salva dados localmente quando em modo offline
 * 
 * @details Armazena dados dos sensores na NVS quando não há conexão
 * com o Firebase. Os dados são enviados posteriormente quando a
 * conexão for restabelecida.
 * 
 * @note Utiliza carimbo de tempo atual para timestamp dos dados
 * 
 * @see FirebaseHandler::saveDataLocally()
 * @see FirebaseHandler::getCurrentTimestamp()
 */
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

/**
 * @brief Envia dados para o histórico do Firebase
 * 
 * @details Envia leituras dos sensores para o histórico no Firebase.
 * Se offline, os dados são salvos localmente para envio posterior.
 * 
 * @return void
 * 
 * @see FirebaseHandler::sendDataToHistory()
 * @see saveDataLocally()
 */
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

// =============================================================================
// FUNÇÕES DE VERIFICAÇÃO DE CONEXÃO
// =============================================================================

/**
 * @brief Verifica e recupera status de conexão
 * 
 * @details Monitora periodicamente a conexão WiFi e Firebase,
 * tentando reconectar automaticamente em caso de falha.
 * 
 * @note Executada a cada 30 segundos
 * 
 * @see WiFi.reconnect()
 * @see FirebaseHandler::refreshTokenIfNeeded()
 * @see FirebaseHandler::sendLocalData()
 */
void verifyConnectionStatus() {
    static unsigned long lastCheck = 0;
    const unsigned long CHECK_INTERVAL = 30000;
    
    if (millis() - lastCheck > CHECK_INTERVAL) {
        lastCheck = millis();
        
        // Verifica e tenta recuperar conexão WiFi
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
                // Tenta enviar dados pendentes após reconexão
                if (firebase.isAuthenticated()) {
                    firebase.sendLocalData();
                }
            } else {
                Serial.println("❌ WiFi reconnection failed");
            }
        }
        
        // Verifica e recupera conexão Firebase
        if (firebase.isAuthenticated() && !Firebase.ready()) {
            Serial.println("⚠️ Firebase disconnected! Attempting to reconnect...");
            firebase.refreshTokenIfNeeded();
        }
    }
}

// =============================================================================
// FUNÇÕES PRINCIPAIS DE CONTROLE (EXECUTADAS NO LOOP)
// =============================================================================

/**
 * @brief Gerencia leitura periódica dos sensores
 * 
 * @details Executa a leitura de todos os sensores no intervalo definido
 * 
 * @see SensorController::update()
 * @see SENSOR_READ_INTERVAL
 */
void handleSensors() {
    if (millis() - lastSensorRead > SENSOR_READ_INTERVAL) {
        sensors.update();
        lastSensorRead = millis();
    }
}

/**
 * @brief Gerencia controle automático dos atuadores
 * 
 * @details Aplica lógica de controle baseada nas leituras dos sensores
 * e setpoints configurados
 * 
 * @see ActuatorController::controlAutomatically()
 * @see ACTUATOR_CONTROL_INTERVAL
 */
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

/**
 * @brief Gerencia comunicação com Firebase
 * 
 * @details Envia dados dos sensores, atualiza estados dos atuadores,
 * recebe setpoints e envia heartbeats periódicos
 * 
 * @see FirebaseHandler::sendSensorData()
 * @see FirebaseHandler::updateActuatorState()
 * @see FirebaseHandler::receiveSetpoints()
 * @see FirebaseHandler::sendHeartbeat()
 */
void handleFirebase() {
    if (!firebase.isAuthenticated() || WiFi.status() != WL_CONNECTED) {
        return;
    }
    
    if (millis() - lastFirebaseUpdate > FIREBASE_UPDATE_INTERVAL) {
        // Envia dados dos sensores para Firebase
        firebase.sendSensorData(
            sensors.getTemperature(),
            sensors.getHumidity(),
            sensors.getCO2(),
            sensors.getCO(),
            sensors.getLight(),
            sensors.getTVOCs(),
            sensors.getWaterLevel()
        );
        
        // Atualiza estados dos atuadores no Firebase
        firebase.updateActuatorState(
            actuators.getRelayState(1),
            actuators.getRelayState(2),
            actuators.getRelayState(3),
            actuators.getRelayState(4),
            actuators.areLEDsOn(),
            actuators.getLEDsWatts(),
            actuators.isHumidifierOn()
        );
        
        // Recebe setpoints e comandos do Firebase
        firebase.receiveSetpoints(actuators);
        
        lastFirebaseUpdate = millis();
    }
    
    // Envia heartbeat periódico
    if (millis() - lastHeartbeat > HEARTBEAT_INTERVAL) {
        firebase.sendHeartbeat();
        lastHeartbeat = millis();
    }
}

/**
 * @brief Gerencia histórico e dados locais
 * 
 * @details Controla o envio de dados para histórico e salvamento
 * local em backup
 * 
 * @see sendDataToHistory()
 * @see saveDataLocally()
 */
void handleHistoryAndLocalData() {
    // Envia dados para histórico em intervalo definido
    if (millis() - lastHistoryUpdate > HISTORY_UPDATE_INTERVAL) {
        sendDataToHistory();
        lastHistoryUpdate = millis();
    }
    
    // Salva dados localmente periodicamente (backup)
    if (millis() - lastLocalSave > LOCAL_SAVE_INTERVAL) {
        if (WiFi.status() != WL_CONNECTED || !firebase.isAuthenticated()) {
            saveDataLocally();
        }
        lastLocalSave = millis();
    }
}

// =============================================================================
// SETUP PRINCIPAL
// =============================================================================

/**
 * @brief Função de setup inicial do Arduino
 * 
 * @details Inicializa todos os componentes do sistema:
 * - Serial communication
 * - Tarefa do LED
 * - Sensores e atuadores
 * - Conexão WiFi e Firebase
 * - Geração do ID da estufa e QR Code
 * 
 * @note Executada uma vez na inicialização do ESP32
 * 
 * @see setupLEDTask()
 * @see setupSensorsAndActuators()
 * @see setupWiFiAndFirebase()
 * @see getMacAddress()
 * @see QRCodeGenerator::generateQRCode()
 */
void setup() {
    // Inicialização da comunicação serial
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n\n[SISTEMA] Iniciando Sistema IFungi Greenhouse...");
    
    // Inicialização das tarefas e componentes
    setupLEDTask();
    setupSensorsAndActuators();
    setupWiFiAndFirebase();

    // Geração do ID único da estufa
    greenhouseID = "IFUNGI-" + getMacAddress();
    Serial.println("[SISTEMA] ID da Estufa: " + greenhouseID);
    qrGenerator.generateQRCode(greenhouseID);

    Serial.println("[SISTEMA] Sistema inicializado e pronto para operação");
}

// =============================================================================
// LOOP PRINCIPAL
// =============================================================================

/**
 * @brief Função loop principal do Arduino
 * 
 * @details Executa continuamente todas as tarefas do sistema em intervalos
 * controlados. Implementa uma máquina de estados sem bloqueio.
 * 
 * @note Todas as funções são não-bloqueantes e baseadas em timestamps
 * 
 * @see handleSensors()
 * @see handleActuators()
 * @see handleFirebase()
 * @see handleHistoryAndLocalData()
 * @see verifyConnectionStatus()
 * @see handleDebugAndCalibration()
 */
void loop() {
    // Executa todas as tarefas periódicas
    handleSensors();
    handleActuators();
    handleFirebase();
    handleHistoryAndLocalData();
    verifyConnectionStatus();
    handleDebugAndCalibration();
    
    // Pequeno delay para estabilidade do sistema
    delay(10);
}