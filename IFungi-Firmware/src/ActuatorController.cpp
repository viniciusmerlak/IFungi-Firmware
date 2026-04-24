#include "ActuatorController.h"
#include <Preferences.h>

// =============================================================================
// MÉTODOS DE CONTROLE DE ESCRITA NO FIREBASE
// =============================================================================

/**
 * @brief Ativa ou desativa o bloqueio de escrita no Firebase
 * @param block true para bloquear, false para desbloquear
 * 
 * @note Utilizado durante operações manuais para evitar conflitos de escrita
 */
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

/**
 * @brief Verifica se é possível escrever no Firebase
 * @return true se pode escrever, false caso contrário
 * 
 * @note Considera bloqueio manual e timeout automático
 */
bool ActuatorController::canWriteToFirebase() {
    if (!blockFirebaseWrite) {
        return true;
    }
    
    // Verifica se o tempo de bloqueio expirou
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

/**
 * @brief Salva os setpoints atuais na memória não volátil (NVS)
 * 
 * @note Garante que os setpoints sejam mantidos após reinicialização
 */
void ActuatorController::saveSetpointsNVS() {
    Preferences preferences;
    if(!preferences.begin("setpoints", false)) {
        Serial.println("❌ Error opening NVS to save setpoints!");
        return;
    }
    
    // Salva todos os setpoints individualmente
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

/**
 * @brief Carrega os setpoints da memória não volátil (NVS)
 * @return true se carregou com sucesso, false se usa valores padrão
 */
bool ActuatorController::loadSetpointsNVS() {
    Preferences preferences;
    if(!preferences.begin("setpoints", true)) {
        Serial.println("❌ Setpoints NVS not found, using defaults");
        return false;
    }
    
    // Verifica se existem setpoints salvos
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
        Serial.println("📁 Setpoints loaded from NVS");
        return true;
    } else {
        preferences.end();
        Serial.println("❌ No setpoints saved in NVS, using defaults");
        return false;
    }
}

// =============================================================================
// DEFINIÇÕES DE HISTERESE PARA EVITAR OSCILAÇÃO
// =============================================================================

const float HYSTERESIS_TEMP = 0.5f;        ///< Histerese para temperatura (°C)
const float HYSTERESIS_HUMIDITY = 2.0f;    ///< Histerese para umidade (%)

// =============================================================================
// INICIALIZAÇÃO E CONFIGURAÇÃO
// =============================================================================

/**
 * @brief Inicializa o controlador de atuadores
 * @param pinLED Pino dos LEDs
 * @param pinRelay1 Pino do relé 1 (Peltier - comum)
 * @param pinRelay2 Pino do relé 2 (Peltier - direção)
 * @param pinRelay3 Pino do relé 3 (Umidificador)
 * @param pinRelay4 Pino do relé 4 (Exaustor)
 * @param servoPin Pino do servo motor
 */
void ActuatorController::begin(uint8_t pinLED, uint8_t pinRelay1, uint8_t pinRelay2, 
                             uint8_t pinRelay3, uint8_t pinRelay4, uint8_t servoPin) {
    Serial.println("🔧 Initializing ActuatorController...");
    
    // Configuração dos pinos
    _pinLED = pinLED;
    _pinRelay1 = pinRelay1;
    _pinRelay2 = pinRelay2;
    _pinRelay3 = pinRelay3;
    _pinRelay4 = pinRelay4;
    _servoPin = servoPin;
    
    // Configuração do servo motor
    myServo.attach(servoPin);
    myServo.write(closedPosition);
    
    // Configuração dos pinos como saída
    pinMode(_pinLED, OUTPUT);
    pinMode(_pinRelay1, OUTPUT);
    pinMode(_pinRelay2, OUTPUT);
    pinMode(_pinRelay3, OUTPUT);
    pinMode(_pinRelay4, OUTPUT);
    
    // Inicializa todos os atuadores desligados
    digitalWrite(_pinLED, LOW);
    digitalWrite(_pinRelay1, LOW);
    digitalWrite(_pinRelay2, LOW);
    digitalWrite(_pinRelay3, LOW);
    digitalWrite(_pinRelay4, LOW);
    
    // Inicialização de variáveis de estado
    humidifierOn = false;
    peltierActive = false;
    lastPeltierTime = 0;
    lastUpdateTime = 0;
    inCooldown = false;
    cooldownStart = 0;
    
    // Inicialização das variáveis de controle de escrita
    blockFirebaseWrite = false;
    firebaseWriteBlockTime = 0;
    
    Serial.println("✅ ActuatorController initialized successfully");
}

/**
 * @brief Define o handler do Firebase para sincronização
 * @param handler Ponteiro para o objeto FirebaseHandler
 */
void ActuatorController::setFirebaseHandler(FirebaseHandler* handler) {
    firebaseHandler = handler;
    Serial.println("🔥 FirebaseHandler set for ActuatorController");
}

/**
 * @brief Aplica novos setpoints e os salva no NVS
 * @param lux Setpoint de luminosidade
 * @param tMin Temperatura mínima
 * @param tMax Temperatura máxima
 * @param uMin Umidade mínima
 * @param uMax Umidade máxima
 * @param coSp Setpoint de CO
 * @param co2Sp Setpoint de CO2
 * @param tvocsSp Setpoint de TVOCs
 */
void ActuatorController::applySetpoints(int lux, float tMin, float tMax, float uMin, float uMax, int coSp, int co2Sp, int tvocsSp) {
    luxSetpoint = lux;
    tempMin = tMin;
    tempMax = tMax;
    humidityMin = uMin;
    humidityMax = uMax;
    coSetpoint = coSp;
    co2Setpoint = co2Sp;
    tvocsSetpoint = tvocsSp;
    
    // Persiste os setpoints no NVS
    saveSetpointsNVS();
    
    Serial.printf("⚙️ Setpoints applied: Lux=%d, Temp=[%.1f-%.1f], Humidity=[%.1f-%.1f], CO=%d, CO2=%d, TVOCs=%d\n", 
                 lux, tMin, tMax, uMin, uMax, coSp, co2Sp, tvocsSp);
}

// =============================================================================
// CONTROLE AUTOMÁTICO PRINCIPAL
// =============================================================================

/**
 * @brief Executa o controle automático dos atuadores baseado nos sensores
 * @param temp Temperatura atual
 * @param humidity Umidade atual
 * @param light Luminosidade atual
 * @param co Nível de CO
 * @param co2 Nível de CO2
 * @param tvocs Nível de TVOCs
 * @param waterLevel Nível de água (true = baixo)
 * 
 * @note Não executa em modo debug para permitir controle manual
 */
void ActuatorController::controlAutomatically(float temp, float humidity, int light, int co, int co2, int tvocs, bool waterLevel) {
    // Não executa controle automático em modo debug
    if (debugMode) {
        return;
    }
    
    // =========================================================================
    // CONTROLE DE SEGURANÇA DO PELTIER
    // =========================================================================
    
    // Verifica tempo máximo de operação em modo aquecimento
    if (peltierActive && currentPeltierMode == HEATING && 
        (millis() - lastPeltierTime >= operationTime)) {
        Serial.println("🛑 [SAFETY] Peltier operation time exceeded, starting cooldown");
        controlPeltier(false, false);
        inCooldown = true;
        cooldownStart = millis();
    }
    
    // Verifica fim do período de cooldown (apenas para aquecimento)
    if (inCooldown && (millis() - cooldownStart >= cooldownTime)) {
        Serial.println("✅ [SAFETY] Cooldown finished, Peltier available");
        inCooldown = false;
    }

    // =========================================================================
    // CONTROLE DE TEMPERATURA (PELTIER)
    // =========================================================================
    
    if (temp < (tempMin - HYSTERESIS_TEMP)) {
        // Temperatura abaixo do mínimo - Aquecer
        if ((!peltierActive || currentPeltierMode != HEATING) && !inCooldown) {
            Serial.printf("🔥 [ACTUATOR] Temperature below (%.1f < %.1f), heating\n", temp, tempMin);
            controlPeltier(false, true); // Modo aquecimento
        }
    } 
    else if (temp > (tempMax + HYSTERESIS_TEMP)) {
        // Temperatura acima do máximo - Resfriar
        if (!peltierActive || currentPeltierMode != COOLING) {
            Serial.printf("❄️ [ACTUATOR] Temperature above (%.1f > %.1f), cooling\n", temp, tempMax);
            controlPeltier(true, true); // Modo resfriamento
        }
    }
    else if (peltierActive) {
        // Temperatura dentro da faixa - Desligar Peltier
        Serial.println("✅ [ACTUATOR] Temperature OK, turning off Peltier");
        controlPeltier(false, false); // Desligar
    }
    
    // =========================================================================
    // CONTROLE DE UMIDADE (UMIDIFICADOR)
    // =========================================================================
    
    // Lógica corrigida do sensor de água:
    // waterLevel = TRUE quando água está BAIXA (sensor seco)
    // waterLevel = FALSE quando água está OK (sensor molhado)
    
    if (waterLevel) {
        // Água BAIXA - Desliga umidificador por segurança
        Serial.println("🛑 [SAFETY] Low water level, turning off humidifier");
        if (humidifierOn) {
            controlRelay(3, false); // Desliga umidificador
            humidifierOn = false;
        }
    } 
    else {
        // Água OK - Controla umidificador normalmente baseado na umidade
        if (humidity < (humidityMin - HYSTERESIS_HUMIDITY)) {
            // Umidade abaixo do mínimo - Liga umidificador
            if (!humidifierOn) {
                Serial.printf("💧 [ACTUATOR] Humidity below (%.1f < %.1f), turning ON humidifier\n", humidity, humidityMin);
                controlRelay(3, true); // Liga umidificador
                humidifierOn = true;
            }
        } 
        else if (humidity > (humidityMax + HYSTERESIS_HUMIDITY)) {
            // Umidade acima do máximo - Desliga umidificador
            if (humidifierOn) {
                Serial.printf("💧 [ACTUATOR] Humidity above (%.1f > %.1f), turning OFF humidifier\n", humidity, humidityMax);
                controlRelay(3, false); // Desliga umidificador
                humidifierOn = false;
            }
        }
        // Se a umidade está dentro da faixa, mantém o estado atual
    }
    
    // =========================================================================
    // CONTROLE DE LUMINOSIDADE (LEDs)
    // =========================================================================
    //
    // Prioridade:
    //  1. ledScheduler ativo (timer ou solar) → scheduler decide intensidade
    //  2. Scheduler inativo → lógica automática por LDR (comportamento original)
    //
    if (ledScheduler.isActive()) {
        // O scheduler já foi atualizado em applyLEDSchedule() antes desta chamada.
        // Aqui apenas aplicamos a decisão que ele calculou.
        int schedIntensity = ledScheduler.wantsLEDsOn() ? ledScheduler.getIntensity() : 0;
        if (schedIntensity != currentLEDIntensity) {
            controlLEDs(schedIntensity > 0, schedIntensity);
        }
    } else {
        // Modo automático clássico: compensa luminosidade abaixo do setpoint
        int newIntensity = (light < luxSetpoint) ? 255 : 0;
        if (newIntensity != currentLEDIntensity) {
            controlLEDs(newIntensity > 0, newIntensity);
        }
    }
    
    // =========================================================================
    // ATUALIZAÇÃO PERIÓDICA DO FIREBASE
    // =========================================================================
    
    if (firebaseHandler != nullptr && firebaseHandler->isAuthenticated() && firebaseHandler->isFirebaseReady()) {
        if (millis() - lastUpdateTime > 5000 && canWriteToFirebase()) {
            updateFirebaseState();
            lastUpdateTime = millis();
        }
    }
    
    // =========================================================================
    // CONTROLE DE GASES (EXAUSTOR)
    // =========================================================================
    
    if (co > coSetpoint || co2 > co2Setpoint || tvocs > tvocsSetpoint) {
        Serial.printf("💨 [ACTUATOR] Gases above limit (CO: %d, CO2: %d, TVOCs: %d), turning on exhaust\n", 
                     co, co2, tvocs);
        myServo.write(openPosition);        // Abre dampers
        controlRelay(4, true);              // Liga exaustor
    } else {
        myServo.write(closedPosition);      // Fecha dampers
        controlRelay(4, false);             // Desliga exaustor
    }
}

// =============================================================================
// MÉTODOS DE CONTROLE INDIVIDUAL DOS ATUADORES
// =============================================================================

/**
 * @brief Controla o módulo Peltier (aquecimento/resfriamento)
 * @param cooling true para resfriamento, false para aquecimento
 * @param on true para ligar, false para desligar
 */
void ActuatorController::controlPeltier(bool cooling, bool on) {
    bool stateChanged = false;
    
    if (on) {
        // Verifica se pode ligar (bloqueio por cooldown)
        if (inCooldown && !cooling) {
            Serial.println("🛑 [ATUADOR] Peltier: BLOQUEADO - Em período de cooldown");
            return;
        }
        
        if (cooling) {
            // Modo resfriamento: Relé1 HIGH, Relé2 LOW
            if (!relay1State || relay2State || currentPeltierMode != COOLING) {
                digitalWrite(_pinRelay1, HIGH);
                digitalWrite(_pinRelay2, LOW);
                currentPeltierMode = COOLING;
                relay1State = true;
                relay2State = false;
                stateChanged = true;
                Serial.println("❄️ [ATUADOR] Peltier: Modo resfriamento ATIVADO");
            }
        } else {
            // Modo aquecimento: Ambos relés HIGH
            if (!relay1State || !relay2State || currentPeltierMode != HEATING) {
                digitalWrite(_pinRelay1, HIGH);
                digitalWrite(_pinRelay2, HIGH);
                currentPeltierMode = HEATING;
                relay1State = true;
                relay2State = true;
                stateChanged = true;
                Serial.println("🔥 [ATUADOR] Peltier: Modo aquecimento ATIVADO");
            }
        }
        peltierActive = true;
        lastPeltierTime = millis();
    } else {
        // Desliga ambos os relés do Peltier
        if (relay1State || relay2State) {
            digitalWrite(_pinRelay1, LOW);
            digitalWrite(_pinRelay2, LOW);
            peltierActive = false;
            currentPeltierMode = OFF;
            relay1State = false;
            relay2State = false;
            stateChanged = true;
            Serial.println("⭕ [ATUADOR] Peltier: DESLIGADO");
        }
    }
    
    // Atualiza Firebase apenas se houve mudança e pode escrever
    if (stateChanged && firebaseHandler != nullptr && firebaseHandler->isAuthenticated() && 
        firebaseHandler->isFirebaseReady() && canWriteToFirebase()) {
        updateFirebaseStateImmediately();
    }
}

/**
 * @brief Controla os LEDs com efeito de fade
 * @param on Liga/desliga os LEDs
 * @param intensity Intensidade dos LEDs (0-255)
 */
void ActuatorController::controlLEDs(bool on, int intensity) {
    bool stateChanged = false;
    int oldIntensity = currentLEDIntensity;
    
    if (on) {
        // Efeito fade in
        for (int i = currentLEDIntensity; i <= intensity; i += 5) {
            analogWrite(_pinLED, i);
            delay(10);
        }
        currentLEDIntensity = intensity;
        if (oldIntensity != intensity) {
            stateChanged = true;
            Serial.printf("💡 [ATUADOR] LEDs: LIGADO, Intensidade: %d/255\n", intensity);
        }
    } else {
        // Efeito fade out
        for (int i = currentLEDIntensity; i >= 0; i -= 5) {
            analogWrite(_pinLED, i);
            delay(10);
        }
        if (currentLEDIntensity > 0) {
            stateChanged = true;
            Serial.println("💡 [ATUADOR] LEDs: DESLIGADO");
        }
        currentLEDIntensity = 0;
    }
    
    // Atualiza Firebase apenas se houve mudança e pode escrever
    if (stateChanged && firebaseHandler != nullptr && firebaseHandler->isAuthenticated() && 
        firebaseHandler->isFirebaseReady() && canWriteToFirebase()) {
        updateFirebaseStateImmediately();
    }
}

/**
 * @brief Controla um relé individual
 * @param relayNumber Número do relé (1-4)
 * @param state Estado desejado (true = ligado, false = desligado)
 */
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
                humidifierOn = state;  // Sincroniza estado do umidificador
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
        Serial.printf("🔌 [ATUADOR] Rele %d: %s\n", relayNumber, state ? "LIGADO" : "DESLIGADO");
        
        // Atualiza Firebase apenas se pode escrever
        if (firebaseHandler != nullptr && firebaseHandler->isAuthenticated() && 
            firebaseHandler->isFirebaseReady() && canWriteToFirebase()) {
            updateFirebaseStateImmediately();
        }
    }
}

// =============================================================================
// COMUNICAÇÃO COM FIREBASE
// =============================================================================

/**
 * @brief Atualiza o estado dos atuadores no Firebase (periódico)
 */
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
        Serial.println("🔥 [FIREBASE] Estado dos atuadores atualizado (periódico)");
    }
}

/**
 * @brief Atualiza o estado dos atuadores no Firebase (imediato)
 */
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
        Serial.println("🔥 [FIREBASE] Estado dos atuadores atualizado (imediato)");
    }
}

// =============================================================================
// MÉTODOS DE COMPATIBILIDADE E CONSULTA
// =============================================================================

/**
 * @brief Controla o Peltier em modo aquecimento (compatibilidade)
 * @param on Ligar/desligar
 * @return Sempre retorna true
 */
bool ActuatorController::heatPeltier(bool on) {
    // Mantida para compatibilidade
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
        default: return -1; // Estado inválido
    }
}

// =============================================================================
// MÉTODOS PARA MODO DEBUG E CONTROLE MANUAL
// =============================================================================

/**
 * @brief Ativa/desativa o modo de debug
 * @param debug true para ativar modo debug
 * 
 * @note No modo debug, o controle automático é desativado
 */
void ActuatorController::setDebugMode(bool debug) {
    if (debug != debugMode) {
        debugMode = debug;
        Serial.println(debugMode ? "🔧 DEBUG MODE: ON" : "🔧 DEBUG MODE: OFF");
        
        // Ao entrar no modo debug, bloqueia escrita por um tempo
        if (debugMode) {
            setFirebaseWriteBlock(true);
        }
        
        // Atualiza Firebase quando o modo debug muda
        if (firebaseHandler != nullptr && firebaseHandler->isAuthenticated() && 
            firebaseHandler->isFirebaseReady() && canWriteToFirebase()) {
            updateFirebaseStateImmediately();
        }
    }
}

/**
 * @brief Aplica estados manuais aos atuadores (apenas em modo debug)
 * @param relay1 Estado do relé 1
 * @param relay2 Estado do relé 2
 * @param relay3 Estado do relé 3
 * @param relay4 Estado do relé 4
 * @param ledsOn Estado dos LEDs
 * @param ledsIntensity Intensidade dos LEDs
 * @param humidifierOn Estado do umidificador
 */
void ActuatorController::setManualStates(bool relay1, bool relay2, bool relay3, bool relay4, bool ledsOn, int ledsIntensity, bool humidifierOn) {
    if (!debugMode) return;

    // Bloqueia escrita no Firebase durante mudanças manuais
    setFirebaseWriteBlock(true);

    // Variável para detectar mudanças reais
    bool anyChange = false;

    // Aplica os estados manuais apenas se diferentes dos atuais
    if (relay1 != relay1State) {
        digitalWrite(_pinRelay1, relay1 ? HIGH : LOW);
        relay1State = relay1;
        anyChange = true;
        Serial.printf("🔧 [MANUAL] Relay 1: %s\n", relay1 ? "ON" : "OFF");
    }
    if (relay2 != relay2State) {
        digitalWrite(_pinRelay2, relay2 ? HIGH : LOW);
        relay2State = relay2;
        anyChange = true;
        Serial.printf("🔧 [MANUAL] Relay 2: %s\n", relay2 ? "ON" : "OFF");
    }
    if (relay3 != relay3State) {
        digitalWrite(_pinRelay3, relay3 ? HIGH : LOW);
        relay3State = relay3;
        this->humidifierOn = relay3;
        anyChange = true;
        Serial.printf("🔧 [MANUAL] Relay 3 (Humidifier): %s\n", relay3 ? "ON" : "OFF");
    }
    if (relay4 != relay4State) {
        digitalWrite(_pinRelay4, relay4 ? HIGH : LOW);
        relay4State = relay4;
        anyChange = true;
        Serial.printf("🔧 [MANUAL] Relay 4: %s\n", relay4 ? "ON" : "OFF");
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
        Serial.printf("🔧 [MANUAL] LEDs: %s, Intensity: %d/255\n", ledsOn ? "ON" : "OFF", ledsIntensity);
    }
    
    if (anyChange) {
        Serial.println("✅ Manual changes applied successfully");
    }
    
    // Não atualiza Firebase imediatamente após mudanças manuais
    // O Firebase será atualizado apenas quando o bloqueio expirar (10 segundos)
}

// =============================================================================
// MÉTODOS PARA MODO DE DESENVOLVIMENTO (DEVMODE)
// =============================================================================

/**
 * @brief Configura as opções do modo de desenvolvimento
 * @param analogRead Habilita leitura analógica
 * @param digitalWrite Habilita escrita digital
 * @param pin Pino para operações
 * @param pwm Habilita PWM
 * @param pwmValue Valor PWM
 */
void ActuatorController::setDevModeSettings(bool analogRead, bool digitalWrite, int pin, bool pwm, int pwmValue) {
    devModeAnalogRead = analogRead;
    devModeDigitalWrite = digitalWrite;
    devModePWM = pwm;
    devModePin = pin;
    devModePWMValue = pwmValue;
    
    Serial.printf("🔬 [DEVMODE] Settings updated - AnalogRead: %d, DigitalWrite: %d, Pin: %d, PWM: %d, PWMValue: %d\n",
                 devModeAnalogRead, devModeDigitalWrite, devModePin, devModePWM, devModePWMValue);
}

/**
 * @brief Executa as operações configuradas no modo de desenvolvimento
 */
void ActuatorController::executeDevModeOperations() {
    // Verifica se o pino é válido
    if (devModePin < 0 || devModePin > 39) {
        Serial.println("❌ [DEVMODE] ERRO - Pino inválido: " + String(devModePin));
        return;
    }
    
    // Operação de leitura analógica
    if (devModeAnalogRead) {
        int analogValue = analogRead(devModePin);
        Serial.printf("🔬 [DEVMODE] Analog Read - Pin %d: %d\n", devModePin, analogValue);
    }
    
    // Operação de escrita digital
    if (devModeDigitalWrite) {
        digitalWrite(devModePin, devModePWMValue > 0 ? HIGH : LOW);
        Serial.printf("🔬 [DEVMODE] Digital Write - Pin %d: %s\n", 
                     devModePin, devModePWMValue > 0 ? "HIGH" : "LOW");
    }
    
    // Operação PWM
    if (devModePWM && !devModeDigitalWrite) {
        if (devModePWMValue < 0 || devModePWMValue > 255) {
            Serial.println("❌ [DEVMODE] ERRO - Valor PWM inválido: " + String(devModePWMValue));
            return;
        }
        analogWrite(devModePin, devModePWMValue);
        Serial.printf("🔬 [DEVMODE] PWM Write - Pin %d: %d/255\n", devModePin, devModePWMValue);
    }
}

/**
 * @brief Gerencia o modo de desenvolvimento
 * 
 * @note Configura os pinos e executa operações quando o modo está ativo
 */
void ActuatorController::handleDevMode() {
    if (!debugMode) {
        if (lastDevModeState) {
            // Saiu do modo dev - reseta os pinos para estado seguro
            if (devModePin >= 0) {
                pinMode(devModePin, INPUT); // Coloca o pino em estado seguro
                Serial.printf("🔬 [DEVMODE] Pino %d resetado para INPUT\n", devModePin);
            }
            lastDevModeState = false;
        }
        return;
    }
    
    // Entrou no modo dev
    if (!lastDevModeState) {
        Serial.println("🔬 [DEVMODE] 🔧 Modo desenvolvimento ATIVADO");
        lastDevModeState = true;
        
        // Configura o pino se for válido
        if (devModePin >= 0 && devModePin <= 39) {
            if (devModeDigitalWrite || devModePWM) {
                pinMode(devModePin, OUTPUT);
                Serial.printf("🔬 [DEVMODE] Pino %d configurado como OUTPUT\n", devModePin);
            } else if (devModeAnalogRead) {
                pinMode(devModePin, INPUT);
                Serial.printf("🔬 [DEVMODE] Pino %d configurado como INPUT\n", devModePin);
            }
        }
    }
    
    // Executa as operações do devmode
    executeDevModeOperations();
}

// =============================================================================
// CONTROLE DO AGENDADOR DE LEDs
// =============================================================================

/**
 * @brief Atualiza o LEDScheduler e aplica sua decisão nos LEDs
 *
 * @param ts Timestamp Unix atual (de getCurrentTimestamp())
 *
 * @details Deve ser chamado periodicamente no loop() principal.
 *          Se o scheduler não estiver ativo, não faz nada.
 *          Se o modo debug estiver ativo, o scheduler fica inativo internamente.
 */
void ActuatorController::applyLEDSchedule(unsigned long ts) {
    ledScheduler.update(ts, debugMode);
    // A decisão é aplicada dentro de controlAutomatically() via ledScheduler.isActive()
    // Aqui forçamos a aplicação imediata caso controlAutomatically não tenha rodado ainda
    if (ledScheduler.isActive() && !debugMode) {
        int schedIntensity = ledScheduler.wantsLEDsOn() ? ledScheduler.getIntensity() : 0;
        if (schedIntensity != currentLEDIntensity) {
            controlLEDs(schedIntensity > 0, schedIntensity);
        }
    }
}
