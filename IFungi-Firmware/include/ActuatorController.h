#ifndef ACTUATOR_CONTROLLER_H
#define ACTUATOR_CONTROLLER_H

#include <Arduino.h>
#include "GreenhouseSystem.h"
#include <ESP32Servo.h>
#include "LEDScheduler.h"
#include "OperationMode.h"

class FirebaseHandler;

/**
 * @class ActuatorController
 * @brief Controlador principal dos atuadores do sistema (relés, LEDs, servo, Peltier)
 *
 * NOTA (v1.1): controlAutomatically() agora recebe dhtHealthy (bool).
 * Quando false, o Peltier é desligado imediatamente e bloqueado até que
 * o DHT22 se recupere — evita aquecimento/resfriamento sem leitura válida.
 */
class ActuatorController {
public:
    int closedPosition = 160;
    int openPosition   = 45;

    void begin(uint8_t pinLED, uint8_t pinRelay1, uint8_t pinRelay2,
               uint8_t pinRelay3, uint8_t pinRelay4, uint8_t servoPin);
    void setFirebaseHandler(FirebaseHandler* handler);
    void applySetpoints(int lux, float tempMin, float tempMax,
                        float humidityMin, float humidityMax,
                        int coSetpoint, int co2Setpoint, int tvocsSetpoint);

    void controlLEDs(bool on, int intensity);
    void controlRelay(uint8_t relayNumber, bool state);
    void controlPeltier(bool cooling, bool on);

    /**
     * @brief Controla atuadores automaticamente baseado em leituras dos sensores
     *
     * @param temp        Temperatura (°C) — pode ser NAN se dhtHealthy=false
     * @param humidity    Umidade (%) — pode ser 100.0f (fail-safe) se dhtHealthy=false
     * @param light       Lux (ADC)
     * @param co          CO ppm
     * @param co2         CO2 ppm
     * @param tvocs       TVOCs ppb
     * @param waterLevel  true=água baixa
     * @param dhtHealthy  false=DHT inoperante → Peltier BLOQUEADO por segurança
     * @param allowFirebaseWrite  Permite que o controlador envie atualizações ao Firebase
     *                            (desative quando chamado de uma thread secundária)
     */
    void controlAutomatically(float temp, float humidity, int light,
                               int co, int co2, int tvocs,
                               bool waterLevel, bool dhtHealthy = true,
                               bool allowFirebaseWrite = true);

    void setDebugMode(bool debug);
    void setManualStates(bool relay1, bool relay2, bool relay3, bool relay4,
                         bool ledsOn, int ledsIntensity, bool humidifierOn);

    void applyOperationMode(OperationMode mode);
    OperationMode getOperationMode() const { return _currentMode; }

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
    int  getLEDsWatts() const;
    int  getRelayState(uint8_t relayNumber) const;

    void setFirebaseWriteBlock(bool block);
    bool canWriteToFirebase();

    LEDScheduler ledScheduler;
    void applyLEDSchedule(unsigned long ts);
    void saveLEDScheduleNVS();
    bool loadLEDScheduleNVS();
    void persistLEDScheduleIfChanged();

    void setDevModeSettings(bool analogRead, bool digitalWrite, int pin, bool pwm, int pwmValue);
    void handleDevMode();

private:
    uint8_t _pinLED, _pinRelay1, _pinRelay2, _pinRelay3, _pinRelay4, _servoPin;
    Servo myServo;
    FirebaseHandler* firebaseHandler = nullptr;

    OperationMode _currentMode = MODE_MANUAL;

    bool _humidifierAllowed = true;
    bool _ledsAllowed       = true;
    bool _peltierAllowed    = true;
    bool _exhaustForced     = false;

    bool humidifierOn       = false;
    bool peltierActive      = false;
    unsigned long lastPeltierTime     = 0;
    unsigned long peltierHeatingStart = 0;
    unsigned long cooldownStart       = 0;

    int   luxSetpoint   = 5000;
    float tempMin       = 20.0;
    float tempMax       = 30.0;
    float humidityMin   = 60.0;
    float humidityMax   = 80.0;
    int   coSetpoint    = 50;
    int   co2Setpoint   = 400;
    int   tvocsSetpoint = 100;

    PeltierMode currentPeltierMode  = OFF;
    int         currentLEDIntensity = 0;
    volatile int targetLEDIntensity = 0;
    bool relay1State = false;
    bool relay2State = false;
    bool relay3State = false;
    bool relay4State = false;
    unsigned long lastUpdateTime = 0;

    bool debugMode     = false;
    bool lastDebugMode = false;

    bool          blockFirebaseWrite      = false;
    unsigned long firebaseWriteBlockTime  = 0;
    const unsigned long FIREBASE_WRITE_BLOCK_DURATION = 10000;

    bool inCooldown = false;
    const unsigned long operationTime = 300000;
    const unsigned long cooldownTime  = 60000;

    bool devModeAnalogRead   = false;
    bool devModeDigitalWrite = false;
    bool devModePWM          = false;
    int  devModePin          = -1;
    int  devModePWMValue     = 0;
    bool lastDevModeState    = false;
    TaskHandle_t ledPwmTaskHandle = nullptr;
    bool _loadedScheduleFromNvs   = false;

    bool _allowFirebaseUpdates = true;   // proteção contra chamadas multi‑thread

    void updateFirebaseState();
    void updateFirebaseStateImmediately();
    void executeDevModeOperations();
    static void ledPwmTask(void* parameter);
    static int  ledLogicalToHardwarePwm(int logical);
    void        writeLedHardwareFromLogical(int logical);
};

#endif