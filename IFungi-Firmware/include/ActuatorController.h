#ifndef ACTUATOR_CONTROLLER_H
#define ACTUATOR_CONTROLLER_H

#include <Arduino.h>
#include "GreenhouseSystem.h"
#include <ESP32Servo.h>
#include "LEDScheduler.h"
#include "OperationMode.h"

// Forward declaration para evitar dependência circular
class FirebaseHandler;

/**
 * @class ActuatorController
 * @brief Controlador principal dos atuadores do sistema (relés, LEDs, servo, Peltier)
 *
 * Esta classe gerencia todos os atuadores do sistema de estufa, incluindo:
 * - Controle de relés para Peltier: relé 1 = ligar/desligar pastilha; relé 2 = polaridade
 *   (aquecer/resfriar com relé 1 ligado). Relés 3 e 4 = umidificador e exaustor.
 * - Controle de LEDs com intensidade variável
 * - Controle do servo motor para exaustão
 * - Controle do umidificador
 * - Modos de operação (incubação, frutificação, secagem, manutenção, manual)
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

    // ── Modos de operação ────────────────────────────────────────────────────

    /**
     * @brief Aplica um modo de operação pré-configurado
     *
     * @details Carrega o preset do modo, aplica setpoints e configura o
     * LEDScheduler de acordo. No modo MANUAL, apenas reseta as restrições
     * sem alterar os setpoints (eles continuam vindo do Firebase normalmente).
     *
     * @param mode Novo modo de operação
     */
    void applyOperationMode(OperationMode mode);

    /**
     * @brief Retorna o modo de operação atual
     */
    OperationMode getOperationMode() const { return _currentMode; }

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

    // Agendador de LEDs (timer e simulação solar)
    LEDScheduler ledScheduler;                  ///< Instância pública — lida pelo Firebase
    void applyLEDSchedule(unsigned long ts);    ///< Aplica decisão do scheduler nos LEDs
    void saveLEDScheduleNVS();
    bool loadLEDScheduleNVS();
    void persistLEDScheduleIfChanged();

    // Métodos para modo de desenvolvimento
    void setDevModeSettings(bool analogRead, bool digitalWrite, int pin, bool pwm, int pwmValue);
    void handleDevMode();

private:
    // Pinos dos atuadores
    uint8_t _pinLED, _pinRelay1, _pinRelay2, _pinRelay3, _pinRelay4, _servoPin;
    Servo myServo;                              ///< Objeto do servo motor
    FirebaseHandler* firebaseHandler = nullptr; ///< Ponteiro para handler do Firebase

    // ── Modo de operação atual ───────────────────────────────────────────────
    OperationMode _currentMode = MODE_MANUAL;   ///< Modo de operação corrente

    // Restrições impostas pelo modo de operação
    // Quando false, o atuador correspondente fica FORÇADAMENTE desligado
    // independente da lógica automática ou setpoints.
    bool _humidifierAllowed = true;             ///< Umidificador permitido neste modo?
    bool _ledsAllowed       = true;             ///< LEDs permitidos neste modo?
    bool _peltierAllowed    = true;             ///< Peltier permitido neste modo?
    bool _exhaustForced     = false;            ///< Exaustor forçado ligado?

    // Variáveis de estado dos atuadores
    bool humidifierOn = false;              ///< Estado do umidificador
    bool peltierActive = false;             ///< Estado do Peltier (ativo/inativo)
    unsigned long lastPeltierTime = 0;      ///< Reservado / logs
    unsigned long peltierHeatingStart = 0;   ///< Início do segmento atual de aquecimento (R1+R2)
    unsigned long cooldownStart = 0;        ///< Início do cooldown pós-aquecimento

    // Setpoints do sistema
    int luxSetpoint = 5000;                 ///< Setpoint de luminosidade (lux)
    float tempMin = 20.0;                   ///< Temperatura mínima (°C)
    float tempMax = 30.0;                   ///< Temperatura máxima (°C)
    float humidityMin = 60.0;               ///< Umidade mínima (%)
    float humidityMax = 80.0;               ///< Umidade máxima (%)
    int coSetpoint = 50;                    ///< Setpoint de CO (ppm) — exaustão se acima
    int co2Setpoint = 400;                  ///< Setpoint de CO2 (ppm)
    int tvocsSetpoint = 100;                ///< Setpoint de TVOCs (ppb)

    // Variáveis de estado atual
    PeltierMode currentPeltierMode = OFF;   ///< Modo atual do Peltier
    int currentLEDIntensity = 0;            ///< Intensidade atual dos LEDs (0-255)
    volatile int targetLEDIntensity = 0;    ///< Alvo lógico de intensidade (consumido pela task)
    bool relay1State = false;               ///< Relé 1: alimentação da Peltier (on/off)
    bool relay2State = false;               ///< Relé 2: polaridade (com R1 ligado: frio/calor)
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
    const unsigned long operationTime = 300000;  ///< Aquecimento contínuo máx. (5 min) antes do cooldown
    const unsigned long cooldownTime = 60000;    ///< Cooldown após aquecimento prolongado (1 min)

    // Variáveis para modo de desenvolvimento
    bool devModeAnalogRead = false;
    bool devModeDigitalWrite = false;
    bool devModePWM = false;
    int devModePin = -1;
    int devModePWMValue = 0;
    bool lastDevModeState = false;
    TaskHandle_t ledPwmTaskHandle = nullptr;
    bool _loadedScheduleFromNvs = false;

    // Métodos privados
    void updateFirebaseState();
    void updateFirebaseStateImmediately();
    void executeDevModeOperations();
    static void ledPwmTask(void* parameter);
    /// Converte intensidade lógica (0=apagado, 255=máximo) em PWM no hardware (driver invertido).
    static int ledLogicalToHardwarePwm(int logical);
    void writeLedHardwareFromLogical(int logical);
};

#endif
