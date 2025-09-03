    #include "SensorController.h"
#include <DHT.h>  // Para DHT22

#define DHTPIN 33      // Pino do DHT22
#define DHTTYPE DHT22 // Tipo do sensor
int bcont1 = 0;
int bcont2 = 0;
int bcont3 = 0;
int bcont4 = 0;
int bcont5 = 0;
int bcont6 = 0;
int bcont7 = 0;



DHT dht(DHTPIN, DHTTYPE);

void SensorController::begin() {
    Serial.println("Inicializando sensor DHT22");
    dht.begin();  // Inicializa o sensor DHT22
    pinMode(LDR_PIN, INPUT); // Configura o pino do LDR como entrada
    pinMode(MQ7_PIN, INPUT); // Configura o pino do MQ-7 como entrada
    // Verifica se o DHT22 está respondendo
    if (isnan(dht.readTemperature())) {
        while(bcont1 <2) {
            Serial.println("Falha ao inicializar o DHT22!");
            bcont1++;
        }

        dhtOK = false;
    } else {
        while(bcont2 <2) {
            Serial.println("DHT22 inicializado com sucesso!");
            bcont2++;
        }
        dhtOK = true;
    }

    // Inicialização do sensor CCS811 (CO2 e TVOC)
    Serial.println("Inicializando sensor CCS811");
    if (!ccs.begin()) {
        while(bcont3 <2) {
            Serial.println("Falha ao inicializar o CCS811!");
            bcont3++;
        }
        ccsOK = false;
    } else {
        while(bcont4 <2) {
            Serial.println("CCS811 inicializado com sucesso!");
            bcont4++;
        }

        ccsOK = true;
        // Aguarda o sensor ficar pronto
        while(!ccs.available());
    }

    // Inicialização do sensor MQ-7 (CO)
    while (bcont5 < 2)
    {
        Serial.println("Inicializando sensor MQ-7");
        bcont5++;
        /* code */
    }
    
    pinMode(MQ7_PIN, INPUT);
    while(bcont6<2) {
        Serial.println("MQ-7 inicializado (aquecimento necessário)");
        bcont6++;
    }



    // Inicialização do LDR (Sensor de luz)
    while (bcont7 < 2) {
        Serial.println("Inicializando sensor de luminosidade (LDR)");
        bcont7++;
    }

    pinMode(LDR_PIN, INPUT);


    // Inicialização de variáveis
    lastUpdate = 0;
    co2 = 0;
    co = 0;
    temperature = 0;
    humidity = 0;
    light = 0;
    waterLevel = false;
}

void SensorController::update() {
    // Garante leitura a cada 2 segundos
    if(millis() - lastUpdate >= 2000) {
        if(dhtOK) {
            temperature = dht.readTemperature();
            humidity = dht.readHumidity();
        }
        
        light = analogRead(LDR_PIN);
        co = analogRead(MQ7_PIN);
        
        if(ccsOK && ccs.available()) {
            co2 = ccs.geteCO2();
            tvocs = ccs.getTVOC();
        }

        if(touchRead(WATERLEVEL_PIN) <= 35){
            waterLevel = true;
        }else{
            waterLevel = false;
        }

        Serial.printf("Leituras - Temp: %.1f C, Umid: %.1f %%, Luz: %d, CO: %d, CO2: %d ppm, TVOCs: %d ppb, Nível de água: %s\n", temperature, humidity, light, co, co2, tvocs, waterLevel ? "Baixo" : "Ok");
        lastUpdate = millis();
    }
}

float SensorController::getTemperature() { return temperature; }
float SensorController::getHumidity() { return humidity; }
int SensorController::getCO2() { return co2; }
int SensorController::getCO() { return co; }
int SensorController::getTVOCs() { return tvocs; }
int SensorController::getLight() { return light; }
bool SensorController::getWaterLevel() { return waterLevel; }