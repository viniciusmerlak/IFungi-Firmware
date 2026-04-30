/**
 * @file SensorController.cpp
 * @brief Controlador de sensores ambientais (DHT22, CCS811, LDR, MQ-7, nível de água)
 */

#include "SensorController.h"
#include <Adafruit_CCS811.h>
#include <cmath>
#include <type_traits>

// Garante API esperada da Adafruit CCS811 (readData → uint8_t, 0 = sucesso) sem fixar versão no platformio.ini
static_assert(std::is_same<decltype(std::declval<Adafruit_CCS811>().readData()), uint8_t>::value,
              "IFungi: atualize 'adafruit/Adafruit CCS811 Library' — readData() deve retornar uint8_t (0 = sucesso). Verifique release >= 1.1.x.");

int SensorController::mq7PpmFromAdc(int adcRaw, float tempC, float rhPct, bool compensate) const {
    if (adcRaw <= 0) return 0;
    float v = (adcRaw / 4095.0f) * 3.3f;
    if (v < 0.05f) v = 0.05f;
    if (v >= MQ7_VC_VOLTS - 0.05f) v = MQ7_VC_VOLTS - 0.05f;

    // Divisor típico: Vout = Vc * Rs / (Rs + RL) → Rs = RL * (Vc - Vout) / Vout  (Rs em kΩ)
    float rsK = MQ7_RL_KOHM * (MQ7_VC_VOLTS - v) / v;
    if (rsK < 0.01f) rsK = 0.01f;

    if (compensate) {
        // Correção leve RH/T (literatura MQ; não altera buffers do DHT — só usa cópias)
        float env = 1.0f + 0.0018f * (tempC - 20.0f) - 0.004f * (rhPct - 50.0f) / 50.0f;
        if (env < 0.6f) env = 0.6f;
        if (env > 1.6f) env = 1.6f;
        rsK /= env;
    }

    float ratio = rsK / MQ7_R0_KOHM;
    if (ratio < 0.01f) ratio = 0.01f;

    float ppm = MQ7_CURVE_A * powf(ratio, MQ7_CURVE_B);
    if (ppm < 0.0f) ppm = 0.0f;
    if (ppm > 2000.0f) ppm = 2000.0f;
    return (int)(ppm + 0.5f);
}

void SensorController::begin() {
    Serial.println("[sensor] Inicializando controlador de sensores...");

    mq7WarmupUntil = millis() + 120000UL;
    Serial.println("[sensor] MQ-7: aguardando ~120 s de estabilização antes de usar ppm (sem leitura de aquecedor).");

    pinMode(WATERLEVEL_PIN, INPUT);
    pinMode(LDR_PIN, INPUT);
    pinMode(MQ7_PIN, INPUT);

    Serial.println("[sensor] Configuração de pinos concluída");
    Serial.printf("[sensor] Threshold sensor água: %d\n", WATER_LEVEL_THRESHOLD);

    Serial.println("[sensor] Inicializando DHT22...");
    dht.begin();
    delay(2000);

    int dhtAttempts = 0;
    dhtOK = false;

    while (dhtAttempts < 3 && !dhtOK) {
        float tempTest = dht.readTemperature();
        float humidityTest = dht.readHumidity();

        if (!isnan(tempTest) && !isnan(humidityTest)) {
            dhtOK = true;
            Serial.println("[sensor] DHT22 inicializado com sucesso");
        } else {
            Serial.printf("[sensor] Tentativa %d: Falha na leitura do DHT22\n", dhtAttempts + 1);
            dhtAttempts++;
            delay(1000);
        }
    }

    if (!dhtOK) {
        Serial.println("[sensor] DHT22: ERRO - Sensor não responde");
    }

    Serial.println("[sensor] Inicializando CCS811...");
    ccsOK = false;

    for (int attempt = 0; attempt < 3; attempt++) {
        if (ccs.begin()) {
            ccsOK = true;
            Serial.println("[sensor] CCS811 inicializado com sucesso");

            unsigned long startTime = millis();
            while (!ccs.available() && (millis() - startTime < 5000)) {
                delay(100);
            }

            if (ccs.available()) {
                Serial.println("[sensor] CCS811 pronto para leitura");
                break;
            } else {
                Serial.println("[sensor] CCS811: ERRO - Não ficou pronto dentro do timeout");
                ccsOK = false;
            }
        } else {
            Serial.printf("[sensor] Tentativa %d: Falha na inicialização do CCS811\n", attempt + 1);
            delay(1000);
        }
    }

    if (!ccsOK) {
        Serial.println("[sensor] CCS811: ERRO - Sensor não inicializado");
    }

    lastUpdate = 0;
    co2 = 0;
    co = 0;
    tvocs = 0;
    temperature = 0;
    humidity = 0;
    light = 0;
    waterLevel = false;

    Serial.println("[sensor] Controlador de sensores inicializado com sucesso");
}

void SensorController::update() {
    if (millis() - lastUpdate >= 2000) {
        static unsigned int readCount = 0;

        light         = analogRead(LDR_PIN);
        int mqAdc     = analogRead(MQ7_PIN);

        if (readCount % 2 == 0) {
            if (dhtOK) {
                float newTemp = dht.readTemperature();
                float newHum  = dht.readHumidity();

                if (isnan(newTemp) || isnan(newHum)) {
                    dhtFailCount++;
                    Serial.printf("[sensor] DHT22: leitura inválida (%d/3 falhas consecutivas)\n", dhtFailCount);

                    if (dhtFailCount >= 3) {
                        dhtOK = false;
                        dhtRecoveryTime = millis();
                        // Fail-safe while DHT is down: keep climate neutral and avoid over-humidification.
                        temperature = 25.0f;
                        humidity = 100.0f;
                        Serial.println("[sensor] DHT22: INOPERANTE — tentativa de recuperação em 30s");
                    }
                } else {
                    temperature  = newTemp;
                    humidity     = newHum;
                    dhtFailCount = 0;

                    if (ccsOK) {
                        float hEnv = humidity;
                        float tEnv = temperature;
                        ccs.setEnvironmentalData(hEnv, tEnv);
                    }
                }
            } else {
                if (millis() - dhtRecoveryTime >= 30000) {
                    Serial.println("[sensor] DHT22: tentando recuperação...");
                    dht.begin();
                    delay(500);
                    float testTemp = dht.readTemperature();
                    float testHum  = dht.readHumidity();
                    if (!isnan(testTemp) && !isnan(testHum)) {
                        dhtOK        = true;
                        dhtFailCount = 0;
                        temperature  = testTemp;
                        humidity     = testHum;
                        Serial.println("[sensor] DHT22: RECUPERADO com sucesso");
                        if (ccsOK) {
                            ccs.setEnvironmentalData(humidity, temperature);
                        }
                    } else {
                        dhtRecoveryTime = millis();
                        Serial.println("[sensor] DHT22: recuperação falhou, nova tentativa em 30s");
                    }
                }
            }
        }

        if (readCount % 3 == 0) {
            if (ccsOK) {
                if (ccs.available()) {
                    uint8_t st = ccs.readData();
                    if (st == 0) {
                        co2          = ccs.geteCO2();
                        tvocs        = ccs.getTVOC();
                        ccsFailCount = 0;
                    } else {
                        ccsFailCount++;
                        Serial.printf("[sensor] CCS811: falha na leitura código %u (%d/3)\n", st, ccsFailCount);

                        if (ccsFailCount >= 3) {
                            ccsOK           = false;
                            ccsRecoveryTime = millis();
                            Serial.println("[sensor] CCS811: INOPERANTE — tentativa de recuperação em 30s");
                        }
                    }
                }
            } else {
                if (millis() - ccsRecoveryTime >= 30000) {
                    Serial.println("[sensor] CCS811: tentando recuperação...");
                    if (ccs.begin()) {
                        unsigned long t = millis();
                        while (!ccs.available() && millis() - t < 3000) delay(100);
                        if (ccs.available()) {
                            ccsFailCount = 0;
                            ccsOK        = true;
                            Serial.println("[sensor] CCS811: RECUPERADO com sucesso");
                            if (dhtOK) {
                                ccs.setEnvironmentalData(humidity, temperature);
                            }
                        } else {
                            ccsRecoveryTime = millis();
                            Serial.println("[sensor] CCS811: recuperação falhou, nova tentativa em 30s");
                        }
                    } else {
                        ccsRecoveryTime = millis();
                        Serial.println("[sensor] CCS811: recuperação falhou, nova tentativa em 30s");
                    }
                }
            }
        }

        if (millis() < mq7WarmupUntil) {
            co = 0;
        } else {
            float tComp = dhtOK ? temperature : 20.0f;
            float hComp = dhtOK ? humidity : 50.0f;
            co = mq7PpmFromAdc(mqAdc, tComp, hComp, dhtOK);
        }

        int waterSensorValue = analogRead(WATERLEVEL_PIN);
        waterLevel = (waterSensorValue > WATER_LEVEL_THRESHOLD);

        if (readCount % 5 == 0) {
            float voltage = (waterSensorValue / 4095.0) * 3.3;
            Serial.printf("[sensor] Água: %d (%1.2fV) -> %s\n",
                         waterSensorValue, voltage,
                         waterLevel ? "BAIXA" : "OK");
        }

        if (readCount % 10 == 0) {
            Serial.printf("[sensor] DHT22: %.1fC, %.1f%%, LDR: %d, CO: %d ppm, CCS811: %d ppm CO2\n",
                         temperature, humidity, light, co, co2);
        }

        lastUpdate = millis();
        readCount++;
    }
}

float SensorController::getTemperature() { return temperature; }
float SensorController::getHumidity() { return humidity; }
int SensorController::getCO2() { return co2; }
int SensorController::getCO() { return co; }
int SensorController::getTVOCs() { return tvocs; }
int SensorController::getLight() { return light; }
bool SensorController::getWaterLevel() { return waterLevel; }
