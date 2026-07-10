/**
 * @file exhaustScheduler.cpp
 * @brief Implementação do agendador por horário do exaustor
 * @version 1.0
 * @date 2026
 */

#include "exhaustScheduler.h"
#include "LEDScheduler.h"

void ExhaustScheduler::update(unsigned long currentTimestamp, bool debugMode) {
    // Scheduler fica completamente inativo no modo debug
    if (debugMode) {
        _exhaustOn = false;
        return;
    }

    // Timer desabilitado — não interfere no controle automático
    if (!scheduleEnabled) {
        _exhaustOn = false;
        return;
    }

    // ─── Extrai hora e minuto do timestamp UTC ──────────────────────────────
    // O timestamp já vem ajustado para o fuso horário (UTC-3) por getCurrentTimestamp()
    unsigned long secondsOfDay = currentTimestamp % 86400UL; // segundos decorridos hoje
    int nowHour, nowMinute;
    LEDScheduler::secondsToHM(secondsOfDay, nowHour, nowMinute);

    int nowMinutes   = nowHour * 60 + nowMinute;
    int startMinutes = onHour  * 60 + onMinute;
    int endMinutes   = offHour * 60 + offMinute;

    // Valida janela
    if (endMinutes <= startMinutes) {
        Serial.println("[exhaust] WARN: Horario de fim <= horario de inicio, scheduler ignorado.");
        _exhaustOn = false;
        return;
    }

    bool withinWindow = (nowMinutes >= startMinutes && nowMinutes < endMinutes);
    _exhaustOn = withinWindow;

    static int lastReportedMin = -1;
    if (nowMinutes != lastReportedMin) {
        Serial.printf("[exhaust] Timer: %02d:%02d | Janela: %02d:%02d-%02d:%02d | %s\n",
                      nowHour, nowMinute,
                      onHour, onMinute, offHour, offMinute,
                      _exhaustOn ? "LIGADO" : "DESLIGADO");
        lastReportedMin = nowMinutes;
    }
}
