#ifndef ACTUATOR_CONTROLLER_H
#define ACTUATOR_CONTROLLER_H

#include <Arduino.h>
#include "GreenhouseSystem.h"
#include <ESP32Servo.h>

// Forward declaration para evitar dependência circular
class FirebaseHandler;

/**
 * @class ActuatorController
 * @brief Controlador principal dos atuadores do sistema (relés, LEDs, servo, Peltier)
 * 
 * Esta classe gerencia todos os atuadores do sistema de estufa, incluindo:
 * - Controle de relés para Peltier (aquecimento/resfriamento)
 * - Controle de LEDs com intensidade variável
 * - Controle do servo motor para exaustão
 * - Controle do umidificador
 * - Modos manual e automático
 * - Integração com Firebase para sincronização de estados
 */
class ActuatorController {
public:
    // Configurações do servo motor
    int closedPosition = 160;   ///< Posição fechada do servo (dampers fechados)
    int openPosition = 45;      ///< Posição aberta do servo (dampers abertos)
    
    // Métodos de inicialização e configuração
    void begin(uint8_t pinLED, uint8_t pinRelay1, uint8_t pinRelay2, uint8_t pinRelay3, uint8_t pinRelay4, uint8_t servoPin);
    void setFirebaseHandler(FirebaseHandler* handler);
    void applySetpoints(int lux, float tempMin, float tempMax, float humidityMin, float humidityMax, int coSetpoint, int co2Setpoint, int tvocsSetpoint);
    
    // Métodos de controle direto dos atuadores
    void controlLEDs(bool on, int intensity);
    void controlRelay(uint8_t relayNumber, bool state);
    void controlPeltier(bool cooling, bool on);
    void controlAutomatically(float temp, float humidity, int light, int co, int co2, int tvocs, bool waterLevel);
    
    // Métodos para modo de debug e controle manual
    void setDebugMode(bool debug);
    void setManualStates(bool relay1, bool relay2, bool relay3, bool relay4, bool ledsOn, int ledsIntensity, bool humidifierOn);
    
    // Métodos de compatibilidade e persistência
    bool heatPeltier(bool on);
    void saveSetpointsNVS();
    bool loadSetpointsNVS();
    
    /**
     * @enum PeltierMode
     * @brief Modos de operação do módulo Peltier
     */
    enum PeltierMode {
        OFF,        ///< Peltier desligado
        HEATING,    ///< Modo aquecimento
        COOLING     ///< Modo resfriamento
    };
    
    // Métodos de consulta de estado
    bool isHumidifierOn() const { return humidifierOn; }
    bool areLEDsOn() const;
    int getLEDsWatts() const;
    int getRelayState(uint8_t relayNumber) const;

    // Controle de escrita no Firebase
    void setFirebaseWriteBlock(bool block);
    bool canWriteToFirebase();

    // Métodos para modo de desenvolvimento
    void setDevModeSettings(bool analogRead, bool digitalWrite, int pin, bool pwm, int pwmValue);
    void handleDevMode();

private:
    // Pinos dos atuadores
    uint8_t _pinLED, _pinRelay1, _pinRelay2, _pinRelay3, _pinRelay4, _servoPin;
    Servo myServo;                          ///< Objeto do servo motor
    FirebaseHandler* firebaseHandler = nullptr; ///< Ponteiro para handler do Firebase
    
    // Variáveis de estado dos atuadores
    bool humidifierOn = false;              ///< Estado do umidificador
    bool peltierActive = false;             ///< Estado do Peltier (ativo/inativo)
    unsigned long lastPeltierTime = 0;      ///< Último tempo de ativação do Peltier
    unsigned long cooldownStart = 0;        ///< Início do período de cooldown
    
    // Setpoints do sistema
    int luxSetpoint = 5000;                 ///< Setpoint de luminosidade (lux)
    float tempMin = 20.0;                   ///< Temperatura mínima (°C)
    float tempMax = 30.0;                   ///< Temperatura máxima (°C)
    float humidityMin = 60.0;               ///< Umidade mínima (%)
    float humidityMax = 80.0;               ///< Umidade máxima (%)
    int coSetpoint = 400;                   ///< Setpoint de CO (ppm)
    int co2Setpoint = 400;                  ///< Setpoint de CO2 (ppm)
    int tvocsSetpoint = 100;                ///< Setpoint de TVOCs (ppb)
    
    // Variáveis de estado atual
    PeltierMode currentPeltierMode = OFF;   ///< Modo atual do Peltier
    int currentLEDIntensity = 0;            ///< Intensidade atual dos LEDs (0-255)
    bool relay1State = false;               ///< Estado do relé 1 (Peltier)
    bool relay2State = false;               ///< Estado do relé 2 (Peltier)
    bool relay3State = false;               ///< Estado do relé 3 (Umidificador)
    bool relay4State = false;               ///< Estado do relé 4 (Exaustor)
    unsigned long lastUpdateTime = 0;       ///< Último tempo de atualização
    
    // Variáveis para modo de debug
    bool debugMode = false;                 ///< Indica se modo debug está ativo
    bool lastDebugMode = false;             ///< Último estado do modo debug
    
    // Controle de escrita no Firebase
    bool blockFirebaseWrite = false;        ///< Bloqueio de escrita no Firebase
    unsigned long firebaseWriteBlockTime = 0; ///< Tempo do início do bloqueio
    const unsigned long FIREBASE_WRITE_BLOCK_DURATION = 10000; ///< Duração do bloqueio (10s)

    // Variáveis de segurança do Peltier
    bool inCooldown = false;                ///< Indica se está em cooldown
    const unsigned long operationTime = 10000;  ///< Tempo máximo de operação (10s)
    const unsigned long cooldownTime = 10000;   ///< Tempo de cooldown (10s)
    
    // Variáveis para modo de desenvolvimento
    bool devModeAnalogRead = false;         ///< Habilita leitura analógica no devmode
    bool devModeDigitalWrite = false;       ///< Habilita escrita digital no devmode
    bool devModePWM = false;                ///< Habilita PWM no devmode
    int devModePin = -1;                    ///< Pino para operações do devmode
    int devModePWMValue = 0;                ///< Valor PWM para devmode
    bool lastDevModeState = false;          ///< Último estado do devmode
    
    // Métodos privados
    void updateFirebaseState();
    void updateFirebaseStateImmediately();
    void executeDevModeOperations();
};

#endif