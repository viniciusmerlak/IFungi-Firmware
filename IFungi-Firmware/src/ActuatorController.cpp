#include "ActuatorController.h"
#include <Preferences.h>

// 🔥 NOVAS FUNÇÕES PARA CONTROLE DE ESCRITA
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
    
    // Se está bloqueado, verifica se já passou o tempo de bloqueio
    if (millis() - firebaseWriteBlockTime > FIREBASE_WRITE_BLOCK_DURATION) {
        blockFirebaseWrite = false;
        Serial.println("🔓 Firebase write auto-UNBLOCKED (timeout)");
        return true;
    }
    
    return false;
}

void ActuatorController::saveSetpointsNVS() {
    Preferences preferences;
    if(!preferences.begin("setpoints", false)) {
        Serial.println("Error opening NVS to save setpoints!");
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
    Serial.println("Setpoints saved to NVS");
}

bool ActuatorController::loadSetpointsNVS() {
    Preferences preferences;
    if(!preferences.begin("setpoints", true)) {
        Serial.println("Setpoints NVS not found, using defaults");
        return false;
    }
    
    if(preferences.isKey("lux")) {
        luxSetpoint = preferences.getInt("lux", 100);
        tempMin = preferences.getFloat("tMin", 20.0);
        tempMax = preferences.getFloat("tMax", 30.0);
        humidityMin = preferences.getFloat("uMin", 60.0);
        humidityMax = preferences.getFloat("uMax", 80.0);
        coSetpoint = preferences.getInt("coSp", 400);
        co2Setpoint = preferences.getInt("co2Sp", 400);
        tvocsSetpoint = preferences.getInt("tvocsSp", 100);
        
        preferences.end();
        Serial.println("Setpoints loaded from NVS");
        return true;
    } else {
        preferences.end();
        Serial.println("No setpoints saved in NVS, using defaults");
        return false;
    }
}

// Hysteresis definitions to avoid oscillations
const float HYSTERESIS_TEMP = 0.5f;
const float HYSTERESIS_HUMIDITY = 2.0f;

void ActuatorController::begin(uint8_t pinLED, uint8_t pinRelay1, uint8_t pinRelay2, 
                             uint8_t pinRelay3, uint8_t pinRelay4, uint8_t servoPin) {
    Serial.println("Initializing ActuatorController...");
    
    // Configure pins
    _pinLED = pinLED;
    _pinRelay1 = pinRelay1;
    _pinRelay2 = pinRelay2;
    _pinRelay3 = pinRelay3;
    _pinRelay4 = pinRelay4;
    _servoPin = servoPin;
    
    myServo.attach(servoPin);
    myServo.write(closedPosition);
    
    pinMode(_pinLED, OUTPUT);
    pinMode(_pinRelay1, OUTPUT);
    pinMode(_pinRelay2, OUTPUT);
    pinMode(_pinRelay3, OUTPUT);
    pinMode(_pinRelay4, OUTPUT);
    
    // Initialize all off
    digitalWrite(_pinLED, LOW);
    digitalWrite(_pinRelay1, LOW);
    digitalWrite(_pinRelay2, LOW);
    digitalWrite(_pinRelay3, LOW);
    digitalWrite(_pinRelay4, LOW);
    
    // Initialize states
    humidifierOn = false;
    peltierActive = false;
    lastPeltierTime = 0;
    lastUpdateTime = 0;
    inCooldown = false;
    cooldownStart = 0;
    
    // 🔥 INICIALIZAÇÃO DAS NOVAS VARIÁVEIS
    blockFirebaseWrite = false;
    firebaseWriteBlockTime = 0;
    
    Serial.println("ActuatorController initialized successfully");
}

void ActuatorController::setFirebaseHandler(FirebaseHandler* handler) {
    firebaseHandler = handler;
    Serial.println("FirebaseHandler set for ActuatorController");
}

void ActuatorController::applySetpoints(int lux, float tMin, float tMax, float uMin, float uMax, int coSp, int co2Sp, int tvocsSp) {
    luxSetpoint = lux;
    tempMin = tMin;
    tempMax = tMax;
    humidityMin = uMin;
    humidityMax = uMax;
    coSetpoint = coSp;
    co2Setpoint = co2Sp;
    tvocsSetpoint = tvocsSp;
    
    // Save setpoints to NVS
    saveSetpointsNVS();
    
    Serial.printf("Setpoints applied: Lux=%d, Temp=[%.1f-%.1f], Humidity=[%.1f-%.1f], CO=%d, CO2=%d, TVOCs=%d\n", 
                 lux, tMin, tMax, uMin, uMax, coSp, co2Sp, tvocsSp);
}

void ActuatorController::controlAutomatically(float temp, float humidity, int light, int co, int co2, int tvocs, bool waterLevel) {
    // 🔥 CORREÇÃO: Não executa controle automático em modo debug
    if (debugMode) {
        return;
    }
    
    // Restante do código do controle automático...
    if (peltierActive && currentPeltierMode == HEATING && 
        (millis() - lastPeltierTime >= operationTime)) {
        Serial.println("[SAFETY] Peltier operation time exceeded, starting cooldown");
        controlPeltier(false, false);
        inCooldown = true;
        cooldownStart = millis();
    }
    
    // Check if cooldown ended (only for heating)
    if (inCooldown && (millis() - cooldownStart >= cooldownTime)) {
        Serial.println("[SAFETY] Cooldown finished, Peltier available");
        inCooldown = false;
    }

    // 1. Temperature control - Peltier (Relay 1 and 2) with hysteresis
    if (temp < (tempMin - HYSTERESIS_TEMP)) {
        // Temperature below minimum - Heat
        if ((!peltierActive || currentPeltierMode != HEATING) && !inCooldown) {
            Serial.printf("[ACTUATOR] Temperature below (%.1f < %.1f), heating\n", temp, tempMin);
            controlPeltier(false, true); // Heat (cooling = false)
        }
    } 
    else if (temp > (tempMax + HYSTERESIS_TEMP)) {
        // Temperature above maximum - Cool
        if (!peltierActive || currentPeltierMode != COOLING) {
            Serial.printf("[ACTUATOR] Temperature above (%.1f > %.1f), cooling\n", temp, tempMax);
            controlPeltier(true, true); // Cool (cooling = true)
        }
    }
    else if (peltierActive) {
        // Temperature within range - Turn off Peltier
        Serial.println("[ACTUATOR] Temperature OK, turning off Peltier");
        controlPeltier(false, false); // Turn off
    }
    
    // 2. Humidity control - CORRECTED LOGIC
    if (!waterLevel) {
        Serial.println("[SAFETY] Low water level, turning off humidifier");
        if (humidifierOn) {
            controlRelay(3, false); // Turn off humidifier
            humidifierOn = false;
        }
    } 
    else {
        // Only control humidity if water level is OK
        if (humidity < (humidityMin - HYSTERESIS_HUMIDITY) && !humidifierOn) {
            Serial.printf("[ACTUATOR] Humidity below (%.1f < %.1f), turning on humidifier\n", humidity, humidityMin);
            controlRelay(3, true); // Turn on humidifier
            humidifierOn = true;
        } 
        else if (humidity > (humidityMax + HYSTERESIS_HUMIDITY) && humidifierOn) {
            Serial.printf("[ACTUATOR] Humidity above (%.1f > %.1f), turning off humidifier\n", humidity, humidityMax);
            controlRelay(3, false); // Turn off humidifier
            humidifierOn = false;
        }
    }
    
    // 3. Light control - LEDs
    int newIntensity = (light < luxSetpoint) ? 255 : 0;
    
    if (newIntensity != currentLEDIntensity) {
        controlLEDs(newIntensity > 0, newIntensity);
    }
    
    // 4. Update state to Firebase periodicamente - CORREÇÃO: Verifica se pode escrever
    if (firebaseHandler != nullptr && firebaseHandler->isAuthenticated() && firebaseHandler->isFirebaseReady()) {
        if (millis() - lastUpdateTime > 5000 && canWriteToFirebase()) {
            updateFirebaseState();
            lastUpdateTime = millis();
        }
    }
    
    // 5. Gas control (CO, CO2, TVOCs)
    if (co > coSetpoint || co2 > co2Setpoint || tvocs > tvocsSetpoint) {
        Serial.printf("[ACTUATOR] Gases above limit (CO: %d, CO2: %d, TVOCs: %d), turning on exhaust\n", 
                     co, co2, tvocs);
        myServo.write(openPosition);
        controlRelay(4, true); // Turn on exhaust
    } else {
        myServo.write(closedPosition);
        controlRelay(4, false); // Turn off exhaust
    }
}

void ActuatorController::controlPeltier(bool cooling, bool on) {
    bool stateChanged = false;
    
    if (on) {
        // Check cooldown only for heating
        if (inCooldown && !cooling) {
            Serial.println("[SAFETY] Peltier in cooldown, cannot turn on for heating");
            return;
        }
        
        if (cooling) {
            // Cooling mode: Relay1 HIGH, Relay2 LOW (no timeout)
            if (!relay1State || relay2State || currentPeltierMode != COOLING) {
                digitalWrite(_pinRelay1, HIGH);
                digitalWrite(_pinRelay2, LOW);
                currentPeltierMode = COOLING;
                relay1State = true;
                relay2State = false;
                stateChanged = true;
                Serial.println("[PELTIER] Cooling mode (no timeout)");
            }
        } else {
            // Heating mode: Relay1 HIGH, Relay2 HIGH (with timeout)
            if (!relay1State || !relay2State || currentPeltierMode != HEATING) {
                digitalWrite(_pinRelay1, HIGH);
                digitalWrite(_pinRelay2, HIGH);
                currentPeltierMode = HEATING;
                relay1State = true;
                relay2State = true;
                stateChanged = true;
                Serial.println("[PELTIER] Heating mode (with timeout)");
            }
        }
        peltierActive = true;
        lastPeltierTime = millis();
    } else {
        // Turn off both relays
        if (relay1State || relay2State) {
            digitalWrite(_pinRelay1, LOW);
            digitalWrite(_pinRelay2, LOW);
            peltierActive = false;
            currentPeltierMode = OFF;
            relay1State = false;
            relay2State = false;
            stateChanged = true;
            Serial.println("[PELTIER] Turned off");
        }
    }
    
    // 🔥 CORREÇÃO: Atualiza Firebase apenas se pode escrever
    if (stateChanged && firebaseHandler != nullptr && firebaseHandler->isAuthenticated() && 
        firebaseHandler->isFirebaseReady() && canWriteToFirebase()) {
        updateFirebaseStateImmediately();
    }
}

void ActuatorController::controlLEDs(bool on, int intensity) {
    bool stateChanged = false;
    int oldIntensity = currentLEDIntensity;
    
    if (on) {
        // Smooth transition to turn on
        for (int i = currentLEDIntensity; i <= intensity; i += 5) {
            analogWrite(_pinLED, i);
            delay(10);
        }
        currentLEDIntensity = intensity;
        if (oldIntensity != intensity) {
            stateChanged = true;
            Serial.printf("[LEDS] Turned on, Intensity: %d/255\n", intensity);
        }
    } else {
        // Smooth transition to turn off
        for (int i = currentLEDIntensity; i >= 0; i -= 5) {
            analogWrite(_pinLED, i);
            delay(10);
        }
        if (currentLEDIntensity > 0) {
            stateChanged = true;
            Serial.println("[LEDS] Turned off");
        }
        currentLEDIntensity = 0;
    }
    
    // 🔥 CORREÇÃO: Atualiza Firebase apenas se pode escrever
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
                relay1State = state;
                stateChanged = true;
            }
            break;
        case 2: 
            if (relay2State != state) {
                digitalWrite(_pinRelay2, state ? HIGH : LOW);
                relay2State = state;
                stateChanged = true;
            }
            break;
        case 3: 
            if (relay3State != state) {
                digitalWrite(_pinRelay3, state ? HIGH : LOW);
                relay3State = state;
                humidifierOn = state; // Update humidifier state
                stateChanged = true;
            }
            break;
        case 4: 
            if (relay4State != state) {
                digitalWrite(_pinRelay4, state ? HIGH : LOW);
                relay4State = state;
                stateChanged = true;
            }
            break;
    }
    
    if (stateChanged) {
        Serial.printf("[RELAY %d] %s\n", relayNumber, state ? "ON" : "OFF");
        
        // 🔥 CORREÇÃO: Atualiza Firebase apenas se pode escrever
        if (firebaseHandler != nullptr && firebaseHandler->isAuthenticated() && 
            firebaseHandler->isFirebaseReady() && canWriteToFirebase()) {
            updateFirebaseStateImmediately();
        }
    }
}

void ActuatorController::updateFirebaseState() {
    if (firebaseHandler != nullptr && firebaseHandler->isAuthenticated() && 
        firebaseHandler->isFirebaseReady() && canWriteToFirebase()) {
        firebaseHandler->updateActuatorState(
            relay1State,
            relay2State, 
            relay3State, 
            relay4State, 
            (currentLEDIntensity > 0), 
            currentLEDIntensity,
            humidifierOn
        );
        Serial.println("✅ Estado dos atuadores atualizado no Firebase (periódico)");
    }
}

void ActuatorController::updateFirebaseStateImmediately() {
    if (firebaseHandler != nullptr && firebaseHandler->isAuthenticated() && 
        firebaseHandler->isFirebaseReady() && canWriteToFirebase()) {
        firebaseHandler->updateActuatorState(
            relay1State,
            relay2State, 
            relay3State, 
            relay4State, 
            (currentLEDIntensity > 0), 
            currentLEDIntensity,
            humidifierOn
        );
        Serial.println("✅ Estado dos atuadores atualizado no Firebase (imediato)");
    }
}

bool ActuatorController::heatPeltier(bool on) {
    // Function kept for compatibility
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
        default: return -1; // Invalid state
    }
}

// 🔥 NOVAS FUNÇÕES PARA DEBUG
void ActuatorController::setDebugMode(bool debug) {
    if (debug != debugMode) {
        debugMode = debug;
        Serial.println(debugMode ? "🔧 DEBUG MODE: ON" : "🔧 DEBUG MODE: OFF");
        
        // Quando entra no modo debug, bloqueia escrita por um tempo
        if (debugMode) {
            setFirebaseWriteBlock(true);
        }
        
        // Atualiza o Firebase imediatamente quando o modo debug muda
        if (firebaseHandler != nullptr && firebaseHandler->isAuthenticated() && 
            firebaseHandler->isFirebaseReady() && canWriteToFirebase()) {
            updateFirebaseStateImmediately();
        }
    }
}
void ActuatorController::setManualStates(bool relay1, bool relay2, bool relay3, bool relay4, bool ledsOn, int ledsIntensity, bool humidifierOn) {
    if (!debugMode) return;

    // 🔥 BLOQUEIA ESCRITA NO FIREBASE durante mudanças manuais por TEMPO MAIOR
    setFirebaseWriteBlock(true);

    // 🔥 CORREÇÃO: Variáveis para detectar mudanças reais
    bool anyChange = false;

    // Aplica os estados manuais apenas se diferentes dos atuais
    if (relay1 != relay1State) {
        digitalWrite(_pinRelay1, relay1 ? HIGH : LOW);
        relay1State = relay1;
        anyChange = true;
        Serial.printf("[MANUAL] Relay 1: %s\n", relay1 ? "ON" : "OFF");
    }
    if (relay2 != relay2State) {
        digitalWrite(_pinRelay2, relay2 ? HIGH : LOW);
        relay2State = relay2;
        anyChange = true;
        Serial.printf("[MANUAL] Relay 2: %s\n", relay2 ? "ON" : "OFF");
    }
    if (relay3 != relay3State) {
        digitalWrite(_pinRelay3, relay3 ? HIGH : LOW);
        relay3State = relay3;
        this->humidifierOn = relay3;
        anyChange = true;
        Serial.printf("[MANUAL] Relay 3 (Humidifier): %s\n", relay3 ? "ON" : "OFF");
    }
    if (relay4 != relay4State) {
        digitalWrite(_pinRelay4, relay4 ? HIGH : LOW);
        relay4State = relay4;
        anyChange = true;
        Serial.printf("[MANUAL] Relay 4: %s\n", relay4 ? "ON" : "OFF");
    }
    if (ledsOn != (currentLEDIntensity > 0) || (ledsOn && ledsIntensity != currentLEDIntensity)) {
        // Aplica mudança de LEDs sem atualizar Firebase (já está bloqueado)
        if (ledsOn) {
            for (int i = currentLEDIntensity; i <= ledsIntensity; i += 5) {
                analogWrite(_pinLED, i);
                delay(10);
            }
            currentLEDIntensity = ledsIntensity;
        } else {
            for (int i = currentLEDIntensity; i >= 0; i -= 5) {
                analogWrite(_pinLED, i);
                delay(10);
            }
            currentLEDIntensity = 0;
        }
        anyChange = true;
        Serial.printf("[MANUAL] LEDs: %s, Intensity: %d/255\n", ledsOn ? "ON" : "OFF", ledsIntensity);
    }
    
    if (anyChange) {
        Serial.println("✅ Manual changes applied successfully");
    }
    
    // 🔥 ATUALIZAÇÃO: NÃO atualiza Firebase imediatamente após mudanças manuais
    // Deixa o bloqueio ativo para prevenir escrita competitiva
    // O Firebase será atualizado apenas quando o bloqueio expirar (10 segundos)
}
