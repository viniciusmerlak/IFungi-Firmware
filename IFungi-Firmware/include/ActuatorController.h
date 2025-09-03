#ifndef ACTUATORCONTROLLER_H
#define ACTUATORCONTROLLER_H

#include <Arduino.h>
#include "FirebaseHandler.h"

class FirebaseHandler;

class ActuatorController {
public:
    void begin(uint8_t pinLED, uint8_t pinRele1, uint8_t pinRele2, uint8_t pinRele3, uint8_t pinRele4);
    void setFirebaseHandler(FirebaseHandler* handler);
    void aplicarSetpoints(int lux, float tMin, float tMax, float uMin, float uMax, int coSp, int co2Sp, int tvocsSp);
    void controlarLEDs(bool ligado, int watts);
    void controlarRele(uint8_t num, bool estado);
    void controlarPeltier(bool resfriar, bool ligar);
    void controlarAutomaticamente(float temp, float umid, int luz, int co, int co2, int tvocs, bool waterLevel);
    bool AquecerPastilha(bool ligar);
    void salvarSetpointsNVS();
    bool carregarSetpointsNVS();
    enum ModoPeltier {
        DESLIGADO,
        AQUECENDO,
        RESFRIANDO
    };
    
private:
    uint8_t _pinLED, _pinRele1, _pinRele2, _pinRele3, _pinRele4;
    FirebaseHandler* firebaseHandler = nullptr;
    
    // Variáveis de estado
    bool umidLigado = false;
    bool peltierHeating = false;
    unsigned long lastPeltierTime = 0;
    unsigned long inicioCooldown = 0;
    
    // Setpoints
    int luxSetpoint = 5000;
    float tempMin = 20.0;
    float tempMax = 30.0;
    float umidMin = 60.0;
    float umidMax = 80.0;
    int coSetpoint = 400;    // ppm
    int co2Setpoint = 400;  // ppm
    int tvocsSetpoint = 100; // ppb - Add this line
    
    // Novas variáveis para controle de estado
    ModoPeltier modoPeltierAtual = DESLIGADO;
    int intensidadeLEDAtual = 0;
    bool rele1Estado = false;
    bool rele2Estado = false;
    bool rele3Estado = false;
    bool rele4Estado = false;
    unsigned long lastUpdateTime = 0;
    
    // Variáveis de segurança da Peltier
    bool emCooldown = false;
    const unsigned long tempoOperacao = 10000; // 10 segundos de operação
    const unsigned long tempoCooldown = 10000; // 10 segundos de cooldown
    
    void atualizarEstadoFirebase();
};

#endif