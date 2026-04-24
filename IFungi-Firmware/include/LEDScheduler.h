#ifndef LED_SCHEDULER_H
#define LED_SCHEDULER_H

#include <Arduino.h>

/**
 * @file LEDScheduler.h
 * @brief Sistema de agendamento e simulação de ciclo solar para os LEDs
 * @version 1.0
 * @date 2026
 *
 * @details Gerencia dois modos de operação dos LEDs de crescimento:
 *
 *  MODO TIMER (scheduleEnabled = true):
 *  ─────────────────────────────────────
 *  Liga/desliga os LEDs em horários fixos configurados no Firebase.
 *  Exemplo: ligar às 06:00, desligar às 20:00.
 *  A intensidade é fixa (configIntensity, 0-255).
 *
 *  MODO SIMULAÇÃO SOLAR (solarSimEnabled = true):
 *  ─────────────────────────────────────────────
 *  Simula o arco solar ao longo do dia com intensidade variável:
 *
 *  Intensidade (%)
 *  100 |          ╭────────────╮
 *   75 |       ╭──╯            ╰──╮
 *   50 |    ╭──╯                  ╰──╮
 *   25 |  ╭─╯                        ╰─╮
 *    0 |──╯                            ╰──
 *       06h  08h  10h  12h  14h  16h  18h  20h
 *
 *  A curva usa uma função senoidal sobre a janela de luz definida.
 *  Pico de intensidade ao meio-dia solar (midpoint entre onHour e offHour).
 *
 *  PRIORIDADE:
 *  ─────────────────────────────────────
 *  1. debugMode ativo → scheduler desabilitado totalmente
 *  2. solarSimEnabled → ignora scheduleEnabled
 *  3. scheduleEnabled → intensidade fixa no período
 *  4. Nenhum ativo → não interfere no controle automático
 *
 *  ESTRUTURA NO FIREBASE RTDB:
 *  ─────────────────────────────────────
 *  /greenhouses/<ID>/led_schedule: {
 *    "scheduleEnabled": true,
 *    "solarSimEnabled": false,
 *    "onHour":   6,       // hora de ligar (0-23)
 *    "onMinute": 0,       // minuto de ligar (0-59)
 *    "offHour":  20,      // hora de desligar (0-23)
 *    "offMinute": 0,      // minuto de desligar (0-59)
 *    "intensity": 255     // intensidade fixa (0-255, ignorada no modo solar)
 *  }
 */

class LEDScheduler {
public:

    // ─── Configurações lidas do Firebase ──────────────────────────────────────

    bool  scheduleEnabled = false; ///< Timer simples ligado/desligado
    bool  solarSimEnabled = false; ///< Simulação de arco solar
    int   onHour          = 6;     ///< Hora de início (0-23)
    int   onMinute        = 0;     ///< Minuto de início (0-59)
    int   offHour         = 20;    ///< Hora de fim (0-23)
    int   offMinute       = 0;     ///< Minuto de fim (0-59)
    int   configIntensity = 255;   ///< Intensidade no modo timer (0-255)

    // ─── Interface pública ────────────────────────────────────────────────────

    /**
     * @brief Atualiza a lógica do scheduler com base no timestamp atual
     *
     * @param currentTimestamp Timestamp Unix atual (de getCurrentTimestamp())
     * @param debugMode        Se true, scheduler fica inativo
     *
     * @details Calcula se os LEDs devem estar ligados e em qual intensidade.
     *          Deve ser chamado periodicamente no loop (a cada ~5s é suficiente).
     */
    void update(unsigned long currentTimestamp, bool debugMode);

    /**
     * @brief Retorna se o scheduler quer os LEDs ligados agora
     * @return true se os LEDs devem estar ligados
     */
    bool wantsLEDsOn() const { return _ledsOn; }

    /**
     * @brief Retorna a intensidade calculada (0-255)
     * @return Intensidade atual calculada pelo scheduler
     */
    int getIntensity() const { return _intensity; }

    /**
     * @brief Retorna se o scheduler está ativo (algum modo habilitado)
     * @return true se scheduleEnabled ou solarSimEnabled
     */
    bool isActive() const { return scheduleEnabled || solarSimEnabled; }

    /**
     * @brief Converte segundos desde meia-noite em hora e minuto
     * @param secondsOfDay  Segundos decorridos desde 00:00:00 UTC (timestamp % 86400)
     * @param hour          Saída: hora (0-23)
     * @param minute        Saída: minuto (0-59)
     */
    static void secondsToHM(unsigned long secondsOfDay, int& hour, int& minute);

private:

    bool _ledsOn    = false; ///< Estado calculado: LEDs devem estar ligados?
    int  _intensity = 0;     ///< Intensidade calculada (0-255)

    /**
     * @brief Calcula intensidade do modo simulação solar
     *
     * @details Usa função senoidal normalizada:
     *   t = posição normalizada dentro da janela de luz (0.0 a 1.0)
     *   intensity = sin(t * PI) * 255
     *
     * Isso garante rampa suave de subida e descida, com pico ao meio do dia.
     *
     * @param nowMinutes   Minutos desde meia-noite (hora*60 + minuto)
     * @param startMinutes Minutos do horário de início
     * @param endMinutes   Minutos do horário de fim
     * @return Intensidade (0-255)
     */
    int _solarIntensity(int nowMinutes, int startMinutes, int endMinutes);
};

#endif // LED_SCHEDULER_H
