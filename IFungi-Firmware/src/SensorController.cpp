#include "SensorController.h"
#include <DHT.h>
#include <Preferences.h>

#define DHTPIN 33
#define DHTTYPE DHT22

DHT dht(DHTPIN, DHTTYPE);

void SensorController::begin() {
    Serial.println("Initializing SensorController...");
    loadWaterCalibration();
    Serial.println("Initializing sensor DHT22");
    dht.begin();
    
    delay(2000);
    
    // DHT22 initialization with retries
    int dhtAttempts = 0;
    dhtOK = false;
    
    while (dhtAttempts < 3 && !dhtOK) {
        float tempTest = dht.readTemperature();
        float humidityTest = dht.readHumidity();
        
        if (!isnan(tempTest) && !isnan(humidityTest)) {
            dhtOK = true;
            Serial.println("DHT22 initialized successfully!");
        } else {
            Serial.printf("Attempt %d: Failed to read DHT22\n", dhtAttempts + 1);
            dhtAttempts++;
            delay(1000);
        }
    }
    
    if (!dhtOK) {
        Serial.println("WARNING: DHT22 not responding. Continuing without temperature/humidity sensor.");
    }

    // Initialize pins
    pinMode(LDR_PIN, INPUT);
    pinMode(MQ7_PIN, INPUT);
    pinMode(WATERLEVEL_PIN, INPUT);

    // CCS811 initialization with improved error handling
    Serial.println("Initializing sensor CCS811");
    ccsOK = false;
    
    for (int attempt = 0; attempt < 3; attempt++) {
        if (ccs.begin()) {
            ccsOK = true;
            Serial.println("CCS811 initialized successfully!");
            
            // Wait for sensor to be ready with timeout
            unsigned long startTime = millis();
            while (!ccs.available() && (millis() - startTime < 5000)) {
                delay(100);
            }
            
            if (ccs.available()) {
                Serial.println("CCS811 ready for reading");
                break;
            } else {
                Serial.println("CCS811 didn't become ready within timeout");
                ccsOK = false;
            }
        } else {
            Serial.printf("Attempt %d: Failed to initialize CCS811\n", attempt + 1);
            delay(1000);
        }
    }
    
    if (!ccsOK) {
        Serial.println("WARNING: CCS811 not initialized. Continuing without CO2/TVOC sensor.");
    }

    // Water level sensor initialization - SIMPLIFIED
    Serial.println("Initializing water level sensor");
    Serial.println("💧 Using fixed threshold: 2000");

    // Initialize variables
    lastUpdate = 0;
    co2 = 0;
    co = 0;
    tvocs = 0;
    temperature = 0;
    humidity = 0;
    light = 0;
    waterLevel = false;
    
    Serial.println("SensorController initialized");
}

void SensorController::update() {
    // Ensure reading every 2 seconds
    if(millis() - lastUpdate >= 2000) {
        static unsigned int readCount = 0;
        
        // Basic readings always happen
        light = analogRead(LDR_PIN);
        co = analogRead(MQ7_PIN);
        
        // DHT reading only if OK
        if(dhtOK && (readCount % 2 == 0)) {
            temperature = dht.readTemperature();
            humidity = dht.readHumidity();
            
            // Check if readings are valid
            if (isnan(temperature) || isnan(humidity)) {
                Serial.println("Invalid DHT22 reading");
                dhtOK = false; // Mark as faulty
            }
        }
        
        // CCS811 reading only if OK
        if(ccsOK && (readCount % 3 == 0)) {
            if (ccs.available()) {
                if (!ccs.readData()) {
                    co2 = ccs.geteCO2();
                    tvocs = ccs.getTVOC();
                } else {
                    Serial.println("Error reading CCS811");
                    ccsOK = false; // Mark as faulty
                }
            }
        }

        // 🔥 LEITURA CALIBRADA DO SENSOR DE ÁGUA
        int waterSensorValue = analogRead(WATERLEVEL_PIN);
        // Usa os valores calibrados para determinar o threshold
        int currentThreshold = (waterDryValue + waterWetValue) / 2;
        waterLevel = (waterSensorValue <= currentThreshold);
        
        // Debug water sensor occasionally
        if(readCount % 5 == 0) {
            Serial.printf("💧 Water sensor - Raw: %d, Dry: %d, Wet: %d, Threshold: %d, Water: %s\n", 
                         waterSensorValue, waterDryValue, waterWetValue, currentThreshold, 
                         waterLevel ? "LOW" : "OK");
        }

        // Log every 10 cycles (20 seconds)
        if(readCount % 10 == 0) {
            Serial.printf("Readings - Temp: %.1f C, Humidity: %.1f %%, Light: %d, CO: %d, CO2: %d ppm, TVOCs: %d ppb, Water Level: %s\n", 
                         temperature, humidity, light, co, co2, tvocs, waterLevel ? "LOW" : "OK");
        }
        
        lastUpdate = millis();
        readCount++;
    }
}
// 🔥 NOVAS FUNÇÕES PARA CALIBRAÇÃO
void SensorController::calibrateWaterDry() {
    waterDryValue = analogRead(WATERLEVEL_PIN);
    // Salvar na NVS
    Preferences preferences;
    if (preferences.begin("water_calib", false)) {
        preferences.putInt("dry", waterDryValue);
        preferences.end();
        Serial.println("💧 CALIBRAÇÃO SECO: " + String(waterDryValue));
    }
}

void SensorController::calibrateWaterWet() {
    waterWetValue = analogRead(WATERLEVEL_PIN);
    // Salvar na NVS
    Preferences preferences;
    if (preferences.begin("water_calib", false)) {
        preferences.putInt("wet", waterWetValue);
        preferences.end();
        Serial.println("💧 CALIBRAÇÃO MOLHADO: " + String(waterWetValue));
    }
}

void SensorController::loadWaterCalibration() {
    Preferences preferences;
    if (preferences.begin("water_calib", true)) {
        waterDryValue = preferences.getInt("dry", 2000);
        waterWetValue = preferences.getInt("wet", 1000);
        preferences.end();
        Serial.println("💧 Calibração carregada - Seco: " + String(waterDryValue) + " Molhado: " + String(waterWetValue));
    }
}

float SensorController::getTemperature() { return temperature; }
float SensorController::getHumidity() { return humidity; }
int SensorController::getCO2() { return co2; }
int SensorController::getCO() { return co; }
int SensorController::getTVOCs() { return tvocs; }
int SensorController::getLight() { return light; }
bool SensorController::getWaterLevel() { return waterLevel; }