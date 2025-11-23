/**
 * @file SensorController.cpp
 * @brief Implementação do controlador de sensores ambientais
 * @author Seu Nome  
 * @date 2024
 * @version 1.0
 * 
 * @details Este arquivo implementa o controlador responsável pela leitura
 * e gerenciamento de todos os sensores do sistema:
 * - DHT22 (temperatura e umidade)
 * - CCS811 (CO2 e TVOCs)
 * - LDR (luminosidade)
 * - MQ-7 (monóxido de carbono)
 * - Sensor de nível de água
 * 
 * @note Inclui mecanismos de verificação de integridade dos sensores
 * e tratamento de falhas com tentativas de recuperação.
 */

#include "SensorController.h"
#include <DHT.h>

/**
 * @brief Inicializa todos os sensores do sistema
 * 
 * @details Realiza a configuração completa dos sensores:
 * - Configura pinos como entradas
 * - Inicializa DHT22 com verificações de funcionamento
 * - Inicializa CCS811 com timeout e tentativas
 * - Define valores iniciais para todas as variáveis
 * 
 * @note Inclui múltiplas tentativas para sensores que podem falhar
 * na inicialização (DHT22, CCS811)
 * 
 * @warning Sensores com falha na inicialização são marcados como
 * inoperantes e não serão utilizados nas leituras
 * 
 * @see SensorController::update()
 */
void SensorController::begin() {
    Serial.println("[SENSOR] Inicializando controlador de sensores...");
    
    // Configuração dos pinos dos sensores
    pinMode(WATERLEVEL_PIN, INPUT);
    pinMode(LDR_PIN, INPUT);
    pinMode(MQ7_PIN, INPUT);

    Serial.println("[SENSOR] Configuração de pinos concluída");
    Serial.printf("[SENSOR] Threshold sensor água: %d\n", WATER_LEVEL_THRESHOLD);
    
    // =========================================================================
    // INICIALIZAÇÃO DO SENSOR DHT22 (TEMPERATURA E UMIDADE)
    // =========================================================================
    
    Serial.println("[SENSOR] Inicializando DHT22...");
    dht.begin();
    delay(2000); // Aguarda estabilização do sensor
    
    int dhtAttempts = 0;
    dhtOK = false;
    
    // Tentativas de inicialização do DHT22
    while (dhtAttempts < 3 && !dhtOK) {
        float tempTest = dht.readTemperature();
        float humidityTest = dht.readHumidity();
        
        // Verifica se as leituras são válidas (não NaN)
        if (!isnan(tempTest) && !isnan(humidityTest)) {
            dhtOK = true;
            Serial.println("[SENSOR] DHT22 inicializado com sucesso");
        } else {
            Serial.printf("[SENSOR] Tentativa %d: Falha na leitura do DHT22\n", dhtAttempts + 1);
            dhtAttempts++;
            delay(1000); // Aguarda antes da próxima tentativa
        }
    }
    
    // Relata falha permanente do DHT22
    if (!dhtOK) {
        Serial.println("[SENSOR] DHT22: ERRO - Sensor não responde");
    }

    // =========================================================================
    // INICIALIZAÇÃO DO SENSOR CCS811 (CO2 E TVOCs)
    // =========================================================================
    
    Serial.println("[SENSOR] Inicializando CCS811...");
    ccsOK = false;
    
    // Tentativas de inicialização do CCS811
    for (int attempt = 0; attempt < 3; attempt++) {
        if (ccs.begin()) {
            ccsOK = true;
            Serial.println("[SENSOR] CCS811 inicializado com sucesso");
            
            // Aguarda sensor ficar pronto para leitura (timeout 5s)
            unsigned long startTime = millis();
            while (!ccs.available() && (millis() - startTime < 5000)) {
                delay(100);
            }
            
            // Verifica se sensor ficou pronto dentro do timeout
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
    
    // Relata falha permanente do CCS811
    if (!ccsOK) {
        Serial.println("[SENSOR] CCS811: ERRO - Sensor não inicializado");
    }

    // =========================================================================
    // INICIALIZAÇÃO DE VARIÁVEIS DE ESTADO
    // =========================================================================

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

/**
 * @brief Atualiza leituras de todos os sensores
 * 
 * @details Executa leituras periódicas dos sensores com diferentes taxas:
 * - Leituras básicas (LDR, MQ-7): a cada 2 segundos
 * - DHT22: a cada 4 segundos (leitura mais lenta)
 * - CCS811: a cada 6 segundos (leitura mais lenta)
 * - Sensor de água: a cada 2 segundos com debug periódico
 * 
 * @note Utiliza contador estático para alternar leituras lentas
 * e inclui verificações de integridade dos dados
 * 
 * @see SensorController::begin()
 */
void SensorController::update() {
    // Verifica se passou intervalo mínimo desde última atualização
    if(millis() - lastUpdate >= 2000) {
        static unsigned int readCount = 0;
        
        // =====================================================================
        // LEITURAS BÁSICAS (RÁPIDAS)
        // =====================================================================
        
        light = analogRead(LDR_PIN);   // Luminosidade (LDR)
        co = analogRead(MQ7_PIN);      // Monóxido de carbono (MQ-7)
        
        // =====================================================================
        // LEITURA DHT22 (TEMPERATURA E UMIDADE) - A CADA 2 CICLOS
        // =====================================================================
        
        if(dhtOK && (readCount % 2 == 0)) {
            temperature = dht.readTemperature();
            humidity = dht.readHumidity();
            
            // Verifica validade das leituras
            if (isnan(temperature) || isnan(humidity)) {
                Serial.println("[SENSOR] DHT22: ERRO - Leitura inválida");
                dhtOK = false; // Marca sensor como defeituoso
            }
        }
        
        // =====================================================================
        // LEITURA CCS811 (CO2 E TVOCs) - A CADA 3 CICLOS  
        // =====================================================================
        
        if(ccsOK && (readCount % 3 == 0)) {
            if (ccs.available()) {
                if (!ccs.readData()) {
                    co2 = ccs.geteCO2();   // CO2 equivalente
                    tvocs = ccs.getTVOC(); // Compostos orgânicos voláteis
                } else {
                    Serial.println("[SENSOR] CCS811: ERRO - Falha na leitura");
                    ccsOK = false; // Marca sensor como defeituoso
                }
            }
        }

        // =====================================================================
        // LEITURA DO SENSOR DE NÍVEL DE ÁGUA
        // =====================================================================
        
        int waterSensorValue = analogRead(WATERLEVEL_PIN);
        
        /**
         * @brief Lógica do sensor de nível de água
         * @details 
         * - Valor ALTO (>1917) = Sensor SECO = ÁGUA BAIXA (true)
         * - Valor BAIXO (<1917) = Sensor MOLHADO = ÁGUA OK (false)
         */
        waterLevel = (waterSensorValue > WATER_LEVEL_THRESHOLD);
        
        // Debug detalhado do sensor de água (a cada 5 ciclos)
        if(readCount % 5 == 0) {
            float voltage = (waterSensorValue / 4095.0) * 3.3;
            Serial.printf("[SENSOR] Água: %d (%1.2fV) -> %s\n", 
                         waterSensorValue, voltage,
                         waterLevel ? "BAIXA" : "OK");
        }
        
        // Log resumido de todos os sensores (a cada 10 ciclos)
        if(readCount % 10 == 0) {
            Serial.printf("[SENSOR] DHT22: %.1fC, %.1f%%, LDR: %d, MQ-7: %d, CCS811: %d ppm\n", 
                         temperature, humidity, light, co, co2);
        }
        
        // Atualiza timestamp e incrementa contador
        lastUpdate = millis();
        readCount++;
    }
}

// =============================================================================
// FUNÇÕES DE ACESSO AOS DADOS DOS SENSORES
// =============================================================================

/**
 * @brief Obtém a temperatura atual
 * @return Temperatura em graus Celsius
 */
float SensorController::getTemperature() { return temperature; }

/**
 * @brief Obtém a umidade atual  
 * @return Umidade relativa em porcentagem
 */
float SensorController::getHumidity() { return humidity; }

/**
 * @brief Obtém o nível de CO2
 * @return Concentração de CO2 em ppm (partes por milhão)
 */
int SensorController::getCO2() { return co2; }

/**
 * @brief Obtém o nível de monóxido de carbono
 * @return Leitura analógica do sensor MQ-7 (0-4095)
 */
int SensorController::getCO() { return co; }

/**
 * @brief Obtém o nível de compostos orgânicos voláteis
 * @return Concentração de TVOCs em ppb (partes por bilhão)
 */
int SensorController::getTVOCs() { return tvocs; }

/**
 * @brief Obtém o nível de luminosidade
 * @return Leitura analógica do LDR (0-4095)
 */
int SensorController::getLight() { return light; }

/**
 * @brief Obtém o estado do nível de água
 * @return true se nível de água está baixo, false se normal
 */
bool SensorController::getWaterLevel() { return waterLevel; }