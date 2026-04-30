/**
 * @file MainController.cpp
 * @brief Controlador principal do sistema IFungi Greenhouse
 * @author Vinicius Alexandre Merlak
 * @date 2026
 * @version 1.1
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
#include "OTAHandler.h"
#include "LEDScheduler.h"
#include "OperationMode.h"
#include <WiFiManager.h>

// Validação em tempo de compilação — abortam o build se o .env estiver incompleto
// Nota: IFUNGI_FIREBASE_EMAIL e IFUNGI_FIREBASE_PASSWORD NÃO estão aqui
//       pois são credenciais de runtime inseridas via rede cativa, não build-time.
#ifndef IFUNGI_WIFI_AP_NAME
    #error "IFUNGI_WIFI_AP_NAME nao definida. Verifique seu arquivo .env"
#endif
#ifndef IFUNGI_WIFI_AP_PASSWORD
    #error "IFUNGI_WIFI_AP_PASSWORD nao definida. Verifique seu arquivo .env"
#endif
#ifndef IFUNGI_OTA_PASSWORD
    #error "IFUNGI_OTA_PASSWORD nao definida. Verifique seu arquivo .env"
#endif

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

/// @brief Handler de atualização OTA remota
OTAHandler otaHandler;                          // ← OTA: instância global

/// @brief Versão atual do firmware — incremente a cada release
const String FIRMWARE_VERSION = "1.0.0";       // ← OTA: versão atual

/// @brief ID único da estufa baseado no MAC address
String greenhouseID;

/// @brief Flag para indicar se rede cativa está ativa
volatile bool wifiConfigPortalActive = false;

/// @brief Timestamp da última tentativa de reconexão WiFi
unsigned long lastWiFiReconnectAttempt = 0;

/// @brief Intervalo para tentar reconectar WiFi (30 segundos)
const unsigned long WIFI_RECONNECT_INTERVAL = 30000;

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

/// @brief Timestamp do último auto-repair do banco Firebase
unsigned long lastRepairCheck = 0;

/// @brief Timestamp da última leitura do modo de operação
unsigned long lastOperationModeCheck = 0;

/**
 * @def REPAIR_CHECK_INTERVAL
 * @brief Intervalo para verificação de integridade do banco (5 minutos)
 */
const unsigned long REPAIR_CHECK_INTERVAL = 300000;

/**
 * @def OPERATION_MODE_CHECK_INTERVAL
 * @brief Intervalo para verificação do modo de operação no Firebase (5 segundos)
 */
const unsigned long OPERATION_MODE_CHECK_INTERVAL = 5000;

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
/// @brief Handle da tarefa FreeRTOS de suporte de vida durante rede cativa
TaskHandle_t lifeSupportTaskHandle = NULL;
/// @brief Flag de execução da task de suporte de vida
volatile bool lifeSupportTaskRunning = false;

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
// TAREFA DE SUPORTE DE VIDA (DURANTE PORTAL CAPTIVO)
// =============================================================================

/**
 * @brief Mantém controle básico da estufa durante configuração de rede cativa
 *
 * @details Executa leitura de sensores e controle automático de atuadores em
 * paralelo ao WiFiManager quando ele estiver bloqueado no setup.
 * Não depende de Firebase e prioriza continuidade do ambiente interno.
 */
void lifeSupportTask(void * parameter) {
    unsigned long lastSensorTs = 0;
    unsigned long lastActTs = 0;

    for (;;) {
        if (!lifeSupportTaskRunning) {
            vTaskDelay(100 / portTICK_PERIOD_MS);
            continue;
        }

        unsigned long now = millis();

        if (now - lastSensorTs > SENSOR_READ_INTERVAL) {
            sensors.update();
            lastSensorTs = now;
        }

        if (now - lastActTs > ACTUATOR_CONTROL_INTERVAL) {
            // Keep LED schedule behavior consistent during captive portal / boot fallback.
            actuators.applyLEDSchedule(firebase.getCurrentTimestamp());
            actuators.controlAutomatically(
                sensors.getTemperature(),
                sensors.getHumidity(),
                sensors.getLight(),
                sensors.getCO(),
                sensors.getCO2(),
                sensors.getTVOCs(),
                sensors.getWaterLevel()
            );
            lastActTs = now;
        }

        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

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
            
            Serial.println(currentDebugMode ? "[debug] DEBUG MODE ENABLED" : "[debug] DEBUG MODE DISABLED");
            
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
                Serial.println("[debug] Updated actuator states after exiting debug mode");
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

            // Proteção de combinação inválida para Peltier:
            // R2 não pode ficar ON se R1 estiver OFF (R1 OFF + R2 ON não deve ser aceito).
            // Observação: R1 ON + R2 OFF é válido (resfriamento), conforme sua regra.
            if (!relay1 && relay2) {
                relay2 = false;
                String fixPath = "/greenhouses/" + firebase.greenhouseId + "/manual_actuators/rele2";
                if (Firebase.setBool(firebase.fbdo, fixPath.c_str(), false)) {
                    Serial.println("[debug] Correção aplicada: rele2->false (rele1 estava false)");
                } else {
                    Serial.println("[debug] Falha ao corrigir rele2 inválido no Firebase");
                }
            }
            
            // Verifica se houve mudanças reais nos valores
            bool hasChanges = (relay1 != lastRelay1) || (relay2 != lastRelay2) || 
                             (relay3 != lastRelay3) || (relay4 != lastRelay4) ||
                             (ledsOn != lastLedsOn) || (ledsIntensity != lastLedsIntensity) ||
                             (humidifierOn != lastHumidifierOn);
            
            // Aplica mudanças se detectadas
            if (hasChanges) {
                Serial.println("[debug] Manual states changed, applying...");
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
// RECONEXÃO WIFI E FIREBASE PERIÓDICA
// =============================================================================

/**
 * @brief Tenta reconectar WiFi e Firebase periodicamente quando em modo life support
 *
 * @details Se o sistema estiver em modo suporte de vida (rede cativa ou desconectado),
 * tenta reconectar automaticamente a cada 30 segundos.
 */
void handleWiFiReconnection() {
    // Se já está conectado e autenticado, não faz nada
    if (WiFi.status() == WL_CONNECTED && firebase.isAuthenticated()) {
        return;
    }
    
    // Se estiver no portal de configuração, não tenta reconectar automaticamente
    if (wifiConfigPortalActive) {
        return;
    }
    
    // Tenta reconectar a cada 30 segundos
    if (millis() - lastWiFiReconnectAttempt > WIFI_RECONNECT_INTERVAL) {
        lastWiFiReconnectAttempt = millis();
        
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[wifi] Tentando reconectar WiFi...");
            WiFi.reconnect();
        } else if (!firebase.isAuthenticated()) {
            Serial.println("[firebase] Tentando reconectar Firebase...");
            
            String email, password;
            if (firebase.loadFirebaseCredentials(email, password)) {
                if (firebase.authenticate(email, password)) {
                    Serial.println("[firebase] Reconectado com sucesso!");
                    firebase.sendHeartbeat();
                    
                    // Se reconectou, pode desligar life support
                    lifeSupportTaskRunning = false;
                }
            }
        }
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
    
    Serial.println("[init] LED task initialized");
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
    Serial.println("[init] Initializing sensors and actuators...");
    
    // Inicialização dos sensores
    sensors.begin();
    
    // Inicialização dos atuadores com configuração de pinos
    // LED Pin: 4, Relés: 23, 14, 18, 19, Servo: 13
    actuators.begin(4, 23, 14, 18, 19, 13);
    
    // Carrega setpoints do NVS ou usa valores padrão
    if (!actuators.loadSetpointsNVS()) {
        Serial.println("[init] Using default setpoints");
        actuators.applySetpoints(5000, 20.0, 30.0, 60.0, 80.0, 50, 400, 100);
    }
    
    // Conecta atuadores ao handler do Firebase para sincronização
    actuators.setFirebaseHandler(&firebase);
    
    Serial.println("[init] Sensors and actuators initialized");
}

// =============================================================================
// CONFIGURAÇÃO DE REDE E FIREBASE
// =============================================================================

/**
 * @brief Configura conexão WiFi e autenticação Firebase com rede cativa como fallback
 * 
 * @details Gerencia todo o processo de conexão de rede e autenticação:
 * - Tenta conexão automática com WiFi salvo
 * - Se falhar, ativa rede cativa (AP) sem desligar life support
 * - Carrega credenciais Firebase do NVS
 * - Se não existem, ativa rede cativa para configurar
 * - Autentica no Firebase com 2 tentativas
 * - Se falhar, ativa rede cativa para reconfigurar
 * - Life support continua rodando durante toda a configuração
 * 
 * @note Sistema nunca reinicia por falha de WiFi/Firebase — sempre ativa rede cativa
 * 
 * @warning Em modo life support, apenas sensores e atuadores operam (sem Firebase)
 * 
 * @see WiFiManager
 * @see FirebaseHandler::authenticate()
 * @see FirebaseHandler::loadFirebaseCredentials()
 */
void setupWiFiAndFirebase() {
    Serial.println("[wifi] Iniciando conexao...");
    
    WiFiManager wm;
    wm.setConfigPortalTimeout(180);
    wm.setConnectTimeout(30);
    wm.setDebugOutput(true);
    
    // Tenta conexão automática com WiFi salvo
    if (!wm.autoConnect(IFUNGI_WIFI_AP_NAME, IFUNGI_WIFI_AP_PASSWORD)) {
        Serial.println("[wifi] Falha na conexao - ativando rede cativa para reconfiguracao");
        wifiConfigPortalActive = true;
        lifeSupportTaskRunning = true;
        wm.startConfigPortal(IFUNGI_WIFI_AP_NAME, IFUNGI_WIFI_AP_PASSWORD);
        wifiConfigPortalActive = false;
        
        // Se saiu do portal sem conectar, continua em life support
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[wifi] Continuando em modo suporte de vida");
            return;
        }
    }
    
    Serial.println("[wifi] WiFi conectado: " + WiFi.localIP().toString());
    delay(2000);
    
    // Carrega credenciais Firebase do NVS
    String email, password;
    if (!firebase.loadFirebaseCredentials(email, password)) {
        Serial.println("[firebase] Credenciais nao encontradas - rede cativa ativada");
        wifiConfigPortalActive = true;
        lifeSupportTaskRunning = true;
        wm.startConfigPortal(IFUNGI_WIFI_AP_NAME, IFUNGI_WIFI_AP_PASSWORD);
        wifiConfigPortalActive = false;
        return;
    }
    
    // Tenta autenticar no Firebase (máximo 2 tentativas)
    Serial.println("[firebase] Iniciando autenticacao...");
    bool firebaseAuthenticated = false;
    
    for (int i = 1; i <= 2; i++) {
        Serial.printf("[firebase] Tentativa %d/2\n", i);
        
        if (firebase.authenticate(email, password)) {
            firebaseAuthenticated = true;
            Serial.println("[firebase] Autenticacao bem-sucedida!");
            
            // Garante estrutura do banco
            firebase.ensureOTANodeExists();
            firebase.ensureLEDScheduleExists(actuators);
            firebase.ensureOperationModeExists(actuators);
            
            // Envia dados locais pendentes
            firebase.sendLocalData();
            firebase.sendHeartbeat();
            break;
        } else {
            Serial.printf("[firebase] Falha na autenticacao (tentativa %d/2)\n", i);
            
            if (i == 1) {
                Serial.println("[firebase] Possiveis causas:");
                Serial.println("   - Credenciais invalidas");
                Serial.println("   - Problema de conexao com internet");
                Serial.println("   - Problema de DNS");
            }
            
            if (i < 2) delay(3000);
        }
    }
    
    // Se falha autenticação, ativa rede cativa para reconfigurar credenciais
    if (!firebaseAuthenticated) {
        Serial.println("[firebase] Falha critica - rede cativa ativada para reconfiguracao");
        
        // Remove credenciais inválidas do NVS
        Preferences preferences;
        if (preferences.begin("firebase-creds", false)) {
            preferences.clear();
            preferences.end();
            Serial.println("[firebase] Credenciais invalidas removidas");
        }
        
        wifiConfigPortalActive = true;
        lifeSupportTaskRunning = true;
        wm.startConfigPortal(IFUNGI_WIFI_AP_NAME, IFUNGI_WIFI_AP_PASSWORD);
        wifiConfigPortalActive = false;
        return;
    }
    
    Serial.println("[system] WiFi e Firebase configurados com sucesso");
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
        Serial.println("[offline] Data saved locally (offline mode)");
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
            Serial.println("[firebase] Data sent to Firebase history");
        } else {
            Serial.println("[firebase] Failed to send data to history");
        }
    } else {
        Serial.println("[offline] Offline mode - data will be saved locally");
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
    static bool prevWifiUp = true;
    const unsigned long CHECK_INTERVAL = 30000;

    if (millis() - lastCheck > CHECK_INTERVAL) {
        lastCheck = millis();

        bool up = (WiFi.status() == WL_CONNECTED);

        if (!up) {
            Serial.println("[wifi] WiFi desconectado - WiFi.reconnect() (nova checagem em ~30s)");
            WiFi.reconnect();
        } else {
            if (!prevWifiUp) {
                Serial.println("[wifi] WiFi reconectado - enviando dados locais pendentes (NVS)");
                if (firebase.isAuthenticated()) {
                    firebase.sendLocalData();
                }
            }
            if (firebase.isAuthenticated() && !Firebase.ready()) {
                Serial.println("[firebase] Firebase nao pronto - refreshTokenIfNeeded()");
                firebase.refreshTokenIfNeeded();
            }
        }
        prevWifiUp = up;
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
        // Atualiza o scheduler de LEDs com o timestamp atual
        actuators.applyLEDSchedule(firebase.getCurrentTimestamp());

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
        firebase.updateSensorHealth(
            sensors.isDHTHealthy(),
            sensors.isCCS811Healthy(),
            sensors.isMQ7Healthy(),
            sensors.isLDRHealthy(),
            sensors.isWaterLevelHealthy()
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

        // Recebe configuração do agendador de LEDs
        firebase.receiveLEDSchedule(actuators);
        
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
// MODO DE OPERAÇÃO
// =============================================================================

/**
 * @brief Verifica periodicamente o modo de operação no Firebase e aplica mudanças
 *
 * @details Lê /operation_mode/mode a cada 5 segundos. Se o app tiver mudado
 * o modo, o ActuatorController aplica o preset correspondente imediatamente.
 */
void handleOperationMode() {
    if (!firebase.isAuthenticated() || WiFi.status() != WL_CONNECTED) return;

    if (millis() - lastOperationModeCheck > OPERATION_MODE_CHECK_INTERVAL) {
        lastOperationModeCheck = millis();
        firebase.receiveOperationMode(actuators);
    }
}

// =============================================================================
// AUTO-REPAIR DO BANCO FIREBASE
// =============================================================================

/**
 * @brief Verifica e regenera campos ausentes no Firebase periodicamente
 *
 * @details Roda a cada 5 minutos. Mais leve que recriar o banco — só
 * adiciona o que estiver faltando (setpoints, led_schedule, ota, etc.)
 */
void handleRepairAndOTA() {
    if (!firebase.isAuthenticated() || WiFi.status() != WL_CONNECTED) return;

    if (millis() - lastRepairCheck > REPAIR_CHECK_INTERVAL) {
        lastRepairCheck = millis();
        firebase.repairMissingFields();
        firebase.ensureOTANodeExists();
        firebase.ensureLEDScheduleExists(actuators);
        firebase.ensureOperationModeExists(actuators);
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
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n\n[system] Iniciando Sistema IFungi Greenhouse...");
    
    setupLEDTask();
    setupSensorsAndActuators();

    // Inicia tarefa de suporte de vida - SEMPRE ativa
    lifeSupportTaskRunning = true;
    xTaskCreatePinnedToCore(
        lifeSupportTask,
        "LifeSupport_Task",
        4096,
        NULL,
        1,
        &lifeSupportTaskHandle,
        1
    );

    // Tenta configurar WiFi e Firebase
    setupWiFiAndFirebase();
    
    // Se conseguiu autenticar, desliga life support (loop principal assume controle)
    if (firebase.isAuthenticated()) {
        lifeSupportTaskRunning = false;
    }
    // Se não, lifeSupportTaskRunning permanece TRUE e continua rodando

    greenhouseID = "IFUNGI-" + getMacAddress();
    Serial.println("[system] ID da Estufa: " + greenhouseID);
    qrGenerator.generateQRCode(greenhouseID);

    otaHandler.begin(&firebase, greenhouseID, FIRMWARE_VERSION, 60000);

    Serial.println("[system] Sistema inicializado e pronto para operação");
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
    handleOperationMode();          // ← verifica modo de operação a cada 5s
    otaHandler.handle();            // ← OTA: verifica atualizações a cada 60s
    handleRepairAndOTA();           // ← verifica integridade do banco a cada 5min
    handleWiFiReconnection();       // ← tenta reconectar periodicamente
    
    // Pequeno delay para estabilidade do sistema
    delay(10);
}