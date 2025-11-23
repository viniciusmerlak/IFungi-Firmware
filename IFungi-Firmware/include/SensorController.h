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
    int getCO();
    int getLight();
    int getTVOCs();
    
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
    
    float temperature;
    float humidity;
    int co2;
    int co;
    int tvocs;
    int light;
    bool waterLevel;
    
    // 🔥 THRESHOLD CORRIGIDO baseado nas suas medições
    // SECO: ~1985 (1.6V), MOLHADO: ~1849-1861 (1.49-1.50V)
    const int WATER_LEVEL_THRESHOLD = 1917; // Valor médio
};

#endif