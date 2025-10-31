#include <Arduino.h>
#include "FirebaseHandler.h"
#include "SensorController.h"
#include "ActuatorController.h"
#include "genQrCode.h"
#include <WiFiManager.h>

// Instâncias globais
WiFiManager wifiManager;
FirebaseHandler firebase;
SensorController sensors;
ActuatorController actuators;
GenQR qrcode;

String ifungiID;

// Variáveis de timing
unsigned long lastSensorRead = 0;
unsigned long lastActuatorControl = 0;
unsigned long lastFirebaseUpdate = 0;
unsigned long lastHeartbeat = 0;
unsigned long lastHistoricoUpdate = 0;
unsigned long lastLocalSave = 0;

const unsigned long SENSOR_INTERVAL = 2000;
const unsigned long ACTUATOR_INTERVAL = 5000;
const unsigned long FIREBASE_INTERVAL = 5000;
const unsigned long HEARTBEAT_INTERVAL = 30000;
const unsigned long HISTORICO_INTERVAL = 300000; // 5 minutos
const unsigned long LOCAL_SAVE_INTERVAL = 60000; // 1 minuto

// Task handle para o LED
TaskHandle_t ledTaskHandle = NULL;

// Função da task do LED
void ledTask(void * parameter) {
    unsigned long lastBlink = 0;
    int blinkState = 0;
    int blinkPattern = 0; // 0=desligado, 1=rapido, 2=lento, 3=permanente
    unsigned long blinkInterval = 1000;
    
    for(;;) {
        // Atualiza padrão baseado no estado da conexão
        if (!WiFi.isConnected()) {
            blinkPattern = 0; // Desligado
        } else if (WiFi.getMode() == WIFI_AP) {
            blinkPattern = 1; // Piscar rápido (modo AP)
            blinkInterval = 500;
        } else if (WiFi.status() == WL_CONNECTED) {
            if (firebase.isAuthenticated()) {
                blinkPattern = 3; // Permanente (conectado e autenticado)
            } else {
                blinkPattern = 2; // Piscar lento (conectado mas não autenticado)
                blinkInterval = 1000;
            }
        } else {
            blinkPattern = 0; // Desligado
        }
        
        // Executa o padrão de piscada
        switch (blinkPattern) {
            case 0: // Desligado
                digitalWrite(LED_BUILTIN, LOW);
                break;
                
            case 1: // Piscar rápido (modo AP)
            case 2: // Piscar lento (não autenticado)
                if (millis() - lastBlink > blinkInterval) {
                    digitalWrite(LED_BUILTIN, blinkState);
                    blinkState = !blinkState;
                    lastBlink = millis();
                }
                break;
                
            case 3: // Permanente (conectado e autenticado)
                digitalWrite(LED_BUILTIN, HIGH);
                break;
        }
        
        vTaskDelay(50 / portTICK_PERIOD_MS); // Pequeno delay para não sobrecarregar
    }
}

// Inicializa a task do LED
void setupLEDTask() {
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);
    
    xTaskCreatePinnedToCore(
        ledTask,           // Função da task
        "LED_Task",        // Nome da task
        2048,              // Stack size
        NULL,              // Parâmetros
        1,                 // Prioridade (baixa)
        &ledTaskHandle,    // Task handle
        0                  // Core (0 ou 1)
    );
    
    Serial.println("✅ Task do LED inicializada no core " + String(xPortGetCoreID()));
}

void setupSensorsAndActuators() {
    Serial.println("🔧 Inicializando sensores e atuadores...");
    
    sensors.begin();
    actuators.begin(4, 23, 14, 18, 19, 13);
    
    if (!actuators.carregarSetpointsNVS()) {
        Serial.println("⚙️  Usando setpoints padrão");
        actuators.aplicarSetpoints(5000, 20.0, 30.0, 60.0, 80.0, 400, 400, 100);
    }
    
    actuators.setFirebaseHandler(&firebase);
    Serial.println("✅ Sensores e atuadores inicializados");
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
        Serial.println("⚠️  Sinal WiFi fraco (RSSI: " + String(WiFi.RSSI()) + " dBm)");
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
            
            // Verifica se a estufa existe/cria se necessário
            firebase.verificarEstufa();
            
            // Tenta enviar dados locais pendentes
            firebase.enviarDadosLocais();
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
            Serial.println("🗑️  Removendo credenciais inválidas do NVS...");
            Preferences preferences;
            if (preferences.begin("firebase-creds", false)) {
                preferences.clear();
                preferences.end();
                Serial.println("✅ Credenciais inválidas removidas");
            }
        }
        
        Serial.println("🌐 Por favor, reconfigure as credenciais via portal web:");
        Serial.println("   http://" + WiFi.localIP().toString());
        Serial.println("⚠️  O sistema funcionará em modo offline até a configuração");
        
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
        firebase.enviarHeartbeat();
        Serial.println("💓 Heartbeat inicial enviado");
    }
}

// Função para salvar dados localmente quando offline
void salvarDadosLocalmente() {
    if (WiFi.status() != WL_CONNECTED || !firebase.isAuthenticated()) {
        // Salva dados localmente
        unsigned long timestamp = firebase.getCurrentTimestamp();
        firebase.salvarDadosLocalmente(
            sensors.getTemperature(),
            sensors.getHumidity(),
            sensors.getCO2(),
            sensors.getCO(),
            sensors.getLight(),
            sensors.getTVOCs(),
            timestamp
        );
        Serial.println("💾 Dados salvos localmente (modo offline)");
    }
}

// Função para enviar dados ao histórico
void enviarDadosParaHistorico() {
    if (WiFi.status() == WL_CONNECTED && firebase.isAuthenticated()) {
        bool enviado = firebase.enviarDadosParaHistorico(
            sensors.getTemperature(),
            sensors.getHumidity(),
            sensors.getCO2(),
            sensors.getCO(),
            sensors.getLight(),
            sensors.getTVOCs()
        );
        
        if (enviado) {
            Serial.println("📊 Dados enviados para histórico Firebase");
        } else {
            Serial.println("❌ Falha ao enviar dados para histórico");
        }
    } else {
        Serial.println("📴 Modo offline - dados serão salvos localmente");
        salvarDadosLocalmente();
    }
}

// Função auxiliar para verificar conexão periódica
void verifyConnectionStatus() {
    static unsigned long lastCheck = 0;
    const unsigned long CHECK_INTERVAL = 30000; // 30 segundos
    
    if (millis() - lastCheck > CHECK_INTERVAL) {
        lastCheck = millis();
        
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("⚠️  WiFi desconectado! Tentando reconectar...");
            WiFi.reconnect();
            
            int reconnectAttempts = 0;
            while (WiFi.status() != WL_CONNECTED && reconnectAttempts < 5) {
                delay(1000);
                reconnectAttempts++;
            }
            
            if (WiFi.status() == WL_CONNECTED) {
                Serial.println("✅ WiFi reconectado");
                // Tenta enviar dados locais pendentes após reconexão
                if (firebase.isAuthenticated()) {
                    firebase.enviarDadosLocais();
                }
            } else {
                Serial.println("❌ Falha na reconexão WiFi");
            }
        }
        
        if (firebase.isAuthenticated() && !Firebase.ready()) {
            Serial.println("⚠️  Firebase desconectado! Tentando reconectar...");
            firebase.refreshToken();
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
        actuators.controlarAutomaticamente(
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
        firebase.enviarDadosSensores(
            sensors.getTemperature(),
            sensors.getHumidity(),
            sensors.getCO2(),
            sensors.getCO(),
            sensors.getLight(),
            sensors.getTVOCs(),
            sensors.getWaterLevel()
        );
        firebase.atualizarEstadoAtuadores(
            actuators.getReleState(1),
            actuators.getReleState(2),
            actuators.getReleState(3),
            actuators.getReleState(4),
            actuators.areLEDsOn(),
            actuators.getLEDsWatts(),
            actuators.isUmidificadorOn()
        );
        
        firebase.verificarComandos(actuators);
        firebase.RecebeSetpoint(actuators);
        
        lastFirebaseUpdate = millis();
    }
    
    if (millis() - lastHeartbeat > HEARTBEAT_INTERVAL) {
        firebase.enviarHeartbeat();
        lastHeartbeat = millis();
    }
}

// Nova função para gerenciar histórico e dados locais
void handleHistoricoEDadosLocais() {
    // Envia dados para histórico a cada intervalo definido
    if (millis() - lastHistoricoUpdate > HISTORICO_INTERVAL) {
        enviarDadosParaHistorico();
        lastHistoricoUpdate = millis();
    }
    
    // Salva dados localmente periodicamente (backup)
    if (millis() - lastLocalSave > LOCAL_SAVE_INTERVAL) {
        if (WiFi.status() != WL_CONNECTED || !firebase.isAuthenticated()) {
            salvarDadosLocalmente();
        }
        lastLocalSave = millis();
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n\n🚀 Iniciando IFungi System...");
    
    // Inicializa a task do LED primeiro
    setupLEDTask();
    
    // Configuração inicial
    setupSensorsAndActuators();
    setupWiFiAndFirebase();

    // Gera ID da estufa
    ifungiID = "IFUNGI-" + getMacAddress();
    Serial.println("🏷️  ID da Estufa: " + ifungiID);
    qrcode.generateQRCode(ifungiID);

    Serial.println("✅ Sistema inicializado e pronto!");
}

void loop() {
    // O LED agora é controlado pela task separada - REMOVA a chamada updateStatusLED()
    
    // Executa tarefas periódicas
    handleSensors();
    handleActuators();
    handleFirebase();
    handleHistoricoEDadosLocais();
    verifyConnectionStatus();
    
    // Pequeno delay para estabilidade
    delay(10);
}