#include "SensorController.h"
#include <DHT.h>

void SensorController::begin() {
    Serial.println("[SENSOR] Inicializando controlador de sensores...");
    
    // Configuração dos pinos
    pinMode(WATERLEVEL_PIN, INPUT);
    pinMode(LDR_PIN, INPUT);
    pinMode(MQ7_PIN, INPUT);

    Serial.println("[SENSOR] Configuração de pinos concluída");
    Serial.printf("[SENSOR] Threshold sensor água: %d\n", WATER_LEVEL_THRESHOLD);
    
    // Inicialização DHT22
    Serial.println("[SENSOR] Inicializando DHT22...");
    dht.begin();
    delay(2000);
    
    int dhtAttempts = 0;
    dhtOK = false;
    
    while (dhtAttempts < 3 && !dhtOK) {
        float tempTest = dht.readTemperature();
        float humidityTest = dht.readHumidity();
        
        if (!isnan(tempTest) && !isnan(humidityTest)) {
            dhtOK = true;
            Serial.println("[SENSOR] DHT22 inicializado com sucesso");
        } else {
            Serial.printf("[SENSOR] Tentativa %d: Falha na leitura do DHT22\n", dhtAttempts + 1);
            dhtAttempts++;
            delay(1000);
        }
    }
    
    if (!dhtOK) {
        Serial.println("[SENSOR] DHT22: ERRO - Sensor não responde");
    }

    // Inicialização CCS811
    Serial.println("[SENSOR] Inicializando CCS811...");
    ccsOK = false;
    
    for (int attempt = 0; attempt < 3; attempt++) {
        if (ccs.begin()) {
            ccsOK = true;
            Serial.println("[SENSOR] CCS811 inicializado com sucesso");
            
            unsigned long startTime = millis();
            while (!ccs.available() && (millis() - startTime < 5000)) {
                delay(100);
            }
            
            if (ccs.available()) {
                Serial.println("[SENSOR] CCS811 pronto para leitura");
                break;
            } else {
                Serial.println("[SENSOR] CCS811: ERRO - Não ficou pronto dentro do timeout");
                ccsOK = false;
            }
        } else {
            Serial.printf("[SENSOR] Tentativa %d: Falha na inicialização do CCS811\n", attempt + 1);
            delay(1000);
        }
    }
    
    if (!ccsOK) {
        Serial.println("[SENSOR] CCS811: ERRO - Sensor não inicializado");
    }

    // Inicialização de variáveis
    lastUpdate = 0;
    co2 = 0;
    co = 0;
    tvocs = 0;
    temperature = 0;
    humidity = 0;
    light = 0;
    waterLevel = false;
    
    Serial.println("[SENSOR] Controlador de sensores inicializado com sucesso");
}

void SensorController::update() {
    if(millis() - lastUpdate >= 2000) {
        static unsigned int readCount = 0;
        
        // Leituras básicas
        light = analogRead(LDR_PIN);
        co = analogRead(MQ7_PIN);
        
        // Leitura DHT22
        if(dhtOK && (readCount % 2 == 0)) {
            temperature = dht.readTemperature();
            humidity = dht.readHumidity();
            
            if (isnan(temperature) || isnan(humidity)) {
                Serial.println("[SENSOR] DHT22: ERRO - Leitura inválida");
                dhtOK = false;
            }
        }
        
        // Leitura CCS811
        if(ccsOK && (readCount % 3 == 0)) {
            if (ccs.available()) {
                if (!ccs.readData()) {
                    co2 = ccs.geteCO2();
                    tvocs = ccs.getTVOC();
                } else {
                    Serial.println("[SENSOR] CCS811: ERRO - Falha na leitura");
                    ccsOK = false;
                }
            }
        }

        // 🔥 LEITURA CORRETA DO SENSOR DE ÁGUA
        int waterSensorValue = analogRead(WATERLEVEL_PIN);
        
        // LÓGICA: 
        // - Valor ALTO (>1917) = Sensor SECO = ÁGUA BAIXA (true)
        // - Valor BAIXO (<1917) = Sensor MOLHADO = ÁGUA OK (false)
        waterLevel = (waterSensorValue > WATER_LEVEL_THRESHOLD);
        
        // Debug detalhado do sensor de água
        if(readCount % 5 == 0) {
            float voltage = (waterSensorValue / 4095.0) * 3.3;
            Serial.printf("[SENSOR] Água: %d (%1.2fV) -> %s\n", 
                         waterSensorValue, voltage,
                         waterLevel ? "BAIXA" : "OK");
        }
        
        // Log resumido a cada 10 ciclos
        if(readCount % 10 == 0) {
            Serial.printf("[SENSOR] DHT22: %.1fC, %.1f%%, LDR: %d, MQ-7: %d, CCS811: %d ppm\n", 
                         temperature, humidity, light, co, co2);
        }
        
        lastUpdate = millis();
        readCount++;
    }
}

// 🔥 REMOVIDO: Funções de calibração manualCalibrateWaterSensor e autoCalibrateWaterSensor

// Mantenha as funções get
float SensorController::getTemperature() { return temperature; }
float SensorController::getHumidity() { return humidity; }
int SensorController::getCO2() { return co2; }
int SensorController::getCO() { return co; }
int SensorController::getTVOCs() { return tvocs; }
int SensorController::getLight() { return light; }
bool SensorController::getWaterLevel() { return waterLevel; }