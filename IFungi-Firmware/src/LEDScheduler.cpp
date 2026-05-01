/**
 * @file LEDScheduler.cpp
 * @brief Implementação do agendador e simulador solar de LEDs
 * @version 1.3.0
 * @date 2026
 */

#include "LEDScheduler.h"
#include <math.h>

// ─── Utilitário estático ──────────────────────────────────────────────────────

void LEDScheduler::secondsToHM(unsigned long secondsOfDay, int& hour, int& minute) {
    hour   = (secondsOfDay / 3600) % 24;
    minute = (secondsOfDay % 3600) / 60;
}

// ─── Modo simulação solar ─────────────────────────────────────────────────────

int LEDScheduler::_solarIntensity(int nowMinutes, int startMinutes, int endMinutes) {
    if (endMinutes <= startMinutes) return 0; // Janela inválida

    // Posição normalizada dentro da janela (0.0 = início, 1.0 = fim)
    float t = (float)(nowMinutes - startMinutes) / (float)(endMinutes - startMinutes);

    if (t <= 0.0f || t >= 1.0f) return 0;

    // Senoide: sin(t * PI) → pico de 1.0 no meio-dia solar, zero nas extremidades
    float sinVal = sinf(t * 3.14159265f);

    // Garante intensidade mínima de 10% durante o período de luz
    // para não ficar completamente apagado nas primeiras/últimas horas
    float minFraction = 0.10f;
    float intensity   = (minFraction + (1.0f - minFraction) * sinVal) * 255.0f;

    return constrain((int)intensity, 0, 255);
}

// ─── Loop principal ───────────────────────────────────────────────────────────

void LEDScheduler::update(unsigned long currentTimestamp, bool debugMode) {
    // Scheduler fica completamente inativo no modo debug
    if (debugMode) {
        _ledsOn    = false;
        _intensity = 0;
        return;
    }

    // Nenhum modo ativo — não interfere
    if (!scheduleEnabled && !solarSimEnabled) {
        _ledsOn    = false;
        _intensity = 0;
        return;
    }

    // ─── Extrai hora e minuto do timestamp UTC ──────────────────────────────
    // O timestamp já vem ajustado para o fuso horário (UTC-3) por getCurrentTimestamp()
    unsigned long secondsOfDay = currentTimestamp % 86400UL; // segundos decorridos hoje
    int nowHour, nowMinute;
    secondsToHM(secondsOfDay, nowHour, nowMinute);

    int nowMinutes   = nowHour   * 60 + nowMinute;
    int startMinutes = onHour    * 60 + onMinute;
    int endMinutes   = offHour   * 60 + offMinute;

    // Valida janela
    if (endMinutes <= startMinutes) {
        Serial.println("[led] WARN: Horario de fim <= horario de inicio, scheduler ignorado.");
        _ledsOn    = false;
        _intensity = 0;
        return;
    }

    bool withinWindow = (nowMinutes >= startMinutes && nowMinutes < endMinutes);

    // ─── Modo simulação solar ───────────────────────────────────────────────
    if (solarSimEnabled) {
        if (withinWindow) {
            _ledsOn    = true;
            _intensity = _solarIntensity(nowMinutes, startMinutes, endMinutes);
        } else {
            _ledsOn    = false;
            _intensity = 0;
        }

        static int lastReportedMin = -1;
        if (nowMinutes != lastReportedMin) {
            Serial.printf("[led] Solar: %02d:%02d | Janela: %02d:%02d-%02d:%02d | "
                          "Intensidade: %d/255 (%d%%)\n",
                          nowHour, nowMinute,
                          onHour, onMinute, offHour, offMinute,
                          _intensity, (_intensity * 100) / 255);
            lastReportedMin = nowMinutes;
        }
        return;
    }

    // ─── Modo timer simples ─────────────────────────────────────────────────
    if (scheduleEnabled) {
        if (withinWindow) {
            _ledsOn    = true;
            _intensity = constrain(configIntensity, 0, 255);
        } else {
            _ledsOn    = false;
            _intensity = 0;
        }

        static int lastReportedMinTimer = -1;
        if (nowMinutes != lastReportedMinTimer) {
            Serial.printf("[led] Timer: %02d:%02d | Janela: %02d:%02d-%02d:%02d | "
                          "%s (int: %d)\n",
                          nowHour, nowMinute,
                          onHour, onMinute, offHour, offMinute,
                          _ledsOn ? "LIGADO" : "DESLIGADO",
                          _intensity);
            lastReportedMinTimer = nowMinutes;
        }
    }
}
