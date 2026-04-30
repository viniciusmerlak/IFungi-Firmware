#ifndef SENSORCONTROLLER_H
#define SENSORCONTROLLER_H

#include <Arduino.h>
#include <DHT.h>
#include <Adafruit_CCS811.h>

class SensorController {
public:
    void begin();
    void update();
    bool getWaterLevel();
    float getTemperature();
    float getHumidity();
    int getCO2();
    /// CO estimado em ppm (MQ-7; após warmup). Não altera leituras do DHT22.
    int getCO();
    int getLight();
    int getTVOCs();
    bool isDHTHealthy() const { return dhtOK; }
    bool isCCS811Healthy() const { return ccsOK; }
    bool isMQ7Healthy() const { return millis() >= mq7WarmupUntil; }
    bool isLDRHealthy() const { return true; }
    bool isWaterLevelHealthy() const { return true; }
    
private:
    static const uint8_t MQ7_PIN = 35;
    static const uint8_t DHT_PIN = 33;
    static const uint8_t LDR_PIN = 34;
    static const uint8_t WATERLEVEL_PIN = 32;
    
    DHT dht{DHT_PIN, DHT22};
    Adafruit_CCS811 ccs;
    
    bool dhtOK;
    bool ccsOK;
    unsigned long lastUpdate;
    
    uint8_t       dhtFailCount    = 0;
    uint8_t       ccsFailCount    = 0;
    unsigned long dhtRecoveryTime = 0;
    unsigned long ccsRecoveryTime = 0;
    
    float temperature;
    float humidity;
    int co2;
    int co;           ///< CO em ppm (MQ-7)
    int tvocs;
    int light;
    bool waterLevel;

    unsigned long mq7WarmupUntil = 0;

    /// Tensão no divisor do módulo (tip. 5 V no elemento MQ-7; ajuste se sua PCB for 3,3 V).
    static constexpr float MQ7_VC_VOLTS   = 5.0f;
    /// Resistência de carga do módulo em kΩ (tip. 10 k na placa YK/MQ).
    static constexpr float MQ7_RL_KOHM    = 10.0f;
    /// R0 em kΩ no ar “limpo” — calibre com medição de referência se necessário.
    static constexpr float MQ7_R0_KOHM    = 18.0f;
    /// Curva tipo datasheet Hanwei / aproximação DFRobot: ppm ≈ A * (Rs/R0)^B
    static constexpr float MQ7_CURVE_A    = 99.042f;
    static constexpr float MQ7_CURVE_B    = -1.518f;

    int mq7PpmFromAdc(int adcRaw, float tempC, float rhPct, bool compensate) const;
    
    const int WATER_LEVEL_THRESHOLD = 1917;
};

#endif
