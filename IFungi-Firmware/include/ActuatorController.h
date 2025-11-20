#ifndef ACTUATOR_CONTROLLER_H
#define ACTUATOR_CONTROLLER_H

#include <Arduino.h>
#include "GreenhouseSystem.h"
#include <ESP32Servo.h>

class FirebaseHandler;

class ActuatorController {
public:
    int closedPosition = 160;
    int openPosition = 45;
    
    void begin(uint8_t pinLED, uint8_t pinRelay1, uint8_t pinRelay2, uint8_t pinRelay3, uint8_t pinRelay4, uint8_t servoPin);
    void setFirebaseHandler(FirebaseHandler* handler);
    void applySetpoints(int lux, float tempMin, float tempMax, float humidityMin, float humidityMax, int coSetpoint, int co2Setpoint, int tvocsSetpoint);
    void controlLEDs(bool on, int intensity);
    void controlRelay(uint8_t relayNumber, bool state);
    void controlPeltier(bool cooling, bool on);
    void controlAutomatically(float temp, float humidity, int light, int co, int co2, int tvocs, bool waterLevel);
    
    // 🔥 NOVAS FUNÇÕES PARA DEBUG
    void setDebugMode(bool debug);
    void setManualStates(bool relay1, bool relay2, bool relay3, bool relay4, bool ledsOn, int ledsIntensity, bool humidifierOn);
    
    bool heatPeltier(bool on);
    void saveSetpointsNVS();
    bool loadSetpointsNVS();
    
    enum PeltierMode {
        OFF,
        HEATING,
        COOLING
    };
    
    bool isHumidifierOn() const { return humidifierOn; }
    bool areLEDsOn() const;
    int getLEDsWatts() const;
    int getRelayState(uint8_t relayNumber) const;

    // 🔥 NOVAS FUNÇÕES PARA CONTROLE DE ESCRITA
    void setFirebaseWriteBlock(bool block);
    bool canWriteToFirebase();

private:
    uint8_t _pinLED, _pinRelay1, _pinRelay2, _pinRelay3, _pinRelay4, _servoPin;
    Servo myServo;
    FirebaseHandler* firebaseHandler = nullptr;
    
    // State variables
    bool humidifierOn = false;
    bool peltierActive = false;
    unsigned long lastPeltierTime = 0;
    unsigned long cooldownStart = 0;
    
    // Setpoints
    int luxSetpoint = 5000;
    float tempMin = 20.0;
    float tempMax = 30.0;
    float humidityMin = 60.0;
    float humidityMax = 80.0;
    int coSetpoint = 400;
    int co2Setpoint = 400;
    int tvocsSetpoint = 100;
    
    // Current state variables
    PeltierMode currentPeltierMode = OFF;
    int currentLEDIntensity = 0;
    bool relay1State = false;
    bool relay2State = false;
    bool relay3State = false;
    bool relay4State = false;
    unsigned long lastUpdateTime = 0;
    
    // 🔥 NOVAS VARIÁVEIS PARA DEBUG E CONTROLE DE ESCRITA
    bool debugMode = false;
    bool lastDebugMode = false;
    
    // 🔥 VARIÁVEIS PARA CONTROLE DE ESCRITA NO FIREBASE
    bool blockFirebaseWrite = false;
    unsigned long firebaseWriteBlockTime = 0;
    const unsigned long FIREBASE_WRITE_BLOCK_DURATION = 10000; // 10 segundos

    // Peltier safety variables
    bool inCooldown = false;
    const unsigned long operationTime = 10000;
    const unsigned long cooldownTime = 10000;
    
    void updateFirebaseState();
    void updateFirebaseStateImmediately();
};

#endif