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

// Declarações de funções
void memateRapido();
void updateStatusLED();
void setupSensorsAndActuators();
void setupWiFiAndFirebase();
void handleFirebaseOperations();
void handleRegularIntervals();

void memateRapido() {
    String email, password;
    Serial.println("iniciando loop até loadFirebaseCredentials retornar true");
    ansi.println("iniciando loop até loadFirebaseCredentials retornar true");
    while((email, password) != ("") && firebase.authenticated == true && !firebase.loadFirebaseCredentials(email, password)) {
        Serial.print(".");
        webServer.begin(true);
    }
}

void updateStatusLED() {
    if(firebase.isAuthenticated() && wifiConfig.isConnected()) {
        wifiConfig.piscaLED(true, 666666);
        return;
    } else if(!wifiConfig.isConnected()) {
        wifiConfig.piscaLED(true, 666666);
        return;
    } else if(!firebase.isAuthenticated()) {
        wifiConfig.piscaLED(true, 777777);
        return;
    } else {
        wifiConfig.piscaLED(true, 20000);
        return;
    }
}

void setupSensorsAndActuators() {
    sensors.begin();
    actuators.begin(4, 23, 14, 18, 19);
    
    if(!actuators.carregarSetpointsNVS()) {
        actuators.aplicarSetpoints(5000, 20.0, 30.0, 60.0, 80.0, 400, 400, 100);
    }
}

void setupWiFiAndFirebase() {
    String ssid, password;
    if(wifiConfig.loadCredentials(ssid, password)) {
        if(wifiConfig.connectToWiFi(ssid.c_str(), password.c_str(), true)) {
            Serial.println("Conectado ao WiFi! Iniciando servidor...");
            updateStatusLED();
            
            ifungiID = "IFUNGI-" + getMacAddress();
            Serial.println("ID da Estufa: " + ifungiID);
            qrcode.generateQRCode(ifungiID);
            
            webServer.begin(true);
            
            String email, firebasePassword;
            updateStatusLED();
            if(firebase.loadFirebaseCredentials(email, firebasePassword)) {
                Serial.println("Credenciais do Firebase encontradas, autenticando...");
                
                if(firebase.authenticate(email, firebasePassword)) {
                    Serial.println("Autenticação bem-sucedida!");
                    updateStatusLED();
                } else {
                    Serial.println("Falha na autenticação com credenciais salvas.");
                    updateStatusLED();
                }
            } else {
                Serial.println("Nenhuma credencial do Firebase encontrada.");
                updateStatusLED();
            }
        } else {
            wifiConfig.startAP(AP_SSID, AP_PASSWORD);
            webServer.begin(false);
            updateStatusLED();
        }
        updateStatusLED();
    } else {
        wifiConfig.startAP(AP_SSID, AP_PASSWORD);
        webServer.begin(false);
        updateStatusLED();
    }
    actuators.setFirebaseHandler(&firebase);
    updateStatusLED();
}

void handleFirebaseOperations() {
    static unsigned long lastFirebaseUpdate = 0;
    static unsigned long lastTokenCheck = 0;
    static unsigned long lastAuthAttempt = 0;
    
    const unsigned long FIREBASE_INTERVAL = 5000;
    const unsigned long TOKEN_CHECK_INTERVAL = 300000;
    const unsigned long AUTH_RETRY_INTERVAL = 30000;

    if(firebase.isAuthenticated()) {
        if(millis() - lastTokenCheck > TOKEN_CHECK_INTERVAL) {
            firebase.refreshToken();
            lastTokenCheck = millis();
        }

        if(millis() - lastFirebaseUpdate > FIREBASE_INTERVAL) {
            if(Firebase.ready()) {
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
            } else {
                Serial.println("Token do Firebase inválido, tentando renovar...");
                firebase.refreshToken();
            }
        }
    } else {
        if(millis() - lastAuthAttempt > AUTH_RETRY_INTERVAL) {
            String email, password;
            if(firebase.loadFirebaseCredentials(email, password)) {
                if(firebase.authenticate(email, password)) {
                    firebase.verificarEstufa();
                }
            }
            lastAuthAttempt = millis();
        }
    }
}

void handleRegularIntervals() {
    static unsigned long lastSensorRead = 0;
    static unsigned long lastActuatorControl = 0;
    static unsigned long lastHeartbeat = 0;
    
    const unsigned long SENSOR_READ_INTERVAL = 2000;
    const unsigned long ACTUATOR_CONTROL_INTERVAL = 5000;
    const unsigned long HEARTBEAT_INTERVAL = 15000;

    if (millis() - lastSensorRead > SENSOR_READ_INTERVAL) {
        sensors.update();
        lastSensorRead = millis();
    }
    
    if (millis() - lastActuatorControl > ACTUATOR_CONTROL_INTERVAL) {
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
    
    if (millis() - lastHeartbeat > HEARTBEAT_INTERVAL) {
        if (firebase.isAuthenticated()) {
            firebase.enviarHeartbeat();
        }
        lastHeartbeat = millis();
    }
}

void setup() {
    Serial.begin(115200);
    ansi.foreground(ANSI::white | ANSI::bright);
    ansi.background(ANSI::red);
    ansi.print(" Red ");
    ansi.background(ANSI::green);
    ansi.print(" Green ");
    ansi.background(ANSI::magenta);
    ansi.println(" Purple ");
    Serial.println();

    ansi.normal();
    ansi.print("Doing nothing... ");
    ansi.foreground(ANSI::yellow | ANSI::bright);
    ansi.print("[ ]");
    ansi.cursorBack(2);
    delay(1000);
    firebase.setWiFiConfigurator(&wifiConfig);
    
    setupSensorsAndActuators();
    setupWiFiAndFirebase();
}

void loop() {
    updateStatusLED();
    webServer.handleClient();
    handleRegularIntervals();
    handleFirebaseOperations();
    
    delay(10);
}