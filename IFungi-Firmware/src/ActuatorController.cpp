#include "ActuatorController.h"
#include "OperationMode.h"
#include <Preferences.h>

// =============================================================================
// LED — driver com PWM invertido (255 no pino = apagado, 0 = máximo brilho)
// =============================================================================

int ActuatorController::ledLogicalToHardwarePwm(int logical) {
    int v = logical;
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return 255 - v;
}

void ActuatorController::writeLedHardwareFromLogical(int logical) {
    analogWrite(_pinLED, ledLogicalToHardwarePwm(logical));
}

void ActuatorController::ledPwmTask(void* parameter) {
    ActuatorController* self = static_cast<ActuatorController*>(parameter);
    const int step = 5;

    for (;;) {
        int current = self->currentLEDIntensity;
        int target  = self->targetLEDIntensity;

        if (target > current) {
            current += step;
            if (current > target) current = target;
            self->writeLedHardwareFromLogical(current);
            self->currentLEDIntensity = current;
        } else if (target < current) {
            current -= step;
            if (current < target) current = target;
            self->writeLedHardwareFromLogical(current);
            self->currentLEDIntensity = current;
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

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
        coSetpoint    = preferences.getInt("coSp", 50);
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
    
    // LED apagado (lógico 0) = PWM físico alto no driver invertido
    writeLedHardwareFromLogical(0);
    digitalWrite(_pinRelay1, LOW);
    digitalWrite(_pinRelay2, LOW);
    digitalWrite(_pinRelay3, LOW);
    digitalWrite(_pinRelay4, LOW);
    
    humidifierOn        = false;
    peltierActive       = false;
    lastPeltierTime     = 0;
    peltierHeatingStart = 0;
    currentLEDIntensity = 0;
    targetLEDIntensity  = 0;
    lastUpdateTime      = 0;
    inCooldown          = false;
    cooldownStart       = 0;
    blockFirebaseWrite  = false;
    firebaseWriteBlockTime = 0;

    xTaskCreatePinnedToCore(
        ledPwmTask,
        "LED_PWM_Task",
        2048,
        this,
        1,
        &ledPwmTaskHandle,
        1
    );
    
    Serial.println("✅ ActuatorController initialized successfully");
}

void ActuatorController::setFirebaseHandler(FirebaseHandler* handler) {
    firebaseHandler = handler;
    Serial.println("🔥 FirebaseHandler set for ActuatorController");
}

// =============================================================================
// APPLY SETPOINTS — CORRIGIDO
// Antes: saveSetpointsNVS() era chamada SEMPRE, a cada 5s, mesmo sem mudança.
// Depois: compara campo a campo; só grava na NVS quando algo realmente mudou.
// =============================================================================

void ActuatorController::applySetpoints(int lux, float tMin, float tMax,
                                        float uMin, float uMax,
                                        int coSp, int co2Sp, int tvocsSp) {
    // Detecta se ao menos um valor realmente mudou
    bool changed = false;

    if (lux     != luxSetpoint)                   changed = true;
    if (coSp    != coSetpoint)                    changed = true;
    if (co2Sp   != co2Setpoint)                   changed = true;
    if (tvocsSp != tvocsSetpoint)                 changed = true;
    if (fabsf(tMin - tempMin)     > 0.01f)        changed = true;
    if (fabsf(tMax - tempMax)     > 0.01f)        changed = true;
    if (fabsf(uMin - humidityMin) > 0.01f)        changed = true;
    if (fabsf(uMax - humidityMax) > 0.01f)        changed = true;

    if (!changed) {
        // Firebase tem os mesmos valores — nada a fazer, não grava NVS
        return;
    }

    // Aplica os novos valores
    luxSetpoint   = lux;
    tempMin       = tMin;
    tempMax       = tMax;
    humidityMin   = uMin;
    humidityMax   = uMax;
    coSetpoint    = coSp;
    co2Setpoint   = co2Sp;
    tvocsSetpoint = tvocsSp;

    // Persiste na NVS somente quando houve mudança real
    saveSetpointsNVS();
    
    Serial.printf("⚙️ Setpoints ATUALIZADOS: Lux=%d, Temp=[%.1f-%.1f], Hum=[%.1f-%.1f], CO=%d, CO2=%d, TVOCs=%d\n", 
                 lux, tMin, tMax, uMin, uMax, coSp, co2Sp, tvocsSp);
}

// =============================================================================
// MODOS DE OPERAÇÃO — CORRIGIDO
// Antes: applyOperationMode() chamava applySetpoints() com valores hardcoded
//        do preset, apagando os setpoints que o usuário configurou no app.
// Depois: o modo controla SOMENTE as restrições de atuadores (quem pode
//         ligar/desligar) e o scheduler de LEDs. Os setpoints numéricos
//         (temperatura, umidade, CO2, lux) NUNCA são tocados pela troca de modo.
// =============================================================================

void ActuatorController::applyOperationMode(OperationMode mode) {
    if (mode == _currentMode) return;

    _currentMode = mode;
    ModePreset p = getModePreset(mode);

    Serial.printf("[MODE] 🍄 Mudando para modo: %s\n", operationModeLabel(mode).c_str());

    // Aplica restrições de atuadores do modo
    _humidifierAllowed = p.humidifierEnabled;
    _ledsAllowed       = p.ledsEnabled;
    _peltierAllowed    = p.peltierEnabled;
    _exhaustForced     = p.exhaustForcedOn;

    // REMOVIDO: applySetpoints() com valores hardcoded do preset.
    // O app é a única fonte de verdade para setpoints numéricos.
    // Se o app quiser sugerir setpoints ao trocar de modo, ele mesmo deve
    // escrever em /setpoints no Firebase — o ESP32 lerá no próximo ciclo.

    // Controle de LEDs pelo modo
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

    // Desliga imediatamente atuadores proibidos neste modo
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
    
    // ── PELTIER (R1=alimentação, R2=polaridade; cooldown só no aquecimento R1+R2) ──
    if (inCooldown && (millis() - cooldownStart >= cooldownTime)) {
        inCooldown = false;
    }

    if (currentPeltierMode == HEATING && peltierActive && peltierHeatingStart > 0 &&
        (millis() - peltierHeatingStart >= operationTime)) {
        Serial.println("🛑 [PEL] Limite de aquecimento contínuo — desligando (cooldown)");
        controlPeltier(false, false);
        inCooldown    = true;
        cooldownStart = millis();
    }

    if (!_peltierAllowed) {
        if (peltierActive) controlPeltier(false, false);
    } else {
        bool wantHeat = temp < (tempMin - HYSTERESIS_TEMP);
        bool wantCool = temp > (tempMax + HYSTERESIS_TEMP);
        if (inCooldown && wantHeat) {
            wantHeat = false;
        }

        if (wantHeat) {
            if (!peltierActive || currentPeltierMode != HEATING) {
                Serial.printf("🔥 [ACTUATOR] Temp abaixo (%.1f < %.1f), aquecendo\n", temp, tempMin);
                controlPeltier(false, true);
            }
        } else if (wantCool) {
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
        // LDR em ADC: pouca luz → ADC baixo → aumentar brilho (lógico 255)
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
            // R1 ligado + R2 desligado = resfriar (polaridade A)
            if (!relay1State || relay2State || currentPeltierMode != COOLING) {
                digitalWrite(_pinRelay1, HIGH);
                digitalWrite(_pinRelay2, LOW);
                currentPeltierMode = COOLING;
                relay1State = true;
                relay2State = false;
                stateChanged = true;
                peltierHeatingStart = 0;
                Serial.println("❄️ [ATUADOR] Peltier: resfriamento (R1 on, R2 off)");
            }
        } else {
            // R1 ligado + R2 ligado = aquecer (polaridade B)
            if (!relay1State || !relay2State || currentPeltierMode != HEATING) {
                digitalWrite(_pinRelay1, HIGH);
                digitalWrite(_pinRelay2, HIGH);
                currentPeltierMode = HEATING;
                relay1State = true;
                relay2State = true;
                stateChanged = true;
                peltierHeatingStart = millis();
                Serial.println("🔥 [ATUADOR] Peltier: aquecimento (R1 on, R2 on)");
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
            peltierHeatingStart = 0;
            stateChanged       = true;
            Serial.println("⭕ [ATUADOR] Peltier: DESLIGADO (R1/R2 off)");
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
 */
void ActuatorController::controlLEDs(bool on, int intensity) {
    if (intensity < 0) intensity = 0;
    if (intensity > 255) intensity = 255;

    bool stateChanged  = false;
    int oldTarget = targetLEDIntensity;
    int newTarget = on ? intensity : 0;

    if (newTarget != oldTarget) {
        targetLEDIntensity = newTarget;
        stateChanged = true;
        if (newTarget > 0) {
            Serial.printf("💡 [ATUADOR] LEDs LIGADO (task), alvo: %d/255\n", newTarget);
        } else {
            Serial.println("💡 [ATUADOR] LEDs DESLIGADO (task)");
        }
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

    int requestedIntensity = ledsOn ? ledsIntensity : 0;
    if (requestedIntensity < 0) requestedIntensity = 0;
    if (requestedIntensity > 255) requestedIntensity = 255;
    if (requestedIntensity != targetLEDIntensity) {
        controlLEDs(requestedIntensity > 0, requestedIntensity);
        anyChange = true;
        Serial.printf("🔧 [MANUAL] LEDs: %s, Alvo: %d/255\n", requestedIntensity > 0 ? "ON" : "OFF", requestedIntensity);
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
