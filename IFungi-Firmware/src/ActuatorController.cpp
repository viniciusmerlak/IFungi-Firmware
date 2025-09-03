// ActuatorController.cpp
#include "ActuatorController.h"
#include <Preferences.h>

void ActuatorController::salvarSetpointsNVS() {
    Preferences preferences;
    if(!preferences.begin("setpoints", false)) {
        Serial.println("Erro ao abrir NVS para salvar setpoints!");
        return;
    }
    
    preferences.putInt("lux", luxSetpoint);
    preferences.putFloat("tMin", tempMin);
    preferences.putFloat("tMax", tempMax);
    preferences.putFloat("uMin", umidMin);
    preferences.putFloat("uMax", umidMax);
    preferences.putInt("coSp", coSetpoint);
    preferences.putInt("co2Sp", co2Setpoint);
    preferences.putInt("tvocsSp", tvocsSetpoint);
    
    preferences.end();
    Serial.println("Setpoints salvos no NVS");
}

bool ActuatorController::carregarSetpointsNVS() {
    Preferences preferences;
    if(!preferences.begin("setpoints", true)) {
        Serial.println("NVS de setpoints não encontrado, usando padrões");
        return false;
    }
    
    // Carrega os valores, se não existir, retorna false
    if(preferences.isKey("lux")) {
        luxSetpoint = preferences.getInt("lux", 100);
        tempMin = preferences.getFloat("tMin", 20.0);
        tempMax = preferences.getFloat("tMax", 30.0);
        umidMin = preferences.getFloat("uMin", 60.0);
        umidMax = preferences.getFloat("uMax", 80.0);
        coSetpoint = preferences.getInt("coSp", 400);
        co2Setpoint = preferences.getInt("co2Sp", 400);
        tvocsSetpoint = preferences.getInt("tvocsSp", 100);
        
        preferences.end();
        Serial.println("Setpoints carregados do NVS");
        return true;
    } else {
        preferences.end();
        Serial.println("Nenhum setpoint salvo no NVS, usando padrões");
        return false;
    }
}
// Definições de histerese para evitar oscilações
const float HYSTERESIS_TEMP = 0.5f;
const float HYSTERESIS_UMID = 2.0f;

void ActuatorController::begin(uint8_t pinLED, uint8_t pinRele1, uint8_t pinRele2, 
                             uint8_t pinRele3, uint8_t pinRele4) {
    Serial.println("Inicializando ActuatorController...");
    
    // Configurar pinos
    _pinLED = pinLED;
    _pinRele1 = pinRele1;
    _pinRele2 = pinRele2;
    _pinRele3 = pinRele3;
    _pinRele4 = pinRele4;

    pinMode(_pinLED, OUTPUT);
    pinMode(_pinRele1, OUTPUT);
    pinMode(_pinRele2, OUTPUT);
    pinMode(_pinRele3, OUTPUT);
    pinMode(_pinRele4, OUTPUT);
    
    // Inicializar todos desligados
    digitalWrite(_pinLED, LOW);
    digitalWrite(_pinRele1, LOW);
    digitalWrite(_pinRele2, LOW);
    digitalWrite(_pinRele3, LOW);
    digitalWrite(_pinRele4, LOW);
    
    // Inicializar estados
    umidLigado = false;
    peltierHeating = false;
    lastPeltierTime = 0;
    lastUpdateTime = 0;
    emCooldown = false;
    inicioCooldown = 0;
    
    Serial.println("ActuatorController inicializado com sucesso");
}

void ActuatorController::setFirebaseHandler(FirebaseHandler* handler) {
    firebaseHandler = handler;
    Serial.println("FirebaseHandler definido para ActuatorController");
}

void ActuatorController::aplicarSetpoints(int lux, float tMin, float tMax, float uMin, float uMax, int coSp, int co2Sp, int tvocsSp) {
    luxSetpoint = lux;
    tempMin = tMin;
    tempMax = tMax;
    umidMin = uMin;
    umidMax = uMax;
    coSetpoint = coSp;
    co2Setpoint = co2Sp;
    tvocsSetpoint = tvocsSp;
    
    // Salva os setpoints no NVS
    salvarSetpointsNVS();
    
    Serial.printf("Setpoints aplicados: Lux=%d, Temp=[%.1f-%.1f], Umid=[%.1f-%.1f], CO=%d, CO2=%d, TVOCs=%d\n", 
                 lux, tMin, tMax, uMin, uMax, coSp, co2Sp, tvocsSp);
}

void ActuatorController::controlarAutomaticamente(float temp, float umid, int luz, int co, int co2, int tvocs, bool waterLevel) {
    // Verificar segurança da Peltier (apenas para aquecimento)
    if (peltierHeating && modoPeltierAtual == AQUECENDO && 
        (millis() - lastPeltierTime >= tempoOperacao)) {
        Serial.println("[SEGURANÇA] Tempo de operação da Peltier excedido, iniciando cooldown");
        controlarPeltier(false, false);
        emCooldown = true;
        inicioCooldown = millis();
    }
    
    // Verificar se o cooldown terminou (apenas para aquecimento)
    if (emCooldown && (millis() - inicioCooldown >= tempoCooldown)) {
        Serial.println("[SEGURANÇA] Cooldown finalizado, Peltier disponível");
        emCooldown = false;
    }

    // 1. Controle de temperatura - Peltier (Rele 1 e 2) com histerese
    if (temp < (tempMin - HYSTERESIS_TEMP)) {
        // Temperatura abaixo do mínimo - Aquecer
        if ((!peltierHeating || modoPeltierAtual != AQUECENDO) && !emCooldown) {
            Serial.printf("[ATUADOR] Temperatura abaixo (%.1f < %.1f), aquecendo\n", temp, tempMin);
            controlarPeltier(false, true); // Aquecer (resfriar = false)
        }
    } 
    else if (temp > (tempMax + HYSTERESIS_TEMP)) {
        // Temperatura acima do máximo - Resfriar
        if (!peltierHeating || modoPeltierAtual != RESFRIANDO) {
            Serial.printf("[ATUADOR] Temperatura acima (%.1f > %.1f), resfriando\n", temp, tempMax);
            controlarPeltier(true, true); // Resfriar (resfriar = true)
        }
    }
    else if (peltierHeating) {
        // Temperatura dentro da faixa - Desligar Peltier
        Serial.println("[ATUADOR] Temperatura OK, desligando Peltier");
        controlarPeltier(false, false); // Desligar
    }
    
    // 2. Controle de umidade - Umidificador (Rele 3) com histerese
    if (!waterLevel) {
        Serial.println("[SEGURANÇA] Nível de água baixo, não operar umidificador");
        if (umidLigado) {
            controlarRele(3, true); // Desliga o umidificador se estiver ligado
            controlarRele(3, false); // Liga por pulso curto
            umidLigado = false;
        }
        // Não tenta ligar o umidificador se o nível de água estiver baixo
    } 
    else
    if (umid < (umidMin - HYSTERESIS_UMID) && !umidLigado && !waterLevel) {
        Serial.printf("[ATUADOR] Umidade abaixo (%.1f < %.1f), ligando umidificador\n", umid, umidMin);
        
        controlarRele(3, true);
        controlarRele(3, false); // Liga por pulso curto
        umidLigado = true;
    } 
    else if (umid > (umidMax + HYSTERESIS_UMID) && umidLigado && !waterLevel) {
        Serial.printf("[ATUADOR] Umidade acima (%.1f > %.1f), desligando umidificador\n", umid, umidMax);
        controlarRele(3, true);
        controlarRele(3, false); // Liga por pulso curto
        umidLigado = false;
    }
    
    // 3. Controle de luminosidade - LEDs
    int novaIntensidade = (luz < luxSetpoint) ? 255 : 0;
    
    if (novaIntensidade != intensidadeLEDAtual) {
        controlarLEDs(novaIntensidade > 0, novaIntensidade);
    }
    
    // 4. Atualizar estado no Firebase periodicamente
    if (firebaseHandler != nullptr && millis() - lastUpdateTime > 5000) {
        atualizarEstadoFirebase();
        lastUpdateTime = millis();
    }
    // 5. Controle dos gases co, co2 e tvocs
    if (co > coSetpoint || co2 > co2Setpoint || tvocs > tvocsSetpoint) {
        Serial.printf("[ATUADOR] Gases acima do limite (CO: %d, CO2: %d, TVOCs: %d), ligando exaustor\n", 
                     co, co2, tvocs);
        controlarRele(4, true); // Liga exaustor
    } else {
        controlarRele(4, false); // Desliga exaustor
    }
}

void ActuatorController::controlarPeltier(bool resfriar, bool ligar) {
    if (ligar) {
        // Verificar cooldown apenas para aquecimento
        if (emCooldown && !resfriar) {
            Serial.println("[SEGURANÇA] Peltier em cooldown, não pode ligar para aquecimento");
            return;
        }
        
        if (resfriar) {
            // Modo resfriamento: Rele1 HIGH, Rele2 LOW (sem timeout)
            digitalWrite(_pinRele1, HIGH);
            digitalWrite(_pinRele2, LOW);
            modoPeltierAtual = RESFRIANDO;
            Serial.println("[PELTIER] Modo resfriamento (sem timeout)");
        } else {
            // Modo aquecimento: Rele1 HIGH, Rele2 HIGH (com timeout)
            digitalWrite(_pinRele1, HIGH);
            digitalWrite(_pinRele2, HIGH);
            modoPeltierAtual = AQUECENDO;
            Serial.println("[PELTIER] Modo aquecimento (com timeout)");
        }
        peltierHeating = true;
        lastPeltierTime = millis();
    } else {
        // Desligar ambos os relés
        digitalWrite(_pinRele1, LOW);
        digitalWrite(_pinRele2, LOW);
        peltierHeating = false;
        modoPeltierAtual = DESLIGADO;
        Serial.println("[PELTIER] Desligado");
    }
    
    // Atualizar estado no Firebase
    if (firebaseHandler != nullptr) {
        atualizarEstadoFirebase();
    }
}

void ActuatorController::controlarLEDs(bool ligado, int intensidade) {
    if (ligado) {
        // Transição suave para ligar
        for (int i = intensidadeLEDAtual; i <= intensidade; i += 5) {
            analogWrite(_pinLED, i);
            delay(10);
        }
        intensidadeLEDAtual = intensidade;
        Serial.printf("[LEDS] Ligado, Intensidade: %d/255\n", intensidade);
    } else {
        // Transição suave para desligar
        for (int i = intensidadeLEDAtual; i >= 0; i -= 5) {
            analogWrite(_pinLED, i);
            delay(10);
        }
        intensidadeLEDAtual = 0;
        Serial.println("[LEDS] Desligado");
    }
    
    // Atualizar estado no Firebase
    if (firebaseHandler != nullptr) {
        atualizarEstadoFirebase();
    }
}

void ActuatorController::controlarRele(uint8_t num, bool estado) {
    switch(num) {
        case 1: 
            digitalWrite(_pinRele1, estado ? HIGH : LOW);
            rele1Estado = estado;
            break;
        case 2: 
            digitalWrite(_pinRele2, estado ? HIGH : LOW);
            rele2Estado = estado;
            break;
        case 3: 
            digitalWrite(_pinRele3, estado ? HIGH : LOW);
            rele3Estado = estado;
            umidLigado = estado; // Atualiza estado do umidificador
            break;
        case 4: 
            digitalWrite(_pinRele4, estado ? HIGH : LOW);
            rele4Estado = estado;
            break;
    }
    
    Serial.printf("[RELE %d] %s\n", num, estado ? "Ligado" : "Desligado");
    
    // Atualizar estado no Firebase
    if (firebaseHandler != nullptr) {
        atualizarEstadoFirebase();
    }
}

void ActuatorController::atualizarEstadoFirebase() {
    if (firebaseHandler != nullptr) {
        firebaseHandler->atualizarEstadoAtuadores(
            rele1Estado,
            rele2Estado, 
            rele3Estado, 
            rele4Estado, 
            (intensidadeLEDAtual > 0), 
            intensidadeLEDAtual,
            umidLigado
        );
    }
}

bool ActuatorController::AquecerPastilha(bool ligar) {
    // Função mantida para compatibilidade
    controlarPeltier(false, ligar);
    return true;
}