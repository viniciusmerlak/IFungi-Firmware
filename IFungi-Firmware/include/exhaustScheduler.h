#ifndef EXHAUST_SCHEDULER_H
#define EXHAUST_SCHEDULER_H

#include <Arduino.h>

/**
 * @file exhaustScheduler.h
 * @brief Sistema de agendamento por horário do exaustor (ventilação)
 * @version 1.0
 * @date 2026
 *
 * @details Gerencia o modo de operação por horário fixo do exaustor
 * (relé 4 + servo), espelhando o design do LEDScheduler.
 *
 *  MODO TIMER (scheduleEnabled = true):
 *  ─────────────────────────────────────
 *  Liga/desliga o exaustor em horários fixos configurados no Firebase.
 *  Exemplo: ligar às 06:00, desligar às 20:00.
 *
 *  PRIORIDADE (ver ActuatorController::controlAutomatically):
 *  ─────────────────────────────────────
 *  A segurança de gases (CO/CO2/TVOCs acima do limite) SEMPRE tem
 *  prioridade sobre este scheduler — gases altos podem forçar o
 *  exaustor mesmo com o scheduler configurado para "desligado".
 *
 *  ESTRUTURA NO FIREBASE RTDB:
 *  ─────────────────────────────────────
 *  /greenhouses/<ID>/exhaust_schedule: {
 *    "scheduleEnabled": true,
 *    "onHour":   6,       // hora de ligar (0-23)
 *    "onMinute": 0,       // minuto de ligar (0-59)
 *    "offHour":  20,      // hora de desligar (0-23)
 *    "offMinute": 0       // minuto de desligar (0-59)
 *  }
 */
class ExhaustScheduler {
public:

    // ─── Configurações lidas do Firebase ──────────────────────────────────────

    bool scheduleEnabled = false; ///< Timer simples ligado/desligado
    int  onHour           = 6;    ///< Hora de início (0-23)
    int  onMinute         = 0;    ///< Minuto de início (0-59)
    int  offHour          = 20;   ///< Hora de fim (0-23)
    int  offMinute        = 0;    ///< Minuto de fim (0-59)

    // ─── Interface pública ────────────────────────────────────────────────────

    /**
     * @brief Atualiza a lógica do scheduler com base no timestamp atual
     *
     * @param currentTimestamp Timestamp Unix atual (de getCurrentTimestamp())
     * @param debugMode        Se true, scheduler fica inativo
     *
     * @details Deve ser chamado periodicamente no loop (mesmo ritmo do LEDScheduler).
     */
    void update(unsigned long currentTimestamp, bool debugMode);

    /**
     * @brief Retorna se o scheduler está ativo (timer habilitado)
     * @return true se scheduleEnabled
     */
    bool isActive() const { return scheduleEnabled; }

    /**
     * @brief Retorna se o scheduler quer o exaustor ligado agora
     * @return true se dentro da janela configurada
     */
    bool wantsExhaustOn() const { return _exhaustOn; }

private:
    bool _exhaustOn = false; ///< Estado calculado: exaustor deve estar ligado?
};

#endif // EXHAUST_SCHEDULER_H
