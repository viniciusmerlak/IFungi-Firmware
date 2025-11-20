#ifndef SENSOR_CONTROLLER_H
#define SENSOR_CONTROLLER_H

#include <Adafruit_CCS811.h>
#include <DHT.h>

class SensorController {
public:
    void begin();
    void update();
    bool getWaterLevel();
    float getTemperature();
    float getHumidity();
    int getCO2();
    int getCO();
    int getLight();
    int getTVOCs();
    
private:
    static const uint8_t MQ7_PIN = 35;
    static const uint8_t DHT_PIN = 33;
    static const uint8_t LDR_PIN = 34;
    static const uint8_t WATERLEVEL_PIN = 32; // Pino para sensor de água
    
    DHT dht{DHT_PIN, DHT22};
    Adafruit_CCS811 ccs;
    
    bool dhtOK;
    bool ccsOK;
    unsigned long lastUpdate;
    
    float temperature;
    float humidity;
    int co2;
    int co;
    int tvocs;
    int light;
    bool waterLevel;
    
    // 🔥 SIMPLIFICADO: Threshold fixo para sensor de água
    const int WATER_LEVEL_THRESHOLD = 100; // Valor empírico - ajuste conforme necessário
};

#endif