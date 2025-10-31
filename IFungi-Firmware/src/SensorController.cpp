#include "SensorController.h"
#include <DHT.h>  // Para DHT22

#define DHTPIN 33      // Pino do DHT22
#define DHTTYPE DHT22 // Tipo do sensor

DHT dht(DHTPIN, DHTTYPE);

void SensorController::begin() {
    Serial.println("Inicializando sensor DHT22");
    dht.begin();  // Inicializa o sensor DHT22
    
    // Adicionar delay para estabilização do DHT22
    delay(2000);
    
    // Verifica se o DHT22 está respondendo com tentativas
    int tentativasDHT = 0;
    dhtOK = false;
    
    while (tentativasDHT < 3 && !dhtOK) {
        float tempTest = dht.readTemperature();
        float umidTest = dht.readHumidity();
        
        if (!isnan(tempTest) && !isnan(umidTest)) {
            dhtOK = true;
            Serial.println("DHT22 inicializado com sucesso!");
        } else {
            Serial.printf("Tentativa %d: Falha ao ler DHT22\n", tentativasDHT + 1);
            tentativasDHT++;
            delay(1000);
        }
    }
    
    if (!dhtOK) {
        Serial.println("AVISO: DHT22 não respondeu. Continuando sem sensor de temperatura/umidade.");
    }

    pinMode(LDR_PIN, INPUT);
    pinMode(MQ7_PIN, INPUT);

    // Inicialização do sensor CCS811 com tratamento de erro melhorado
    Serial.println("Inicializando sensor CCS811");
    ccsOK = false;
    
    // Tentar inicializar o CCS811 várias vezes
    for (int tentativa = 0; tentativa < 3; tentativa++) {
        if (ccs.begin()) {
            ccsOK = true;
            Serial.println("CCS811 inicializado com sucesso!");
            
            // Aguardar o sensor ficar pronto com timeout
            unsigned long startTime = millis();
            while (!ccs.available() && (millis() - startTime < 5000)) {
                delay(100);
            }
            
            if (ccs.available()) {
                Serial.println("CCS811 pronto para leitura");
                break;
            } else {
                Serial.println("CCS811 não ficou pronto dentro do timeout");
                ccsOK = false;
            }
        } else {
            Serial.printf("Tentativa %d: Falha ao inicializar CCS811\n", tentativa + 1);
            delay(1000);
        }
    }
    
    if (!ccsOK) {
        Serial.println("AVISO: CCS811 não inicializado. Continuando sem sensor de CO2/TVOC.");
    }

    // Inicialização do sensor MQ-7
    Serial.println("Inicializando sensor MQ-7");
    pinMode(MQ7_PIN, INPUT);
    Serial.println("MQ-7 inicializado (aquecimento necessário)");

    // Inicialização do LDR
    Serial.println("Inicializando sensor de luminosidade (LDR)");
    pinMode(LDR_PIN, INPUT);

    // Inicialização de variáveis
    lastUpdate = 0;
    co2 = 0;
    co = 0;
    tvocs = 0; // IMPORTANTE: Inicializar tvocs
    temperature = 0;
    humidity = 0;
    light = 0;
    waterLevel = false;
    
    Serial.println("SensorController inicializado");
}

void SensorController::update() {
    // Garante leitura a cada 2 segundos
    if(millis() - lastUpdate >= 2000) {
        static unsigned int readCount = 0;
        
        // Leitura básica sempre acontece
        light = analogRead(LDR_PIN);
        co = analogRead(MQ7_PIN);
        
        // Leitura do DHT apenas se estiver OK
        if(dhtOK && (readCount % 2 == 0)) {
            temperature = dht.readTemperature();
            humidity = dht.readHumidity();
            
            // Verificar se as leituras são válidas
            if (isnan(temperature) || isnan(humidity)) {
                Serial.println("Leitura inválida do DHT22");
                dhtOK = false; // Marcar como defeituoso
            }
        }
        
        // Leitura do CCS811 apenas se estiver OK
        if(ccsOK && (readCount % 3 == 0)) {
            if (ccs.available()) {
                if (!ccs.readData()) {
                    co2 = ccs.geteCO2();
                    tvocs = ccs.getTVOC();
                } else {
                    Serial.println("Erro na leitura do CCS811");
                    ccsOK = false; // Marcar como defeituoso
                }
            }
        }

        // Leitura do nível de água
        waterLevel = (analogRead(WATERLEVEL_PIN) <= 35);

        // Log a cada 10 ciclos (20 segundos)
        if(readCount % 10 == 0) {
            Serial.printf("Leituras - Temp: %.1f C, Umid: %.1f %%, Luz: %d, CO: %d, CO2: %d ppm, TVOCs: %d ppb, Nível de água: %s\n", 
                         temperature, humidity, light, co, co2, tvocs, waterLevel ? "Baixo" : "Ok");
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