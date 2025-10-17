#include <Arduino.h>
#include "WiFiConfigurator.h"
#include "FirebaseHandler.h"
#include "WebServerHandler.h"
#include "SensorController.h"
#include "ActuatorController.h"
#include "perdiavontadedeviver.h"
#include "genQrCode.h"
#include "ansi.h"

ANSI ansi(&Serial);
const char* AP_SSID = "IFungi-Config";
const char* AP_PASSWORD = "config1234";

String ifungiID;
WiFiConfigurator wifiConfig;
FirebaseHandler firebase;
WebServerHandler webServer(wifiConfig, firebase);
GenQR qrcode;
SensorController sensors;
ActuatorController actuators;

// Variáveis de timing
unsigned long lastSensorRead = 0;
unsigned long lastActuatorControl = 0;
unsigned long lastFirebaseUpdate = 0;
unsigned long lastHeartbeat = 0;
unsigned long lastStatusUpdate = 0;

const unsigned long SENSOR_INTERVAL = 2000;
const unsigned long ACTUATOR_INTERVAL = 5000;
const unsigned long FIREBASE_INTERVAL = 5000;
const unsigned long HEARTBEAT_INTERVAL = 30000;
const unsigned long STATUS_INTERVAL = 1000;

void updateStatusLED() {
    static unsigned long lastBlink = 0;
    
    if (millis() - lastBlink < STATUS_INTERVAL) return;
    lastBlink = millis();
    
    if (firebase.isAuthenticated() && wifiConfig.isConnected()) {
        wifiConfig.piscaLED(true, 666666); // LED permanente
    } else if (!wifiConfig.isConnected()) {
        wifiConfig.piscaLED(true, 500); // Piscar rápido
    } else if (!firebase.isAuthenticated()) {
        wifiConfig.piscaLED(true, 777777); // Piscar duplo
    } else {
        wifiConfig.piscaLED(true, 2000); // Piscar lento
    }
}
void verificarHardware() {
    Serial.println("🔍 Verificando hardware...");
    
    // Testar pinos dos sensores
    pinMode(33, INPUT); // DHT22
    pinMode(32, INPUT); // CCS811
    pinMode(35, INPUT); // LDR
    pinMode(34, INPUT); // MQ-7
    pinMode(15, INPUT); // Nível água
    
    Serial.println("✅ Pinagem verificada");
    
    // Verificar tensão de alimentação
    int vcc = analogRead(34); // Usar pino analogico para verificar tensão
    Serial.printf("📊 Leitura de tensão: %d\n", vcc);
}
void setupSensorsAndActuators() {
    Serial.println("🔧 Inicializando sensores e atuadores...");
    
    sensors.begin();
    actuators.begin(4, 23, 14, 18, 19);
    
    if (!actuators.carregarSetpointsNVS()) {
        Serial.println("⚙️  Usando setpoints padrão");
        actuators.aplicarSetpoints(5000, 20.0, 30.0, 60.0, 80.0, 400, 400, 100);
    }
    
    actuators.setFirebaseHandler(&firebase);
    Serial.println("✅ Sensores e atuadores inicializados");
}

void setupWiFiAndFirebase() {
    Serial.println("🌐 Iniciando configuração de rede...");
    
    // Auto-conexão WiFi
    bool wifiConnected = wifiConfig.autoConnect(AP_SSID, AP_PASSWORD);
    
    if (wifiConnected) {
        Serial.println("✅ WiFi conectado! Iniciando serviços...");
        
        // Gera ID da estufa
        ifungiID = "IFUNGI-" + getMacAddress();
        Serial.println("🏷️  ID da Estufa: " + ifungiID);
        qrcode.generateQRCode(ifungiID);
        
        // Inicia servidor web
        webServer.begin(true);
        
        // Tenta autenticar no Firebase
        String email, firebasePassword;
        if (firebase.loadFirebaseCredentials(email, firebasePassword)) {
            Serial.println("🔥 Tentando autenticar no Firebase...");
            if (firebase.authenticate(email, firebasePassword)) {
                Serial.println("✅ Autenticação Firebase bem-sucedida!");
            } else {
                Serial.println("❌ Falha na autenticação Firebase");
            }
        } else {
            Serial.println("📝 Nenhuma credencial Firebase encontrada");
        }
    } else {
        Serial.println("📡 Modo AP ativo. Conecte-se para configurar");
        webServer.begin(false);
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
    if (!firebase.isAuthenticated() || !wifiConfig.isConnected()) {
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
        
        firebase.verificarComandos(actuators);
        firebase.RecebeSetpoint(actuators);
        
        lastFirebaseUpdate = millis();
    }
    
    if (millis() - lastHeartbeat > HEARTBEAT_INTERVAL) {
        firebase.enviarHeartbeat();
        lastHeartbeat = millis();
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n\n🚀 Iniciando IFungi System...");
    
    // Adicionar esta linha
    verificarHardware();
    
    // Configuração inicial
    setupSensorsAndActuators();
    setupWiFiAndFirebase();
    
    Serial.println("✅ Sistema inicializado e pronto!");
}

void loop() {
    // Processa cliente web
    webServer.handleClient();
    
    // Atualiza status do LED
    updateStatusLED();
    
    // Executa tarefas periódicas
    handleSensors();
    handleActuators();
    handleFirebase();
    
    // Pequeno delay para estabilidade
    delay(10);
}