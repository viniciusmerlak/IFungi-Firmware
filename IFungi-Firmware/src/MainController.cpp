/**
 * @file MainController.cpp
 * @brief Controlador principal do sistema IFungi Greenhouse
 * @version 1.3.0
 * @date 2026
 *
 * CORREÇÕES (v1.2.2):
 *  - BUG WATCHDOG / AUTENTICAÇÃO: authenticate() usava delay(500) num loop de
 *    até 30 s durante Firebase.begin(). Com lifeSupportTask ativa no core 0,
 *    o TWDT (Task Watchdog Timer) disparava e reiniciava o ESP32 antes que a
 *    autenticação completasse — o log cortava após "[firebase] Autenticando".
 *    Corrigido: delay() → vTaskDelay() e adicionado token_status_callback para
 *    alimentar o watchdog e ceder ao scheduler do FreeRTOS durante a espera.
 *  - BUG LIFE SUPPORT: lifeSupportTask movida para core 0 para não competir
 *    com o protocolo 1-Wire do DHT22 (core 1). Mutex adicionado para proteger
 *    sensors.update() de chamadas concorrentes.
 *  - BUG LIFE SUPPORT vs LOOP: quando não havia credenciais Firebase, a flag
 *    lifeSupportTaskRunning permanecia true e ambos (task + loop) chamavam
 *    handleSensors() e handleActuators() concorrentemente. Agora:
 *    * lifeSupportTask SÓ roda quando a flag está true (modo offline/portal).
 *    * O loop principal SÓ chama handleSensors/handleActuators quando a task
 *      NÃO está ativa. Isso elimina a corrida de dados completamente.
 *  - BUG RECONNECT: handleWiFiReconnection() não tentava mais carregar credenciais
 *    após NVS erase porque loadFirebaseCredentials() abria com mode=true e falha
 *    silenciosamente. Corrigido na camada de loadFirebaseCredentials (GreenhouseSystem).
 *  - CORREÇÃO: controlAutomatically() passa sensors.isDHTHealthy() para bloquear
 *    Peltier quando o DHT22 está inoperante.
 *  - CORREÇÃO: RemoteLogger::flush() chamado no loop principal.
 */

#include <Arduino.h>
#include "GreenhouseSystem.h"
#include "SensorController.h"
#include "ActuatorController.h"
#include "QRCodeGenerator.h"
#include "OTAHandler.h"
#include "LEDScheduler.h"
#include "OperationMode.h"
#include "RemoteLogger.h"
#include <WiFiManager.h>
#include <Preferences.h>

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
// VARIÁVEIS GLOBAIS
// =============================================================================

// CORRIGIDO: WiFiManager é instanciado localmente em setupWiFiAndFirebase().
// A declaração global estava duplicada e causava inicialização dupla do WiFi stack.

FirebaseHandler    firebase;
SensorController   sensors;
ActuatorController actuators;
QRCodeGenerator    qrGenerator;
OTAHandler         otaHandler;

const String FIRMWARE_VERSION = "1.3.0";

String greenhouseID;

volatile bool wifiConfigPortalActive = false;
unsigned long lastWiFiReconnectAttempt = 0;
const unsigned long WIFI_RECONNECT_INTERVAL = 30000;

// Mutex para proteger sensors.update() de chamadas concorrentes
// (lifeSupportTask no core 0 vs handleSensors no loop no core 1)
SemaphoreHandle_t sensorMutex = nullptr;

// =============================================================================
// TEMPORIZAÇÃO
// =============================================================================

unsigned long lastSensorRead         = 0;
unsigned long lastActuatorControl    = 0;
unsigned long lastFirebaseUpdate     = 0;
unsigned long lastHeartbeat          = 0;
unsigned long lastHistoryUpdate      = 0;
unsigned long lastLocalSave          = 0;
unsigned long lastRepairCheck        = 0;
unsigned long lastOperationModeCheck = 0;

const unsigned long REPAIR_CHECK_INTERVAL        = 300000;
const unsigned long OPERATION_MODE_CHECK_INTERVAL = 5000;
const unsigned long SENSOR_READ_INTERVAL          = 2000;
const unsigned long ACTUATOR_CONTROL_INTERVAL     = 5000;
const unsigned long FIREBASE_UPDATE_INTERVAL      = 5000;
const unsigned long HEARTBEAT_INTERVAL            = 30000;
const unsigned long HISTORY_UPDATE_INTERVAL       = 300000;
const unsigned long LOCAL_SAVE_INTERVAL           = 60000;

// =============================================================================
// HANDLES DE TAREFAS
// =============================================================================

TaskHandle_t ledTaskHandle         = NULL;
TaskHandle_t lifeSupportTaskHandle = NULL;

// BUG CORRIGIDO: agora é a única flag de controle da task de suporte de vida.
// O loop principal verifica esta flag antes de chamar handleSensors/handleActuators
// para evitar corrida de dados com a task.
volatile bool lifeSupportTaskRunning = false;

bool lastDebugMode   = false;
unsigned long lastDebugCheck = 0;
const unsigned long DEBUG_CHECK_INTERVAL = 2000;

// =============================================================================
// TAREFA DE SUPORTE DE VIDA (DURANTE PORTAL CAPTIVO / OFFLINE)
//
// CORRIGIDO v1.2.0:
//  - Roda no core 0 para não interferir com 1-Wire do DHT22 (core 1).
//  - Usa mutex para serializar acesso ao SensorController.
//  - Fica em loop de espera quando lifeSupportTaskRunning=false, sem consumir
//    CPU (vTaskDelay mantém o RTOS funcionando normalmente).
//
// RESPONSABILIDADE: manter sensores e atuadores ativos enquanto o sistema está
// aguardando WiFi/Firebase (portal cativo, reconexão, credenciais inválidas).
// Assim a estufa não para de funcionar durante configuração ou falha de rede.
// =============================================================================

void lifeSupportTask(void* parameter) {
    unsigned long lastSensorTs = 0;
    unsigned long lastActTs    = 0;

    for (;;) {
        if (!lifeSupportTaskRunning) {
            // Task inativa — yield frequente para não desperdiçar CPU
            vTaskDelay(100 / portTICK_PERIOD_MS);
            continue;
        }

        unsigned long now = millis();

        if (now - lastSensorTs > SENSOR_READ_INTERVAL) {
            // Mutex: evita que lifeSupportTask e handleSensors(loop) chamem
            // sensors.update() ao mesmo tempo — DHT22 é single-threaded.
            if (xSemaphoreTake(sensorMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                sensors.update();
                xSemaphoreGive(sensorMutex);
            }
            lastSensorTs = now;
        }

        if (now - lastActTs > ACTUATOR_CONTROL_INTERVAL) {
            actuators.applyLEDSchedule(firebase.getCurrentTimestamp());
            actuators.controlAutomatically(
                sensors.getTemperature(),
                sensors.getHumidity(),
                sensors.getLight(),
                sensors.getCO(),
                sensors.getCO2(),
                sensors.getTVOCs(),
                sensors.getWaterLevel(),
                sensors.isDHTHealthy(),
                false   // allowFirebaseWrite=false: Firebase (TLS/lwip) não pode ser
                        // chamado de uma FreeRTOS task fora da loopTask (pthread TLS inválido)
            );
            lastActTs = now;
        }

        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

// =============================================================================
// DEBUG E CALIBRAÇÃO
// =============================================================================

void handleDebugAndCalibration() {
    if (millis() - lastDebugCheck > DEBUG_CHECK_INTERVAL) {
        lastDebugCheck = millis();

        bool currentDebugMode = false;

        if (firebase.isAuthenticated() && firebase.isFirebaseReady()) {
            currentDebugMode = firebase.getDebugMode();
        }

        if (currentDebugMode != lastDebugMode) {
            actuators.setDebugMode(currentDebugMode);
            lastDebugMode = currentDebugMode;

            Serial.println(currentDebugMode ? "[debug] DEBUG MODE ENABLED" : "[debug] DEBUG MODE DISABLED");

            if (!currentDebugMode && firebase.isAuthenticated()) {
                delay(500);
                firebase.updateActuatorState(
                    actuators.getRelayState(1),
                    actuators.getRelayState(2),
                    actuators.getRelayState(3),
                    actuators.getRelayState(4),
                    actuators.areLEDsOn(),
                    actuators.getLEDsWatts(),
                    actuators.isHumidifierOn()
                );
            }
        }

        if (currentDebugMode && firebase.isAuthenticated() && firebase.isFirebaseReady()) {
            bool analogReadMode, digitalWriteMode, pwm;
            int pin, pwmValue;
            firebase.getDevModeSettings(analogReadMode, digitalWriteMode, pin, pwm, pwmValue);
            actuators.setDevModeSettings(analogReadMode, digitalWriteMode, pin, pwm, pwmValue);

            static bool lastRelay1 = false, lastRelay2 = false,
                        lastRelay3 = false, lastRelay4 = false;
            static bool lastLedsOn = false;
            static int  lastLedsIntensity = 0;
            static bool lastHumidifierOn  = false;

            bool relay1, relay2, relay3, relay4, ledsOn, humidifierOn;
            int  ledsIntensity;
            firebase.getManualActuatorStates(relay1, relay2, relay3, relay4,
                                             ledsOn, ledsIntensity, humidifierOn);

            if (!relay1 && relay2) {
                relay2 = false;
                String fixPath = "/greenhouses/" + firebase.greenhouseId + "/manual_actuators/rele2";
                Firebase.setBool(firebase.fbdo, fixPath.c_str(), false);
                Serial.println("[debug] Correção aplicada: rele2->false (rele1 estava false)");
            }

            bool hasChanges = (relay1 != lastRelay1) || (relay2 != lastRelay2) ||
                              (relay3 != lastRelay3) || (relay4 != lastRelay4) ||
                              (ledsOn != lastLedsOn) || (ledsIntensity != lastLedsIntensity) ||
                              (humidifierOn != lastHumidifierOn);

            if (hasChanges) {
                actuators.setManualStates(relay1, relay2, relay3, relay4,
                                          ledsOn, ledsIntensity, humidifierOn);
                lastRelay1        = relay1;
                lastRelay2        = relay2;
                lastRelay3        = relay3;
                lastRelay4        = relay4;
                lastLedsOn        = ledsOn;
                lastLedsIntensity = ledsIntensity;
                lastHumidifierOn  = humidifierOn;
            }
        }

        actuators.handleDevMode();
    }
}

// =============================================================================
// RECONEXÃO WIFI/FIREBASE
// =============================================================================

void handleWiFiReconnection() {
    if (WiFi.status() == WL_CONNECTED && firebase.isAuthenticated()) return;
    if (wifiConfigPortalActive) return;

    if (millis() - lastWiFiReconnectAttempt > WIFI_RECONNECT_INTERVAL) {
        lastWiFiReconnectAttempt = millis();

        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[wifi] Tentando reconectar WiFi...");
            WiFi.reconnect();
        } else if (!firebase.isAuthenticated()) {
            Serial.println("[firebase] Tentando reconectar Firebase...");
            String email, password;
            // loadFirebaseCredentials() retorna false silenciosamente se não há credenciais,
            // sem causar spam de erros NOT_FOUND no log.
            if (firebase.loadFirebaseCredentials(email, password)) {
                if (firebase.authenticate(email, password)) {
                    Serial.println("[firebase] Reconectado com sucesso!");
                    firebase.initLogger(LOG_INFO);
                    firebase.sendHeartbeat();
                    // Para a task de suporte de vida — o loop principal assume o controle
                    lifeSupportTaskRunning = false;
                }
            }
        }
    }
}

// =============================================================================
// LED INDICADOR DE STATUS
// =============================================================================

void ledTask(void* parameter) {
    unsigned long lastBlinkTime = 0;
    int blinkState    = 0;
    int blinkPattern  = 0;
    unsigned long blinkInterval = 1000;

    for (;;) {
        if (!WiFi.isConnected()) {
            blinkPattern = 0;
        } else if (WiFi.getMode() == WIFI_AP) {
            blinkPattern  = 1;
            blinkInterval = 200;
        } else if (WiFi.status() == WL_CONNECTED) {
            if (firebase.isAuthenticated()) {
                blinkPattern = 3;
            } else {
                blinkPattern  = 2;
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
                if (millis() - lastBlinkTime > blinkInterval) {
                    digitalWrite(LED_BUILTIN, blinkState);
                    blinkState    = !blinkState;
                    lastBlinkTime = millis();
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
    xTaskCreatePinnedToCore(ledTask, "LED_Task", 2048, NULL, 1, &ledTaskHandle, 0);
    Serial.println("[init] LED task initialized");
}

// =============================================================================
// SENSORES E ATUADORES
// =============================================================================

void setupSensorsAndActuators() {
    Serial.println("[init] Initializing sensors and actuators...");

    sensors.begin();
    actuators.begin(4, 23, 14, 18, 19, 13);

    // BUG CORRIGIDO v1.2.1: quando loadSetpointsNVS() falha (namespace não existe
    // ainda, ou flash recém-apagada), NÃO gravamos os defaults na NVS.
    // Gravar defaults aqui causava o seguinte ciclo vicioso:
    //   1. NVS vazia → loadSetpointsNVS() falha
    //   2. applySetpoints(defaults) → saveSetpointsNVS() salva defaults na NVS
    //   3. receiveSetpoints() lê Firebase (ex: tMax=23.0) e compara com statics
    //      internos que também são defaults (tMax=30.0) → changed=true → aplica
    //      E salva na NVS → OK neste boot
    //   4. Porém: se o OTA reinicia com NVS válida, loadSetpointsNVS() CARREGA
    //      os valores corretos (tMax=23.0). Aí receiveSetpoints() compara com
    //      statics default (tMax=30.0) → changed=true → aplica Firebase → OK.
    //   O real problema: applySetpoints com persistToNVS=false mantém valores
    //   em RAM mas não contamina a NVS com defaults. Na próxima leitura do
    //   Firebase, receiveSetpoints() sempre vai aplicar porque os statics
    //   internos != Firebase, garantindo que os valores do usuário prevaleçam.
    if (!actuators.loadSetpointsNVS()) {
        Serial.println("[init] NVS sem setpoints — usando defaults temporarios (Firebase prevalecera)");
        actuators.applySetpoints(5000, 20.0f, 30.0f, 60.0f, 80.0f, 50, 400, 100,
                                 false);  // ← persistToNVS=false: não contamina NVS
    }

    actuators.setFirebaseHandler(&firebase);
    Serial.println("[init] Sensors and actuators initialized");
}

// =============================================================================
// WIFI E FIREBASE
//
// FLUXO DE SUPORTE DE VIDA (life support):
//  1. lifeSupportTask é criada no setup() e começa com lifeSupportTaskRunning=true.
//  2. Durante setupWiFiAndFirebase(), o WiFiManager pode bloquear por até 180s
//     no portal cativo. A task mantém a estufa funcionando neste período.
//  3. Ao conectar e autenticar com sucesso, lifeSupportTaskRunning=false e o
//     loop principal assume handleSensors/handleActuators.
//  4. Se não houver credenciais ou autenticação falhar, a task continua ativa
//     e o loop principal PULA handleSensors/handleActuators (ver loop()).
//
// NOVIDADE: portal cativo tem campos de Email e Senha do Firebase.
// =============================================================================

void setupWiFiAndFirebase() {
    Serial.println("[wifi] Iniciando conexao...");

    WiFiManager wm;   // ← instância local; a duplicata global foi removida
    wm.setConfigPortalTimeout(180);
    wm.setConnectTimeout(30);
    wm.setDebugOutput(false);

    // Carrega credenciais existentes para pré-preencher o portal
    String savedEmail    = "";
    String savedPassword = "";
    firebase.loadFirebaseCredentials(savedEmail, savedPassword);

    WiFiManagerParameter paramEmail(
        "fb_email", "Email Firebase", savedEmail.c_str(), 64);
    WiFiManagerParameter paramPassword(
        "fb_password", "Senha Firebase", "", 64,
        "type='password' placeholder='(manter atual se vazio)'");

    wm.addParameter(&paramEmail);
    wm.addParameter(&paramPassword);

    if (!wm.autoConnect(IFUNGI_WIFI_AP_NAME, IFUNGI_WIFI_AP_PASSWORD)) {
        Serial.println("[wifi] Falha — ativando portal cativo para reconfiguracao");
        wifiConfigPortalActive = true;
        // lifeSupportTaskRunning já é true desde o setup()
        wm.startConfigPortal(IFUNGI_WIFI_AP_NAME, IFUNGI_WIFI_AP_PASSWORD);
        wifiConfigPortalActive = false;

        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[wifi] Sem WiFi — modo suporte de vida ativo");
            // lifeSupportTaskRunning permanece true; loop() vai pular handlers
            return;
        }
    }

    Serial.println("[wifi] WiFi conectado: " + WiFi.localIP().toString());
    delay(1000);

    // Salva credenciais digitadas no portal (se houver)
    String newEmail    = String(paramEmail.getValue());
    String newPassword = String(paramPassword.getValue());

    if (newEmail.length() > 3 && newEmail.indexOf('@') > 0) {
        if (newPassword.length() == 0) {
            newPassword = savedPassword;
        }

        if (newEmail != savedEmail || newPassword != savedPassword) {
            Preferences prefs;
            if (prefs.begin("firebase-creds", false)) {
                prefs.putString("email",    newEmail);
                prefs.putString("password", newPassword);
                prefs.end();
                Serial.println("[firebase] Credenciais atualizadas via portal cativo");
                savedEmail    = newEmail;
                savedPassword = newPassword;
            }
        }
    }

    // Carrega credenciais finais
    String email, password;
    if (!firebase.loadFirebaseCredentials(email, password)) {
        Serial.println("[firebase] Credenciais nao encontradas — portal cativo ativado");
        wifiConfigPortalActive = true;
        wm.startConfigPortal(IFUNGI_WIFI_AP_NAME, IFUNGI_WIFI_AP_PASSWORD);
        wifiConfigPortalActive = false;

        if (!firebase.loadFirebaseCredentials(email, password)) {
            Serial.println("[firebase] Sem credenciais — modo suporte de vida");
            // lifeSupportTaskRunning permanece true
            return;
        }
    }

    // Autentica no Firebase (até 2 tentativas)
    Serial.println("[firebase] Iniciando autenticacao...");
    bool firebaseAuthenticated = false;

    for (int i = 1; i <= 2; i++) {
        Serial.printf("[firebase] Tentativa %d/2\n", i);

        if (firebase.authenticate(email, password)) {
            firebaseAuthenticated = true;
            Serial.println("[firebase] Autenticacao bem-sucedida!");

            // CORREÇÃO v1.2.3: aguarda 500 ms para que o contexto SSL do
            // handshake de autenticação seja completamente desalocado do stack
            // antes de abrir uma nova conexão SSL em verifyGreenhouse().
            // Sem este yield, dois contextos TLS/RSA se sobrepõem no stack da
            // loopTask, causando "Stack canary watchpoint triggered" mesmo com
            // o stack aumentado.
            vTaskDelay(pdMS_TO_TICKS(500));

            // Verifica/cria a estrutura da estufa no Firebase.
            // DEVE ser chamado APÓS authenticate() retornar (stack SSL liberado).
            firebase.verifyGreenhouse();
            firebase.checkUserPermission(firebase.userUID, firebase.greenhouseId);

            // Yield extra antes de iniciar mais operações Firebase encadeadas
            vTaskDelay(pdMS_TO_TICKS(200));

            firebase.ensureOTANodeExists();
            firebase.ensureLEDScheduleExists(actuators);
            firebase.ensureOperationModeExists(actuators);
            firebase.initLogger(LOG_INFO);
            firebase.sendLocalData();
            firebase.sendHeartbeat();
            break;
        } else {
            Serial.printf("[firebase] Falha na autenticacao (tentativa %d/2)\n", i);
            if (i < 2) delay(3000);
        }
    }

    if (!firebaseAuthenticated) {
        Serial.println("[firebase] Falha critica — portal cativo para reconfigurar credenciais");

        // Remove credenciais inválidas para não tentar novamente com as mesmas
        Preferences prefs;
        if (prefs.begin("firebase-creds", false)) {
            prefs.clear();
            prefs.end();
            Serial.println("[firebase] Credenciais invalidas removidas da NVS");
        }

        wifiConfigPortalActive = true;
        wm.startConfigPortal(IFUNGI_WIFI_AP_NAME, IFUNGI_WIFI_AP_PASSWORD);
        wifiConfigPortalActive = false;
        // lifeSupportTaskRunning permanece true
        return;
    }

    // Autenticação bem-sucedida — entrega controle ao loop()
    Serial.println("[system] WiFi e Firebase configurados com sucesso");
    lifeSupportTaskRunning = false;
}

// =============================================================================
// GERENCIAMENTO DE DADOS
// =============================================================================

void saveDataLocally() {
    if (WiFi.status() != WL_CONNECTED || !firebase.isAuthenticated()) {
        firebase.saveDataLocally(
            sensors.getTemperature(),
            sensors.getHumidity(),
            sensors.getCO2(),
            sensors.getCO(),
            sensors.getLight(),
            sensors.getTVOCs(),
            firebase.getCurrentTimestamp()
        );
        Serial.println("[offline] Data saved locally");
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
        if (!sent) Serial.println("[firebase] Failed to send data to history");
    } else {
        saveDataLocally();
    }
}

// =============================================================================
// VERIFICAÇÃO DE CONEXÃO
// =============================================================================

void verifyConnectionStatus() {
    static unsigned long lastCheck = 0;
    static bool prevWifiUp = true;

    if (millis() - lastCheck > 30000) {
        lastCheck = millis();
        bool up = (WiFi.status() == WL_CONNECTED);

        if (!up) {
            Serial.println("[wifi] WiFi desconectado — reconnect()");
            WiFi.reconnect();
        } else {
            if (!prevWifiUp) {
                Serial.println("[wifi] WiFi reconectado — enviando dados pendentes");
                if (firebase.isAuthenticated()) firebase.sendLocalData();
            }
            if (firebase.isAuthenticated() && !Firebase.ready()) {
                firebase.refreshTokenIfNeeded();
            }
        }
        prevWifiUp = up;
    }
}

// =============================================================================
// LOOP — SENSORES
// =============================================================================

void handleSensors() {
    if (millis() - lastSensorRead > SENSOR_READ_INTERVAL) {
        if (xSemaphoreTake(sensorMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            sensors.update();
            xSemaphoreGive(sensorMutex);
        }
        lastSensorRead = millis();
    }
}

// =============================================================================
// LOOP — ATUADORES
// =============================================================================

void handleActuators() {
    if (millis() - lastActuatorControl > ACTUATOR_CONTROL_INTERVAL) {
        actuators.applyLEDSchedule(firebase.getCurrentTimestamp());

        actuators.controlAutomatically(
            sensors.getTemperature(),
            sensors.getHumidity(),
            sensors.getLight(),
            sensors.getCO(),
            sensors.getCO2(),
            sensors.getTVOCs(),
            sensors.getWaterLevel(),
            sensors.isDHTHealthy()   // ← CORREÇÃO: bloqueia Peltier se DHT falhou
        );

        lastActuatorControl = millis();
    }
}

// =============================================================================
// LOOP — FIREBASE
// =============================================================================

void handleFirebase() {
    if (!firebase.isAuthenticated() || WiFi.status() != WL_CONNECTED) return;

    if (millis() - lastFirebaseUpdate > FIREBASE_UPDATE_INTERVAL) {
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
        firebase.receiveLEDSchedule(actuators);

        lastFirebaseUpdate = millis();
    }

    if (millis() - lastHeartbeat > HEARTBEAT_INTERVAL) {
        firebase.sendHeartbeat();
        lastHeartbeat = millis();
    }
}

void handleHistoryAndLocalData() {
    if (millis() - lastHistoryUpdate > HISTORY_UPDATE_INTERVAL) {
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

void handleOperationMode() {
    if (!firebase.isAuthenticated() || WiFi.status() != WL_CONNECTED) return;

    if (millis() - lastOperationModeCheck > OPERATION_MODE_CHECK_INTERVAL) {
        lastOperationModeCheck = millis();
        OperationMode prevMode = actuators.getOperationMode();
        firebase.receiveOperationMode(actuators);
        OperationMode newMode = actuators.getOperationMode();
        if (newMode != prevMode) {
            RLOG_FMT(LOG_INFO, "[mode]", "Modo alterado: %s -> %s",
                     operationModeLabel(prevMode).c_str(),
                     operationModeLabel(newMode).c_str());
        }
    }
}

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
// SETUP
// =============================================================================

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n\n[system] Iniciando Sistema IFungi Greenhouse v" + FIRMWARE_VERSION);

    // Mutex deve ser criado antes de qualquer tarefa que use sensores
    sensorMutex = xSemaphoreCreateMutex();
    if (sensorMutex == nullptr) {
        Serial.println("[FATAL] Falha ao criar sensorMutex — sistema pode travar!");
    }

    setupLEDTask();

    // CORREÇÃO (v1.2.1): initializeNVS() ANTES de setupSensorsAndActuators().
    // loadSetpointsNVS() (chamado dentro de setupSensorsAndActuators) tentava abrir
    // o namespace "sensor_data" antes que initializeNVS() o criasse, resultando em
    // [E][Preferences.cpp:50] begin(): nvs_open failed: NOT_FOUND no primeiro boot.
    // Movendo a chamada para cá, a estrutura NVS já existe quando os atuadores
    // tentam carregar seus setpoints.
    firebase.initializeNVS();

    setupSensorsAndActuators();

    // LifeSupport no core 0 — DHT22 (1-Wire) fica livre no core 1
    // Começa ativo para cobrir o período de setupWiFiAndFirebase()
    lifeSupportTaskRunning = true;
    xTaskCreatePinnedToCore(
        lifeSupportTask,
        "LifeSupport_Task",
        4096,
        NULL,
        1,
        &lifeSupportTaskHandle,
        0   // ← core 0
    );

    setupWiFiAndFirebase();

    // Se autenticou com sucesso, lifeSupportTaskRunning já foi setado false
    // dentro de setupWiFiAndFirebase(). Caso contrário permanece true.

    greenhouseID = "IFUNGI-" + getMacAddress();
    Serial.println("[system] ID da Estufa: " + greenhouseID);
    qrGenerator.generateQRCode(greenhouseID);

    otaHandler.begin(&firebase, greenhouseID, FIRMWARE_VERSION, 60000);

    Serial.println("[system] Sistema inicializado e pronto para operacao");
    RLOG_FMT(LOG_INFO, "[system]", "Boot completo | ID: %s | FW: %s | IP: %s",
             greenhouseID.c_str(), FIRMWARE_VERSION.c_str(),
             WiFi.localIP().toString().c_str());
}

// =============================================================================
// LOOP
//
// CORREÇÃO CRÍTICA: handleSensors() e handleActuators() SÓ são chamados quando
// lifeSupportTaskRunning=false. Quando a task está ativa, ela é a única
// responsável por sensores e atuadores, eliminando a corrida de dados.
//
// Em modo offline/sem credenciais:
//  - lifeSupportTaskRunning permanece true
//  - loop() só executa: reconnect, OTA check (sem rede, é no-op) e RemoteLogger flush
//  - A estufa continua funcionando via lifeSupportTask com as últimas configurações
// =============================================================================

void loop() {
    // BUG CORRIGIDO: só chama handlers de sensor/atuador se a task NÃO está rodando.
    // Quando lifeSupportTaskRunning=true, a task cuida disso no core 0.
    if (!lifeSupportTaskRunning) {
        handleSensors();
        handleActuators();
        handleFirebase();
        handleHistoryAndLocalData();
        handleDebugAndCalibration();
        handleOperationMode();
        handleRepairAndOTA();
        verifyConnectionStatus();
        otaHandler.handle();
    }

    // Reconexão sempre ativa, independente do modo
    handleWiFiReconnection();

    RemoteLogger::flush();
    delay(10);
}