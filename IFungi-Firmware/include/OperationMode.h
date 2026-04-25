#ifndef OPERATION_MODE_H
#define OPERATION_MODE_H

#include <Arduino.h>

/**
 * @file OperationMode.h
 * @brief Modos de operação pré-configurados para cultivo de fungos
 * @version 1.0
 * @date 2026
 *
 * @details Define os modos de operação da estufa para cada fase do ciclo
 * de cultivo de fungos. Cada modo aplica um conjunto de setpoints e
 * comportamentos específicos nos atuadores.
 *
 * MODOS DISPONÍVEIS:
 * ─────────────────────────────────────────────────────────────────────
 *
 *  MANUAL
 *    Sem presets automáticos. Os setpoints são controlados pelo app.
 *    Todos os atuadores respondem normalmente à lógica automática.
 *
 *  INCUBACAO (Incubação / Colonização)
 *    Fase em que o substrato inoculado está em sacos plásticos selados.
 *    O micélio coloniza em ambiente escuro e seco.
 *    - Umidificador DESLIGADO (sacos fechados não precisam de umidade externa)
 *    - LEDs DESLIGADOS (fungos não precisam de luz nesta fase)
 *    - Temperatura controlada na faixa ideal para colonização
 *    - Exaustor ativo apenas por CO2 excessivo
 *
 *  FRUTIFICACAO (Frutificação / Pinning)
 *    Fase de formação dos primórdios e crescimento dos cogumelos.
 *    Requer alta umidade, ventilação e ciclo de luz.
 *    - Umidificador ATIVO (umidade alta, 85-95%)
 *    - LEDs com ciclo de 12h (simulação solar ou timer fixo)
 *    - Temperatura ligeiramente mais baixa para induzir frutificação
 *    - Ventilação frequente (CO2 baixo estimula primórdios)
 *
 *  SECAGEM (Pós-colheita / Preparação para secagem)
 *    Fase após a colheita. Reduz umidade para inibir contaminação
 *    enquanto prepara o substrato para novo ciclo ou descarte.
 *    - Umidificador DESLIGADO
 *    - Exaustor em ciclo contínuo para renovação de ar
 *    - LEDs DESLIGADOS
 *    - Temperatura ambiente (sem controle ativo)
 *
 *  MANUTENCAO (Manutenção / Limpeza)
 *    Estufa sem cultivo ativo. Mantém condições de higiene.
 *    - Todos os atuadores DESLIGADOS
 *    - Setpoints em valores neutros (sem acionamentos)
 *    - Ideal para períodos de limpeza e esterilização
 *
 * ─────────────────────────────────────────────────────────────────────
 *
 * ESTRUTURA NO FIREBASE RTDB:
 *  /greenhouses/<ID>/operation_mode: {
 *    "mode": "frutificacao",          // string do enum
 *    "lastChanged": 1720000000,       // timestamp da última mudança
 *    "changedBy": "app"               // quem mudou (app, esp32, auto)
 *  }
 */

// =============================================================================
// ENUM DE MODOS DE OPERAÇÃO
// =============================================================================

enum OperationMode {
    MODE_MANUAL       = 0,  ///< Controle manual via setpoints do app
    MODE_INCUBACAO    = 1,  ///< Colonização: escuro, seco, temp controlada
    MODE_FRUTIFICACAO = 2,  ///< Frutificação: úmido, ciclo de luz, temp baixa
    MODE_SECAGEM      = 3,  ///< Pós-colheita: seco, ventilado, sem luz
    MODE_MANUTENCAO   = 4   ///< Manutenção: tudo desligado
};

// =============================================================================
// CONVERSÃO ENUM ↔ STRING
// =============================================================================

/**
 * @brief Converte string do Firebase para enum OperationMode
 * @param str String do Firebase (ex: "frutificacao")
 * @return OperationMode correspondente (padrão: MODE_MANUAL)
 */
inline OperationMode operationModeFromString(const String& str) {
    if (str == "incubacao")    return MODE_INCUBACAO;
    if (str == "frutificacao") return MODE_FRUTIFICACAO;
    if (str == "secagem")      return MODE_SECAGEM;
    if (str == "manutencao")   return MODE_MANUTENCAO;
    return MODE_MANUAL;
}

/**
 * @brief Converte enum OperationMode para string do Firebase
 * @param mode Enum OperationMode
 * @return String correspondente (ex: "frutificacao")
 */
inline String operationModeToString(OperationMode mode) {
    switch (mode) {
        case MODE_INCUBACAO:    return "incubacao";
        case MODE_FRUTIFICACAO: return "frutificacao";
        case MODE_SECAGEM:      return "secagem";
        case MODE_MANUTENCAO:   return "manutencao";
        default:                return "manual";
    }
}

/**
 * @brief Retorna nome legível do modo para logs
 */
inline String operationModeLabel(OperationMode mode) {
    switch (mode) {
        case MODE_INCUBACAO:    return "Incubação";
        case MODE_FRUTIFICACAO: return "Frutificação";
        case MODE_SECAGEM:      return "Secagem";
        case MODE_MANUTENCAO:   return "Manutenção";
        default:                return "Manual";
    }
}

// =============================================================================
// ESTRUTURA DE PRESET
// =============================================================================

/**
 * @struct ModePreset
 * @brief Conjunto de setpoints e comportamentos para um modo de operação
 */
struct ModePreset {
    // Setpoints ambientais
    float tempMin;          ///< Temperatura mínima (°C)
    float tempMax;          ///< Temperatura máxima (°C)
    float humidityMin;      ///< Umidade mínima (%)
    float humidityMax;      ///< Umidade máxima (%)
    int   co2Setpoint;      ///< Setpoint de CO2 (ppm) — acima disso liga exaustor
    int   luxSetpoint;      ///< Setpoint de luz (lux) — usado no modo automático

    // Comportamento dos atuadores
    bool  humidifierEnabled;    ///< false = umidificador SEMPRE desligado neste modo
    bool  ledsEnabled;          ///< false = LEDs SEMPRE desligados neste modo
    bool  peltierEnabled;       ///< false = Peltier SEMPRE desligado neste modo
    bool  exhaustForcedOn;      ///< true  = exaustor SEMPRE ligado (independente de CO2)

    // Agendamento de LEDs (aplicado automaticamente quando ledsEnabled=true)
    bool  schedulerActive;      ///< true = ativa o scheduler ao entrar no modo
    bool  solarSim;             ///< true = simulação solar, false = timer fixo
    int   ledOnHour;            ///< Hora de ligar os LEDs (0-23)
    int   ledOnMinute;          ///< Minuto de ligar os LEDs (0-59)
    int   ledOffHour;           ///< Hora de desligar os LEDs (0-23)
    int   ledOffMinute;         ///< Minuto de desligar os LEDs (0-59)
    int   ledIntensity;         ///< Intensidade fixa dos LEDs (0-255)
};

// =============================================================================
// PRESETS POR MODO
// =============================================================================

/**
 * @brief Retorna o preset de configuração para um modo de operação
 *
 * @details Valores baseados em literatura de cultivo de fungos comestíveis
 * (Pleurotus ostreatus, Lentinula edodes, Ganoderma lucidum).
 * Ajuste conforme a espécie cultivada.
 *
 * @param mode Modo de operação desejado
 * @return ModePreset com todos os parâmetros configurados
 */
inline ModePreset getModePreset(OperationMode mode) {
    switch (mode) {

        // ─── INCUBAÇÃO ────────────────────────────────────────────────────────
        // Substrato selado em sacos. Micélio coloniza em escuro e seco.
        // Temp ideal Pleurotus: 22-28°C. Sem luz. Sem umidade externa.
        case MODE_INCUBACAO:
            return {
                /* tempMin */          22.0f,
                /* tempMax */          28.0f,
                /* humidityMin */      50.0f,  // irrelevante (umidif. desligado)
                /* humidityMax */      70.0f,  // irrelevante
                /* co2Setpoint */      2000,   // tolerância maior (sacos fechados produzem CO2)
                /* luxSetpoint */      0,      // sem setpoint de luz
                /* humidifierEnabled*/ false,  // ← DESLIGADO: sacos são selados
                /* ledsEnabled */      false,  // ← DESLIGADO: fase escura
                /* peltierEnabled */   true,   // ← ATIVO: controla temperatura
                /* exhaustForcedOn */  false,  // exaustor apenas por CO2
                /* schedulerActive */  false,
                /* solarSim */         false,
                /* ledOnHour */        0,
                /* ledOnMinute */      0,
                /* ledOffHour */       0,
                /* ledOffMinute */     0,
                /* ledIntensity */     0
            };

        // ─── FRUTIFICAÇÃO ─────────────────────────────────────────────────────
        // Sacos abertos ou blocos expostos. Alta umidade, ciclo de luz 12h.
        // Temp ideal Pleurotus frutificação: 18-24°C.
        case MODE_FRUTIFICACAO:
            return {
                /* tempMin */          18.0f,
                /* tempMax */          24.0f,
                /* humidityMin */      85.0f,  // ← alta umidade essencial
                /* humidityMax */      95.0f,
                /* co2Setpoint */      800,    // ← baixo CO2 estimula primórdios
                /* luxSetpoint */      3000,
                /* humidifierEnabled*/ true,   // ← ATIVO: umidade alta
                /* ledsEnabled */      true,   // ← ATIVO: ciclo de luz
                /* peltierEnabled */   true,
                /* exhaustForcedOn */  false,
                /* schedulerActive */  true,   // ← ativa scheduler automaticamente
                /* solarSim */         false,  // timer fixo (12h)
                /* ledOnHour */        6,
                /* ledOnMinute */      0,
                /* ledOffHour */       18,
                /* ledOffMinute */     0,
                /* ledIntensity */     180     // ~70% de intensidade
            };

        // ─── SECAGEM ─────────────────────────────────────────────────────────
        // Pós-colheita. Reduz umidade. Exaustor contínuo para secar o ambiente.
        case MODE_SECAGEM:
            return {
                /* tempMin */          20.0f,
                /* tempMax */          35.0f,  // faixa larga — sem controle ativo
                /* humidityMin */      30.0f,
                /* humidityMax */      50.0f,  // umidade baixa desejada
                /* co2Setpoint */      1000,
                /* luxSetpoint */      0,
                /* humidifierEnabled*/ false,  // ← DESLIGADO
                /* ledsEnabled */      false,  // ← DESLIGADO
                /* peltierEnabled */   false,  // ← DESLIGADO: economia de energia
                /* exhaustForcedOn */  true,   // ← exaustor SEMPRE ligado para secar
                /* schedulerActive */  false,
                /* solarSim */         false,
                /* ledOnHour */        0,
                /* ledOnMinute */      0,
                /* ledOffHour */       0,
                /* ledOffMinute */     0,
                /* ledIntensity */     0
            };

        // ─── MANUTENÇÃO ──────────────────────────────────────────────────────
        // Sem cultivo. Tudo desligado. Ideal para limpeza/esterilização.
        case MODE_MANUTENCAO:
            return {
                /* tempMin */          10.0f,  // faixa muito larga = Peltier nunca aciona
                /* tempMax */          40.0f,
                /* humidityMin */      0.0f,
                /* humidityMax */      100.0f,
                /* co2Setpoint */      5000,   // exaustor nunca aciona
                /* luxSetpoint */      0,
                /* humidifierEnabled*/ false,
                /* ledsEnabled */      false,
                /* peltierEnabled */   false,
                /* exhaustForcedOn */  false,
                /* schedulerActive */  false,
                /* solarSim */         false,
                /* ledOnHour */        0,
                /* ledOnMinute */      0,
                /* ledOffHour */       0,
                /* ledOffMinute */     0,
                /* ledIntensity */     0
            };

        // ─── MANUAL (padrão) ─────────────────────────────────────────────────
        // Sem restrições. Tudo controlado pelos setpoints do app.
        default: // MODE_MANUAL
            return {
                /* tempMin */          20.0f,
                /* tempMax */          30.0f,
                /* humidityMin */      60.0f,
                /* humidityMax */      80.0f,
                /* co2Setpoint */      1000,
                /* luxSetpoint */      5000,
                /* humidifierEnabled*/ true,
                /* ledsEnabled */      true,
                /* peltierEnabled */   true,
                /* exhaustForcedOn */  false,
                /* schedulerActive */  false,
                /* solarSim */         false,
                /* ledOnHour */        6,
                /* ledOnMinute */      0,
                /* ledOffHour */       20,
                /* ledOffMinute */     0,
                /* ledIntensity */     255
            };
    }
}

#endif // OPERATION_MODE_H
