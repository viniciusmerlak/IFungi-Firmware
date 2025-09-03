#ifndef SENSOR_CONTROLLER_H
#define SENSOR_CONTROLLER_H
#include <Adafruit_CCS811.h>
#include <DHT.h>  // Para DHT22

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
    int getTVOCs(); // Add this line
    
private:
    // Defina os pinos conforme sua montagem
    static const uint8_t MQ7_PIN = 32;   // Pino analógico do MQ-7
    static const uint8_t DHT_PIN = 33;    // Pino do DHT22
    static const uint8_t LDR_PIN = 34;   // Pino analógico do LDR
    static const uint8_t WATERLEVEL_PIN = 35; // Pino do sensor de nível de água
    
    // Objetos dos sensores
    DHT dht{DHT_PIN, DHT22};
    Adafruit_CCS811 ccs;
    
    // Variáveis de estado
    bool dhtOK;
    bool ccsOK;
    unsigned long lastUpdate;
    
    // Variáveis das leituras
    float temperature;
    float humidity;
    int co2;
    int co;
    int tvocs;
    int light;
    bool waterLevel;
};

#endif