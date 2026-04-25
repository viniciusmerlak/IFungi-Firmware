#include "ActuatorController.h"
#include "OperationMode.h"
#include <Preferences.h>

// =============================================================================
// MÉTODOS DE CONTROLE DE ESCRITA NO FIREBASE
// =============================================================================

void ActuatorController::setFirebaseWriteBlock(bool block) {
    if (block) {
        blockFirebaseWrite = true;
        firebaseWriteBlockTime = millis();
        Serial.println("🔒 Firebase write BLOCKED for manual control");
    } else {
        blockFirebaseWrite = false;
        Serial.println("🔓 Firebase write UNBLOCKED");
    }
}

bool ActuatorController::canWriteToFirebase() {
    if (!blockFirebaseWrite) {
        return true;
    }
    
    if (millis() - firebaseWriteBlockTime > FIREBASE_WRITE_BLOCK_DURATION) {
        blockFirebaseWrite = false;
        Serial.println("🔓 Firebase write auto-UNBLOCKED (timeout)");
        return true;
    }
    
    return false;
}

// =============================================================================
// MÉTODOS DE PERSISTÊNCIA DE SETPOINTS (NVS)
// =============================================================================

void ActuatorController::saveSetpointsNVS() {
    Preferences preferences;
    if(!preferences.begin("setpoints", false)) {
        Serial.println("❌ Error opening NVS to save setpoints!");
        return;
    }
    
    preferences.putInt("lux", luxSetpoint);
    preferences.putFloat("tMin", tempMin);
    preferences.putFloat("tMax", tempMax);
    preferences.putFloat("uMin", humidityMin);
    preferences.putFloat("uMax", humidityMax);
    preferences.putInt("coSp", coSetpoint);
    preferences.putInt("co2Sp", co2Setpoint);
    preferences.putInt("tvocsSp", tvocsSetpoint);
    
    preferences.end();
    Serial.println("💾 Setpoints saved to NVS");
}

bool ActuatorController::loadSetpointsNVS() {
    Preferences preferences;
    if(!preferences.begin("setpoints", true)) {
        Serial.println("❌ Setpoints NVS not found, using defaults");
        return false;
    }
    
    if(preferences.isKey("lux")) {
        luxSetpoint   = preferences.getInt("lux", 100);
        tempMin       = preferences.getFloat("tMin", 20.0);
        tempMax       = preferences.getFloat("tMax", 30.0);
        humidityMin   = preferences.getFloat("uMin", 60.0);
        humidityMax   = preferences.getFloat("uMax", 80.0);
        coSetpoint    = preferences.getInt("coSp", 400);
        co2Setpoint   = preferences.getInt("co2Sp", 400);
        tvocsSetpoint = preferences.getInt("tvocsSp", 100);
        
        preferences.end();
        Serial.println("📁 Setpoints loaded from NVS");
        return true;
    } else {
        preferences.end();
        Serial.println("❌ No setpoints saved in NVS, using defaults");
        return false;
    }
}

// =============================================================================
// HISTERESE
// =============================================================================

const float HYSTERESIS_TEMP     = 0.5f;
const float HYSTERESIS_HUMIDITY = 2.0f;

// =============================================================================
// INICIALIZAÇÃO E CONFIGURAÇÃO
// =============================================================================

void ActuatorController::begin(uint8_t pinLED, uint8_t pinRelay1, uint8_t pinRelay2, 
                               uint8_t pinRelay3, uint8_t pinRelay4, uint8_t servoPin) {
    Serial.println("🔧 Initializing ActuatorController...");
    
    _pinLED    = pinLED;
    _pinRelay1 = pinRelay1;
    _pinRelay2 = pinRelay2;
    _pinRelay3 = pinRelay3;
    _pinRelay4 = pinRelay4;
    _servoPin  = servoPin;
    
    myServo.attach(servoPin);
    myServo.write(closedPosition);
    
    pinMode(_pinLED,    OUTPUT);
    pinMode(_pinRelay1, OUTPUT);
    pinMode(_pinRelay2, OUTPUT);
    pinMode(_pinRelay3, OUTPUT);
    pinMode(_pinRelay4, OUTPUT);
    
    digitalWrite(_pinLED,    LOW);
    digitalWrite(_pinRelay1, LOW);
    digitalWrite(_pinRelay2, LOW);
    digitalWrite(_pinRelay3, LOW);
    digitalWrite(_pinRelay4, LOW);
    
    humidifierOn        = false;
    peltierActive       = false;
    lastPeltierTime     = 0;
    lastUpdateTime      = 0;
    inCooldown          = false;
    cooldownStart       = 0;
    blockFirebaseWrite  = false;
    firebaseWriteBlockTime = 0;
    
    Serial.println("✅ ActuatorController initialized successfully");
}

void ActuatorController::setFirebaseHandler(FirebaseHandler* handler) {
    firebaseHandler = handler;
    Serial.println("🔥 FirebaseHandler set for ActuatorController");
}

void ActuatorController::applySetpoints(int lux, float tMin, float tMax, float uMin, float uMax, int coSp, int co2Sp, int tvocsSp) {
    luxSetpoint   = lux;
    tempMin       = tMin;
    tempMax       = tMax;
    humidityMin   = uMin;
    humidityMax   = uMax;
    coSetpoint    = coSp;
    co2Setpoint   = co2Sp;
    tvocsSetpoint = tvocsSp;
    
    saveSetpointsNVS();
    
    Serial.printf("⚙️ Setpoints: Lux=%d, Temp=[%.1f-%.1f], Hum=[%.1f-%.1f], CO=%d, CO2=%d, TVOCs=%d\n", 
                 lux, tMin, tMax, uMin, uMax, coSp, co2Sp, tvocsSp);
}

// =============================================================================
// MODOS DE OPERAÇÃO
// =============================================================================

void ActuatorController::applyOperationMode(OperationMode mode) {
    if (mode == _currentMode) return;

    _currentMode = mode;
    ModePreset p = getModePreset(mode);

    Serial.printf("[MODE] 🍄 Mudando para modo: %s\n", operationModeLabel(mode).c_str());

    _humidifierAllowed = p.humidifierEnabled;
    _ledsAllowed       = p.ledsEnabled;
    _peltierAllowed    = p.peltierEnabled;
    _exhaustForced     = p.exhaustForcedOn;

    if (mode != MODE_MANUAL) {
        applySetpoints(p.luxSetpoint,
                       p.tempMin, p.tempMax,
                       p.humidityMin, p.humidityMax,
                       coSetpoint,
                       p.co2Setpoint,
                       tvocsSetpoint);
    }

    if (!p.ledsEnabled) {
        ledScheduler.scheduleEnabled = false;
        ledScheduler.solarSimEnabled = false;
        controlLEDs(false, 0);
        Serial.println("[MODE] LEDs desativados pelo modo de operação");
    } else if (p.schedulerActive) {
        ledScheduler.scheduleEnabled = !p.solarSim;
        ledScheduler.solarSimEnabled = p.solarSim;
        ledScheduler.onHour          = p.ledOnHour;
        ledScheduler.onMinute        = p.ledOnMinute;
        ledScheduler.offHour         = p.ledOffHour;
        ledScheduler.offMinute       = p.ledOffMinute;
        ledScheduler.configIntensity = p.ledIntensity;
        Serial.printf("[MODE] Scheduler: %02d:%02d - %02d:%02d (%s)\n",
                      p.ledOnHour, p.ledOnMinute, p.ledOffHour, p.ledOffMinute,
                      p.solarSim ? "solar" : "timer fixo");
    }

    if (!p.humidifierEnabled && humidifierOn) {
        controlRelay(3, false);
        humidifierOn = false;
        Serial.println("[MODE] Umidificador desligado pelo modo");
    }
    if (!p.peltierEnabled && peltierActive) {
        controlPeltier(false, false);
        Serial.println("[MODE] Peltier desligado pelo modo");
    }
    if (p.exhaustForcedOn) {
        myServo.write(openPosition);
        controlRelay(4, true);
        Serial.println("[MODE] Exaustor forçado LIGADO pelo modo");
    }

    Serial.printf("[MODE] ✅ Modo %s aplicado\n", operationModeLabel(mode).c_str());
}

// =============================================================================
// CONTROLE AUTOMÁTICO PRINCIPAL
// =============================================================================

void ActuatorController::controlAutomatically(float temp, float humidity, int light, int co, int co2, int tvocs, bool waterLevel) {
    if (debugMode) {
        return;
    }
    
    // ── PELTIER ───────────────────────────────────────────────────────────────
    if (!_peltierAllowed) {
        if (peltierActive) controlPeltier(false, false);
    } else {
        if (temp < (tempMin - HYSTERESIS_TEMP)) {
            if (!peltierActive || currentPeltierMode != HEATING) {
                Serial.printf("🔥 [ACTUATOR] Temp abaixo (%.1f < %.1f), aquecendo\n", temp, tempMin);
                controlPeltier(false, true);
            }
        } else if (temp > (tempMax + HYSTERESIS_TEMP)) {
            if (!peltierActive || currentPeltierMode != COOLING) {
                Serial.printf("❄️ [ACTUATOR] Temp acima (%.1f > %.1f), resfriando\n", temp, tempMax);
                controlPeltier(true, true);
            }
        } else if (peltierActive) {
            Serial.println("✅ [ACTUATOR] Temp OK, desligando Peltier");
            controlPeltier(false, false);
        }
    }
    
    // ── UMIDIFICADOR ─────────────────────────────────────────────────────────
    if (!_humidifierAllowed) {
        if (humidifierOn) {
            controlRelay(3, false);
            humidifierOn = false;
        }
    } else if (waterLevel) {
        // waterLevel = true → água BAIXA → desliga por segurança
        Serial.println("🛑 [SAFETY] Nível de água baixo, desligando umidificador");
        if (humidifierOn) {
            controlRelay(3, false);
            humidifierOn = false;
        }
    } else {
        if (humidity < (humidityMin - HYSTERESIS_HUMIDITY)) {
            if (!humidifierOn) {
                Serial.printf("💧 [ACTUATOR] Umidade baixa (%.1f < %.1f), ligando\n", humidity, humidityMin);
                controlRelay(3, true);
                humidifierOn = true;
            }
        } else if (humidity > (humidityMax + HYSTERESIS_HUMIDITY)) {
            if (humidifierOn) {
                Serial.printf("💧 [ACTUATOR] Umidade alta (%.1f > %.1f), desligando\n", humidity, humidityMax);
                controlRelay(3, false);
                humidifierOn = false;
            }
        }
    }
    
    // ── LEDs ─────────────────────────────────────────────────────────────────
    if (!_ledsAllowed) {
        if (currentLEDIntensity > 0) {
            controlLEDs(false, 0);
        }
    } else if (ledScheduler.isActive()) {
        int schedIntensity = ledScheduler.wantsLEDsOn() ? ledScheduler.getIntensity() : 0;
        if (schedIntensity != currentLEDIntensity) {
            controlLEDs(schedIntensity > 0, schedIntensity);
        }
    } else {
        int newIntensity = (light < luxSetpoint) ? 255 : 0;
        if (newIntensity != currentLEDIntensity) {
            controlLEDs(newIntensity > 0, newIntensity);
        }
    }
    
    // ── ATUALIZAÇÃO FIREBASE ──────────────────────────────────────────────────
    if (firebaseHandler != nullptr && firebaseHandler->isAuthenticated() && firebaseHandler->isFirebaseReady()) {
        if (millis() - lastUpdateTime > 5000 && canWriteToFirebase()) {
            updateFirebaseState();
            lastUpdateTime = millis();
        }
    }
    
    // ── EXAUSTOR ─────────────────────────────────────────────────────────────
    if (_exhaustForced) {
        myServo.write(openPosition);
        controlRelay(4, true);
    } else if (co > coSetpoint || co2 > co2Setpoint || tvocs > tvocsSetpoint) {
        Serial.printf("💨 [ACTUATOR] Gases acima do limite (CO:%d CO2:%d TVOCs:%d)\n", co, co2, tvocs);
        myServo.write(openPosition);
        controlRelay(4, true);
    } else {
        myServo.write(closedPosition);
        controlRelay(4, false);
    }
}

// =============================================================================
// CONTROLE INDIVIDUAL DOS ATUADORES
// =============================================================================

void ActuatorController::controlPeltier(bool cooling, bool on) {
    bool stateChanged = false;
    
    if (on) {
        if (cooling) {
            if (!relay1State || relay2State || currentPeltierMode != COOLING) {
                digitalWrite(_pinRelay1, HIGH);
                digitalWrite(_pinRelay2, LOW);
                currentPeltierMode = COOLING;
                relay1State = true;
                relay2State = false;
                stateChanged = true;
                Serial.println("❄️ [ATUADOR] Peltier: resfriamento ATIVADO");
            }
        } else {
            if (!relay1State || !relay2State || currentPeltierMode != HEATING) {
                digitalWrite(_pinRelay1, HIGH);
                digitalWrite(_pinRelay2, HIGH);
                currentPeltierMode = HEATING;
                relay1State = true;
                relay2State = true;
                stateChanged = true;
                Serial.println("🔥 [ATUADOR] Peltier: aquecimento ATIVADO");
            }
        }
        peltierActive   = true;
        lastPeltierTime = millis();
    } else {
        if (relay1State || relay2State) {
            digitalWrite(_pinRelay1, LOW);
            digitalWrite(_pinRelay2, LOW);
            peltierActive      = false;
            currentPeltierMode = OFF;
            relay1State        = false;
            relay2State        = false;
            stateChanged       = true;
            Serial.println("⭕ [ATUADOR] Peltier: DESLIGADO");
        }
    }
    
    if (stateChanged && firebaseHandler != nullptr && firebaseHandler->isAuthenticated() && 
        firebaseHandler->isFirebaseReady() && canWriteToFirebase()) {
        updateFirebaseStateImmediately();
    }
}

/**
 * @brief Controla os LEDs com fade suave não-bloqueante
 *
 * @details O fade usa delayMicroseconds() em vez de delay() para não
 * bloquear o loop principal por mais que alguns milissegundos por step.
 * Total de fade (51 steps × 3ms) ≈ 153ms — aceitável para transições visuais.
 *
 * IMPORTANTE: este método NÃO deve ser chamado com delay() de 10ms+ por step,
 * pois o loop principal precisa continuar processando sensores e WiFi.
 */
void ActuatorController::controlLEDs(bool on, int intensity) {
    bool stateChanged  = false;
    int  oldIntensity  = currentLEDIntensity;
    
    if (on) {
        for (int i = currentLEDIntensity; i <= intensity; i += 5) {
            analogWrite(_pinLED, i);
            delayMicroseconds(3000); // 3ms por step ≈ 153ms total — não bloqueia heap
        }
        analogWrite(_pinLED, intensity);
        currentLEDIntensity = intensity;
        if (oldIntensity != intensity) {
            stateChanged = true;
            Serial.printf("💡 [ATUADOR] LEDs LIGADO, intensidade: %d/255\n", intensity);
        }
    } else {
        for (int i = currentLEDIntensity; i >= 0; i -= 5) {
            analogWrite(_pinLED, i);
            delayMicroseconds(3000);
        }
        analogWrite(_pinLED, 0);
        if (currentLEDIntensity > 0) {
            stateChanged = true;
            Serial.println("💡 [ATUADOR] LEDs DESLIGADO");
        }
        currentLEDIntensity = 0;
    }
    
    if (stateChanged && firebaseHandler != nullptr && firebaseHandler->isAuthenticated() && 
        firebaseHandler->isFirebaseReady() && canWriteToFirebase()) {
        updateFirebaseStateImmediately();
    }
}

void ActuatorController::controlRelay(uint8_t relayNumber, bool state) {
    bool stateChanged = false;
    
    switch(relayNumber) {
        case 1: 
            if (relay1State != state) {
                digitalWrite(_pinRelay1, state ? HIGH : LOW);
                relay1State  = state;
                stateChanged = true;
            }
            break;
        case 2: 
            if (relay2State != state) {
                digitalWrite(_pinRelay2, state ? HIGH : LOW);
                relay2State  = state;
                stateChanged = true;
            }
            break;
        case 3: 
            if (relay3State != state) {
                digitalWrite(_pinRelay3, state ? HIGH : LOW);
                relay3State  = state;
                humidifierOn = state;
                stateChanged = true;
            }
            break;
        case 4: 
            if (relay4State != state) {
                digitalWrite(_pinRelay4, state ? HIGH : LOW);
                relay4State  = state;
                stateChanged = true;
            }
            break;
    }
    
    if (stateChanged) {
        Serial.printf("🔌 [ATUADOR] Rele %d: %s\n", relayNumber, state ? "LIGADO" : "DESLIGADO");
        
        if (firebaseHandler != nullptr && firebaseHandler->isAuthenticated() && 
            firebaseHandler->isFirebaseReady() && canWriteToFirebase()) {
            updateFirebaseStateImmediately();
        }
    }
}

// =============================================================================
// COMUNICAÇÃO COM FIREBASE
// =============================================================================

void ActuatorController::updateFirebaseState() {
    if (firebaseHandler != nullptr && firebaseHandler->isAuthenticated() && 
        firebaseHandler->isFirebaseReady() && canWriteToFirebase()) {
        firebaseHandler->updateActuatorState(
            relay1State, relay2State, relay3State, relay4State,
            (currentLEDIntensity > 0), currentLEDIntensity, humidifierOn
        );
    }
}

void ActuatorController::updateFirebaseStateImmediately() {
    if (firebaseHandler != nullptr && firebaseHandler->isAuthenticated() && 
        firebaseHandler->isFirebaseReady() && canWriteToFirebase()) {
        firebaseHandler->updateActuatorState(
            relay1State, relay2State, relay3State, relay4State,
            (currentLEDIntensity > 0), currentLEDIntensity, humidifierOn
        );
    }
}

// =============================================================================
// COMPATIBILIDADE
// =============================================================================

bool ActuatorController::heatPeltier(bool on) {
    controlPeltier(false, on);
    return true;
}

bool ActuatorController::areLEDsOn() const {
    return currentLEDIntensity > 0;
}

int ActuatorController::getLEDsWatts() const {
    return currentLEDIntensity;
}

int ActuatorController::getRelayState(uint8_t relayNumber) const {
    switch(relayNumber) {
        case 1: return relay1State;
        case 2: return relay2State;
        case 3: return relay3State;
        case 4: return relay4State;
        default: return -1;
    }
}

// =============================================================================
// MODO DEBUG E CONTROLE MANUAL
// =============================================================================

void ActuatorController::setDebugMode(bool debug) {
    if (debug != debugMode) {
        debugMode = debug;
        Serial.println(debugMode ? "🔧 DEBUG MODE: ON" : "🔧 DEBUG MODE: OFF");
        
        if (debugMode) {
            setFirebaseWriteBlock(true);
        }
        
        if (firebaseHandler != nullptr && firebaseHandler->isAuthenticated() && 
            firebaseHandler->isFirebaseReady() && canWriteToFirebase()) {
            updateFirebaseStateImmediately();
        }
    }
}

/**
 * @brief Aplica estados manuais aos atuadores (apenas em modo debug)
 *
 * @details CORREÇÃO: o fade de LEDs agora usa delayMicroseconds() em vez de
 * delay(10ms), eliminando o travamento de 510ms+ que impedia a comunicação
 * Firebase e leituras de sensor durante o ajuste manual de intensidade.
 */
void ActuatorController::setManualStates(bool relay1, bool relay2, bool relay3, bool relay4, bool ledsOn, int ledsIntensity, bool humidifierOn) {
    if (!debugMode) return;

    setFirebaseWriteBlock(true);

    bool anyChange = false;

    if (relay1 != relay1State) {
        digitalWrite(_pinRelay1, relay1 ? HIGH : LOW);
        relay1State = relay1;
        anyChange   = true;
        Serial.printf("🔧 [MANUAL] Relay 1: %s\n", relay1 ? "ON" : "OFF");
    }
    if (relay2 != relay2State) {
        digitalWrite(_pinRelay2, relay2 ? HIGH : LOW);
        relay2State = relay2;
        anyChange   = true;
        Serial.printf("🔧 [MANUAL] Relay 2: %s\n", relay2 ? "ON" : "OFF");
    }
    if (relay3 != relay3State) {
        digitalWrite(_pinRelay3, relay3 ? HIGH : LOW);
        relay3State       = relay3;
        this->humidifierOn = relay3;
        anyChange         = true;
        Serial.printf("🔧 [MANUAL] Relay 3 (Humidifier): %s\n", relay3 ? "ON" : "OFF");
    }
    if (relay4 != relay4State) {
        digitalWrite(_pinRelay4, relay4 ? HIGH : LOW);
        relay4State = relay4;
        anyChange   = true;
        Serial.printf("🔧 [MANUAL] Relay 4: %s\n", relay4 ? "ON" : "OFF");
    }

    // CORREÇÃO: usa delayMicroseconds (3ms/step) — não bloqueia o sistema.
    // O fade completo leva ~153ms no máximo, em vez dos ~510ms com delay(10).
    if (ledsOn != (currentLEDIntensity > 0) || (ledsOn && ledsIntensity != currentLEDIntensity)) {
        if (ledsOn) {
            for (int i = currentLEDIntensity; i <= ledsIntensity; i += 5) {
                analogWrite(_pinLED, i);
                delayMicroseconds(3000);
            }
            analogWrite(_pinLED, ledsIntensity);
            currentLEDIntensity = ledsIntensity;
        } else {
            for (int i = currentLEDIntensity; i >= 0; i -= 5) {
                analogWrite(_pinLED, i);
                delayMicroseconds(3000);
            }
            analogWrite(_pinLED, 0);
            currentLEDIntensity = 0;
        }
        anyChange = true;
        Serial.printf("🔧 [MANUAL] LEDs: %s, Intensidade: %d/255\n", ledsOn ? "ON" : "OFF", ledsIntensity);
    }
    
    if (anyChange) {
        Serial.println("✅ Manual changes applied successfully");
    }
}

// =============================================================================
// DEVMODE
// =============================================================================

void ActuatorController::setDevModeSettings(bool analogRead, bool digitalWrite, int pin, bool pwm, int pwmValue) {
    devModeAnalogRead   = analogRead;
    devModeDigitalWrite = digitalWrite;
    devModePWM          = pwm;
    devModePin          = pin;
    devModePWMValue     = pwmValue;
}

void ActuatorController::executeDevModeOperations() {
    if (devModePin < 0 || devModePin > 39) {
        Serial.println("❌ [DEVMODE] ERRO - Pino inválido: " + String(devModePin));
        return;
    }

    // Bloqueia pinos críticos do sistema
    const int criticalPins[] = {0, 1, 3, 6, 7, 8, 9, 10, 11};
    for (int cp : criticalPins) {
        if (devModePin == cp) {
            Serial.printf("🚫 [DEVMODE] BLOQUEADO - GPIO%d é pino crítico\n", devModePin);
            return;
        }
    }
    
    if (devModeAnalogRead) {
        int analogValue = analogRead(devModePin);
        Serial.printf("🔬 [DEVMODE] Analog Read - Pin %d: %d\n", devModePin, analogValue);
    }
    
    if (devModeDigitalWrite) {
        digitalWrite(devModePin, devModePWMValue > 0 ? HIGH : LOW);
        Serial.printf("🔬 [DEVMODE] Digital Write - Pin %d: %s\n", 
                     devModePin, devModePWMValue > 0 ? "HIGH" : "LOW");
    }
    
    if (devModePWM && !devModeDigitalWrite) {
        if (devModePWMValue < 0 || devModePWMValue > 255) {
            Serial.println("❌ [DEVMODE] Valor PWM inválido: " + String(devModePWMValue));
            return;
        }
        analogWrite(devModePin, devModePWMValue);
        Serial.printf("🔬 [DEVMODE] PWM Write - Pin %d: %d/255\n", devModePin, devModePWMValue);
    }
}

void ActuatorController::handleDevMode() {
    if (!debugMode) {
        if (lastDevModeState) {
            if (devModePin >= 0) {
                pinMode(devModePin, INPUT);
                Serial.printf("🔬 [DEVMODE] Pino %d resetado para INPUT\n", devModePin);
            }
            lastDevModeState = false;
        }
        return;
    }
    
    if (!lastDevModeState) {
        Serial.println("🔬 [DEVMODE] Modo desenvolvimento ATIVADO");
        lastDevModeState = true;
        
        if (devModePin >= 0 && devModePin <= 39) {
            if (devModeDigitalWrite || devModePWM) {
                pinMode(devModePin, OUTPUT);
            } else if (devModeAnalogRead) {
                pinMode(devModePin, INPUT);
            }
        }
    }
    
    executeDevModeOperations();
}

// =============================================================================
// AGENDADOR DE LEDs
// =============================================================================

void ActuatorController::applyLEDSchedule(unsigned long ts) {
    ledScheduler.update(ts, debugMode);
    if (ledScheduler.isActive() && !debugMode) {
        int schedIntensity = ledScheduler.wantsLEDsOn() ? ledScheduler.getIntensity() : 0;
        if (schedIntensity != currentLEDIntensity) {
            controlLEDs(schedIntensity > 0, schedIntensity);
        }
    }
}
