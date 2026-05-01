#include "GreenhouseSystem.h"
#include "OperationMode.h"
#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>
#include <ArduinoJson.h>   // necessário para desserializar registros NVS em sendLocalData

String FirebaseHandler::getMacAddress() {
    return ::getMacAddress();
}

bool FirebaseHandler::authenticate(const String& email, const String& password) {
    // SEGURANÇA: armazenamos em variáveis membro para garantir que os c_str()
    // não apontem para stack que será desalocada após o retorno desta função.
    static String _apiKey;
    static String _dbUrl;
    static String _email;
    static String _password;

    _apiKey    = FIREBASE_API_KEY;
    _dbUrl     = DATABASE_URL;
    _email     = email;
    _password  = password;

    Serial.println("[firebase] Autenticando com Firebase...");

    Firebase.reset(&config);
    Firebase.reconnectNetwork(true);

    config.api_key        = _apiKey.c_str();
    auth.user.email       = _email.c_str();
    auth.user.password    = _password.c_str();
    config.database_url   = _dbUrl.c_str();

    // CORREÇÃO (v1.2.2): token_status_callback alimenta o watchdog enquanto
    // Firebase.begin() processa a autenticação em background. Sem ele, o loop
    // de espera abaixo segurava o core 1 por até 30 s, disparando o TWDT
    // quando a lifeSupportTask estava ativa no core 0.
    config.token_status_callback = tokenStatusCallback;

    Firebase.begin(&config, &auth);

    Serial.print("[firebase] Aguardando autenticação");
    unsigned long startTime = millis();
    const unsigned long TIMEOUT = 30000;

    while (millis() - startTime < TIMEOUT) {
        if (Firebase.ready()) {
            authenticated = true;
            userUID = String(auth.token.uid.c_str());
            greenhouseId = "IFUNGI-" + getMacAddress();

            Serial.println("\n[firebase] Autenticação bem-sucedida!");
            Serial.print("[firebase] UID: "); Serial.println(userUID);
            RLOG_INFO("[firebase]", "Autenticado com sucesso — sistema online");

            verifyGreenhouse();
            checkUserPermission(userUID, greenhouseId);
            return true;
        }
        // CORREÇÃO (v1.2.2): vTaskDelay em vez de delay() para ceder ao
        // scheduler do FreeRTOS e alimentar o watchdog durante a espera.
        // delay() chama vTaskDelay internamente mas sem yield explícito,
        // o que em algumas versões do core pode acumular 'ticks perdidos'
        // e travar outras tasks (como lifeSupportTask no core 0).
        vTaskDelay(pdMS_TO_TICKS(500));
        Serial.print(".");
    }

    Serial.println("\n[firebase] ERRO - Autenticação expirada (timeout)");
    RLOG_ERROR("[firebase]", "Autenticação expirada (timeout 30s) — verifique credenciais e conexão");
    return false;
}

void FirebaseHandler::updateActuatorState(bool relay1, bool relay2, bool relay3, bool relay4, 
                                         bool ledsOn, int ledsWatts, bool humidifierOn) {
    if (!authenticated || !Firebase.ready()) {
        Serial.println("[firebase] ERRO - Não autenticado para atualizar atuadores");
        return;
    }

    FirebaseJson json;
    FirebaseJson actuators;
    
    actuators.set("rele1", relay1);
    actuators.set("rele2", relay2);
    actuators.set("rele3", relay3);
    actuators.set("rele4", relay4);
    
    FirebaseJson leds;
    leds.set("ligado", ledsOn);
    leds.set("watts", ledsWatts);
    actuators.set("leds", leds);
    actuators.set("umidificador", humidifierOn);

    json.set("lastUpdate", (int)getCurrentTimestamp());
    json.set("atuadores", actuators);

    String path = FirebaseHandler::getGreenhousesPath() + greenhouseId;
    
    if (Firebase.updateNode(fbdo, path.c_str(), json)) {
        Serial.println("[firebase] Estados dos atuadores atualizados com sucesso");
    } else {
        Serial.println("[firebase] ERRO - Falha ao atualizar atuadores: " + fbdo.errorReason());
        RLOG_FMT(LOG_ERROR, "[firebase]", "Falha ao atualizar atuadores: %s", fbdo.errorReason().c_str());
    }
}

void FirebaseHandler::refreshTokenIfNeeded() {
    if(!initialized || !authenticated) return;
    
    if(millis() - lastTokenRefresh > TOKEN_REFRESH_INTERVAL) {
        Serial.println("Refreshing Firebase token...");
        Firebase.refreshToken(&config);
        lastTokenRefresh = millis();
    }
}

void FirebaseHandler::verifyGreenhouse() {
    if(!authenticated) {
        Serial.println("User not authenticated. Check credentials.");
        return;
    }
    delay(1000);
    
    Serial.println("Checking greenhouse...");
    if(greenhouseExists(greenhouseId)) {
        Serial.println("Greenhouse found: " + greenhouseId);
    } else {
        Serial.println("Greenhouse not found, creating new...");
        createInitialGreenhouse(userUID, userUID);
    }
}

FirebaseHandler::FirebaseHandler() 
    : timeClient(ntpUDP, "pool.ntp.org", -3 * 3600, 60000) {
}

FirebaseHandler::~FirebaseHandler() {
}

bool FirebaseHandler::sendDataToHistory(float temp, float humidity, int co2, int co, int lux, int tvocs) {
    if (!authenticated || !Firebase.ready()) {
        Serial.println("[firebase] OFFLINE - Dados salvos localmente");
        unsigned long timestamp = getCurrentTimestamp();
        saveDataLocally(temp, humidity, co2, co, lux, tvocs, timestamp);
        return false;
    }
    if (isnan(temp) || isnan(humidity)) {
        Serial.println("[Error] ERRO - Dados inválidos (temp ou humidity)");
        temp = -999; // Valor inválido claro para temperatura aceito pelo json (NAN causaria falha de parsing)
        humidity = -999; // Valor inválido claro para umidade (100.0f manteria umidificador desligado, mas -999 destaca que é um erro de leitura)
        
    }
    String tsStr = String(getCurrentTimestamp());
    String path = "/historico/" + greenhouseId + "/" + tsStr;
    
    FirebaseJson json;
    json.set("timestamp", tsStr);
    json.set("temperatura", temp);
    json.set("umidade", humidity);
    json.set("co2", co2);
    json.set("co", co);
    json.set("tvocs", tvocs);
    json.set("luminosidade", lux);
    json.set("dataHora", getFormattedDateTime());
    
    if (Firebase.setJSON(fbdo, path.c_str(), json)) {
        Serial.println("[firebase] Dados salvos no histórico com sucesso");
        return true;
    } else {
        Serial.println("[firebase] ERRO - Falha ao salvar histórico: " + fbdo.errorReason());
        unsigned long tsLocal = getCurrentTimestamp();
        saveDataLocally(temp, humidity, co2, co, lux, tvocs, tsLocal);
        return false;
    }
}

unsigned long FirebaseHandler::getCurrentTimestamp() {
    static bool ntpInitialized = false;
    if (!ntpInitialized && WiFi.status() == WL_CONNECTED) {
        timeClient.begin();
        ntpInitialized = true;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        timeClient.update();
        return timeClient.getEpochTime();
    } else {
        // Fallback offline: usa millis() relativo sem tocar na NVS
        // para evitar spam de erros NOT_FOUND quando NVS foi apagada.
        static unsigned long millisOffsetCalc = 0;
        static bool offsetLoaded = false;

        if (!offsetLoaded && nvsInitialized) {
            // Só tenta carregar o offset se a NVS foi inicializada com sucesso
            Preferences prefs;
            if (prefs.begin(NAMESPACE, true)) {
                unsigned long saved = prefs.getULong("ultimo_timestamp", 0);
                if (saved > 0) {
                    millisOffsetCalc = saved - (millis() / 1000);
                }
                prefs.end();
            }
            offsetLoaded = true;
        }
        return (millis() / 1000) + millisOffsetCalc;
    }
}

String FirebaseHandler::getFormattedDateTime() {
    unsigned long timestamp = getCurrentTimestamp();
    if (timestamp > 1609459200) {
        time_t time = timestamp;
        struct tm *tm = gmtime(&time);
        char buffer[25];
        strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", tm);
        return String(buffer);
    }
    return "";
}

void FirebaseHandler::saveDataLocally(float temp, float humidity, int co2, int co, int lux, int tvocs, unsigned long timestamp) {
    if (!initializeNVS()) {
        Serial.println("Could not save data - NVS not available");
        return;
    }
    
    Preferences prefs;
    if (!prefs.begin(NAMESPACE, false)) {
        Serial.println("Failed to open Preferences for writing");
        return;
    }
    
    int numRecords = prefs.getInt("num_registros", 0);
    
    if (numRecords >= MAX_RECORDS) {
        // Descarta o registro mais antigo (índice 0) deslocando todos
        for (int i = 1; i < MAX_RECORDS; i++) {
            String src = prefs.getString(("reg_" + String(i)).c_str(), "");
            if (src.length() > 0) {
                prefs.putString(("reg_" + String(i - 1)).c_str(), src.c_str());
            }
        }
        numRecords = MAX_RECORDS - 1;
        prefs.remove(("reg_" + String(numRecords)).c_str());
    }
    
    // Chaves abreviadas para economizar espaço na NVS
    String record = "{\"t\":"   + String(temp, 1)     +
                    ",\"h\":"   + String(humidity, 1)  +
                    ",\"c2\":" + String(co2)           +
                    ",\"co\":" + String(co)            +
                    ",\"l\":"   + String(lux)           +
                    ",\"v\":"   + String(tvocs)         +
                    ",\"ts\":" + String(timestamp)     + "}";

    prefs.putString(("reg_" + String(numRecords)).c_str(), record.c_str());
    prefs.putInt("num_registros", numRecords + 1);
    prefs.putULong("ultimo_timestamp", timestamp);
    
    prefs.end();
    Serial.printf("[nvs] Registro %d salvo localmente (%d bytes)\n", numRecords, record.length());
}

// =============================================================================
// =============================================================================
// CORREÇÃO (v1.2.1): initializeNVS()
//
// Bug original: a chave "nvs_inicializada" tem 16 caracteres — um acima do
// limite de 15 da NVS do ESP32. O nvs_set_i32 falha com KEY_TOO_LONG e a
// ESP-IDF loga o erro, mas a função continuava sem gravar a flag. No boot
// seguinte, prefs.getInt("nvs_inicializada", 0) retornava 0 novamente e o
// bloco de "primeira execução" rodava indefinidamente.
//
// Correção: chave renomeada para "nvs_init" (8 chars), dentro do limite.
//
// Bug original 2: a função era chamada DEPOIS de setupSensorsAndActuators(),
// mas loadSetpointsNVS() (dentro dela) tentava abrir o namespace antes de
// initializeNVS() criá-lo, causando NOT_FOUND no primeiro boot.
// Correção: initializeNVS() movido para ANTES de setupSensorsAndActuators()
// no setup() do MainController.
// =============================================================================
bool FirebaseHandler::initializeNVS() {
    if (nvsInitialized) return true;

    Preferences prefs;

    // Abre diretamente em modo escrita (false) para criar o namespace
    // caso ainda não exista (situação comum após flash erase).
    if (!prefs.begin(NAMESPACE, false)) {
        // Falha rara — pode ocorrer se a partição NVS estiver corrompida.
        // NÃO logamos com Serial.println aqui para evitar spam; o caller
        // já trata o retorno false.
        return false;
    }

    // Verifica se já foi inicializada em algum boot anterior.
    // CORREÇÃO (v1.2.1): chave renomeada de "nvs_inicializada" (16 chars, > limite de 15)
    // para "nvs_init" (8 chars). A chave longa causava KEY_TOO_LONG silencioso no
    // nvs_set_i32, então a flag nunca era gravada e o bloco de "primeira execução"
    // rodava a cada boot sem nunca marcar como inicializado.
    if (prefs.getInt("nvs_init", 0) == 0) {
        Serial.println("[nvs] Flash apagada ou primeira execução — criando estrutura NVS");
        prefs.putInt("num_registros", 0);
        prefs.putInt("nvs_init",      1);
        Serial.println("[nvs] Estrutura NVS criada com sucesso");
    }

    size_t freeEntries = prefs.freeEntries();
    Serial.printf("[nvs] Namespace '%s' pronto | entradas livres: %u\n", NAMESPACE, freeEntries);

    prefs.end();
    nvsInitialized = true;
    return true;
}

void FirebaseHandler::sendLocalData() {
    if (!initializeNVS()) {
        Serial.println("Could not send local data - NVS not available");
        return;
    }
    
    Preferences prefs;
    if (!prefs.begin(NAMESPACE, true)) {
        Serial.println("Failed to open Preferences for reading");
        return;
    }
    
    int numRecords = prefs.getInt("num_registros", 0);
    Serial.println("[nvs] Tentando enviar " + String(numRecords) + " registros locais");
    prefs.end();

    if (numRecords == 0) return;

    if (!prefs.begin(NAMESPACE, false)) {
        Serial.println("Failed to open Preferences for writing");
        return;
    }
    
    int sent = 0;
    bool sentFlags[MAX_RECORDS] = {};

    for (int i = 0; i < numRecords; i++) {
        String keyName = "reg_" + String(i);
        String record  = prefs.getString(keyName.c_str(), "");

        if (record.length() == 0) {
            sentFlags[i] = true;
            continue;
        }

        StaticJsonDocument<128> doc;
        DeserializationError err = deserializeJson(doc, record);
        if (err) {
            Serial.printf("[nvs] Registro %d corrompido, descartando: %s\n", i, err.c_str());
            sentFlags[i] = true;
            continue;
        }

        float         temp      = doc["t"]  | 0.0f;
        float         humidity  = doc["h"]  | 0.0f;
        int           co2       = doc["c2"] | 0;
        int           co        = doc["co"] | 0;
        int           lux       = doc["l"]  | 0;
        int           tvocs     = doc["v"]  | 0;

        if (Firebase.ready() && authenticated) {
            if (sendDataToHistory(temp, humidity, co2, co, lux, tvocs)) {
                sentFlags[i] = true;
                sent++;
            } else {
                Serial.println("[nvs] Falha ao enviar registro " + String(i) + ", interrompendo.");
                break;
            }
        } else {
            Serial.println("[nvs] Firebase indisponível, interrompendo envio.");
            break;
        }
    }
    
    int newIndex = 0;
    for (int i = 0; i < numRecords; i++) {
        if (sentFlags[i]) {
            prefs.remove(("reg_" + String(i)).c_str());
            continue;
        }
        String record = prefs.getString(("reg_" + String(i)).c_str(), "");
        if (record.length() > 0) {
            if (i != newIndex) {
                prefs.putString(("reg_" + String(newIndex)).c_str(), record.c_str());
                prefs.remove(("reg_" + String(i)).c_str());
            }
            newIndex++;
        }
    }
    
    prefs.putInt("num_registros", newIndex);
    prefs.end();
    
    Serial.printf("[nvs] Envio concluído: %d enviados, %d restantes.\n", sent, newIndex);
}

void FirebaseHandler::sendSensorData(float temp, float humidity, int co2, int co, int lux, int tvocs, bool waterLevel) {
    refreshTokenIfNeeded();
    if (!authenticated || !Firebase.ready()) {
        Serial.println("[firebase] ERRO - Não autenticado para enviar dados");
        return;
    }

    String basePath = "/greenhouses/" + greenhouseId;
    
    // Enviar sensores em objeto aninhado
    FirebaseJson sensores;
    sensores.set("temperatura", temp);
    sensores.set("umidade", humidity);
    sensores.set("co2", co2);
    sensores.set("co", co);
    sensores.set("tvocs", tvocs);
    sensores.set("luminosidade", lux);

    FirebaseJson json;
    json.set("sensores", sensores);
    json.set("lastUpdate", (int)getCurrentTimestamp());
    json.set("niveis/agua", waterLevel);

    if (Firebase.updateNode(fbdo, basePath.c_str(), json)) {
        Serial.println("[firebase] Dados dos sensores enviados com sucesso");
    } else {
        Serial.println("[firebase] ERRO - Falha ao enviar dados: " + fbdo.errorReason());
        RLOG_FMT(LOG_ERROR, "[firebase]", "Falha ao enviar dados dos sensores: %s", fbdo.errorReason().c_str());
    }
}

void FirebaseHandler::updateSensorHealth(bool dhtOk, bool ccsOk, bool mq7Ok, bool ldrOk, bool waterOk) {
    if (!authenticated || !Firebase.ready()) return;

    static bool lastDht   = true, lastCcs  = true;
    static bool lastMq7   = false;
    static bool lastWater = true;

    if (!dhtOk  && lastDht)    RLOG_ERROR("[sensor]", "DHT22 INOPERANTE — temperatura/umidade indisponivel");
    if (dhtOk   && !lastDht)   RLOG_INFO ("[sensor]", "DHT22 RECUPERADO — leituras normalizadas");
    if (!ccsOk  && lastCcs)    RLOG_ERROR("[sensor]", "CCS811 INOPERANTE — CO2/TVOCs indisponivel");
    if (ccsOk   && !lastCcs)   RLOG_INFO ("[sensor]", "CCS811 RECUPERADO — leituras normalizadas");
    if (!waterOk && lastWater)  RLOG_WARN ("[sensor]", "Nivel de agua BAIXO — umidificador bloqueado");
    if (waterOk  && !lastWater) RLOG_INFO ("[sensor]", "Nivel de agua OK — umidificador liberado");
    if (mq7Ok   && !lastMq7)   RLOG_INFO ("[sensor]", "MQ-7 aquecido — leituras de CO disponiveis");

    lastDht   = dhtOk;
    lastCcs   = ccsOk;
    lastMq7   = mq7Ok;
    lastWater = waterOk;

    auto sensorErr = [](bool ok, const char* code) -> String {
        return ok ? "OK" : String(code);
    };

    FirebaseJson json;
    json.set("sensor_status/dht22_sensorError",      sensorErr(dhtOk,   "SensorError01"));
    json.set("sensor_status/ccs811_sensorError",     sensorErr(ccsOk,   "SensorError02"));
    json.set("sensor_status/mq07_sensorError",       sensorErr(mq7Ok,   "SensorError03"));
    json.set("sensor_status/ldr_sensorError",        sensorErr(ldrOk,   "SensorError04"));
    json.set("sensor_status/waterlevel_sensorError", sensorErr(waterOk, "SensorError05"));
    json.set("sensor_status/lastUpdate", (int)getCurrentTimestamp());

    String path = "/greenhouses/" + greenhouseId;
    if (!Firebase.updateNode(fbdo, path.c_str(), json)) {
        Serial.println("[firebase] erro ao atualizar sensor_status: " + fbdo.errorReason());
        RLOG_FMT(LOG_ERROR, "[firebase]", "Falha ao atualizar sensor_status: %s", fbdo.errorReason().c_str());
    }
}

bool FirebaseHandler::checkUserPermission(const String& userUID, const String& greenhouseID) {
    if (!Firebase.ready()) {
        Serial.println("Firebase not ready.");
        return false;
    }

    String userPath        = "/Usuarios/" + userUID;
    String permissionsPath = userPath + "/Estufas permitidas";

    auto writePermissionsArray = [&](FirebaseJsonArray& arr) -> bool {
        if (Firebase.setArray(fbdo, permissionsPath.c_str(), arr)) {
            return true;
        }
        String fallback;
        arr.toString(fallback, false);
        return Firebase.setString(fbdo, permissionsPath.c_str(), fallback);
    };

    if (Firebase.get(fbdo, permissionsPath.c_str())) {
        String dataType = fbdo.dataType();

        if (dataType == "json" || dataType == "array") {
            if (Firebase.getArray(fbdo, permissionsPath.c_str())) {
                FirebaseJsonArray* arr = fbdo.jsonArrayPtr();
                FirebaseJsonData   item;
                size_t count = arr->size();
                for (size_t idx = 0; idx < count; idx++) {
                    arr->get(item, idx);
                    if (item.stringValue == greenhouseID) {
                        Serial.println("User already has permission for: " + greenhouseID);
                        return true;
                    }
                }
                arr->add(greenhouseID);
                if (writePermissionsArray(*arr)) {
                    Serial.println("Greenhouse added to existing permission list: " + greenhouseID);
                    return true;
                }
            }
        } else if (dataType == "string") {
            String existing = fbdo.stringData();
            existing.trim();
            Serial.println("[permission] Formato antigo detectado, migrando para array...");

            FirebaseJsonArray newArray;
            if (existing.length() > 0) {
                newArray.add(existing);
            }

            bool alreadyIn = (existing == greenhouseID);
            if (!alreadyIn) {
                newArray.add(greenhouseID);
            }

            if (writePermissionsArray(newArray)) {
                String arrayJson;
                newArray.toString(arrayJson, false);
                Serial.println("[permission] Migrado para array: " + arrayJson);
                return true;
            } else {
                Serial.println("[permission] Falha na migracao: " + fbdo.errorReason());
                return false;
            }
        }
    }

    Serial.println("[permission] Criando permissão de acesso para: " + greenhouseID);
    FirebaseJsonArray newArray;
    newArray.add(greenhouseID);
    if (writePermissionsArray(newArray)) {
        Serial.println("[permission] Permissao criada com sucesso");
        return true;
    }

    FirebaseJson userData;
    userData.set("Estufas permitidas/0", greenhouseID);
    if (Firebase.setJSON(fbdo, userPath.c_str(), userData)) {
        Serial.println("[permission] No do usuario criado com permissao");
        return true;
    }

    Serial.println("[permission] Erro critico ao criar permissao: " + fbdo.errorReason());
    return false;
}

void FirebaseHandler::createInitialGreenhouse(const String& creatorUser, const String& currentUser) {
    if (!authenticated) {
        Serial.println("User not authenticated.");
        return;
    }
    
    if (!Firebase.ready()) {
        Serial.println("Token not ready. Waiting...");
        unsigned long startTime = millis();
        while (!Firebase.ready() && (millis() - startTime < 10000)) {
            delay(500);
        }
        if (!Firebase.ready()) {
            Serial.println("Timeout waiting for token.");
            return;
        }
    }

    // ── Preservar dados do usuario antes de sobrescrever ────────────────────
    // Se a estufa ja existir, lemos setpoints, led_schedule e operation_mode
    // do Firebase para nao perder configuracoes do usuario (ex: apos OTA).
    String path = "/greenhouses/" + greenhouseId;
    bool hadExistingData = false;

    // Defaults (usados apenas se nao existir dado no Firebase)
    int   savedLux = 5000, savedCoSp = 50, savedCo2Sp = 400, savedTvocsSp = 100;
    float savedTMax = 30.0f, savedTMin = 20.0f, savedUMax = 80.0f, savedUMin = 60.0f;

    bool  savedSchedEnabled = false, savedSolarSim = false;
    int   savedOnH = 6, savedOnM = 0, savedOffH = 20, savedOffM = 0, savedIntensity = 255;

    String savedMode = "manual";

    if (Firebase.getJSON(fbdo, path.c_str()) && fbdo.dataType() != "null") {
        hadExistingData = true;
        FirebaseJson *existing = fbdo.jsonObjectPtr();
        FirebaseJsonData r;

        // Preserva setpoints do usuario
        if (existing->get(r, "setpoints/lux"))     savedLux     = r.intValue;
        if (existing->get(r, "setpoints/tMax"))     savedTMax    = r.floatValue;
        if (existing->get(r, "setpoints/tMin"))     savedTMin    = r.floatValue;
        if (existing->get(r, "setpoints/uMax"))     savedUMax    = r.floatValue;
        if (existing->get(r, "setpoints/uMin"))     savedUMin    = r.floatValue;
        if (existing->get(r, "setpoints/coSp"))     savedCoSp    = r.intValue;
        if (existing->get(r, "setpoints/co2Sp"))    savedCo2Sp   = r.intValue;
        if (existing->get(r, "setpoints/tvocsSp"))  savedTvocsSp = r.intValue;

        // Preserva led_schedule
        if (existing->get(r, "led_schedule/scheduleEnabled")) savedSchedEnabled = r.boolValue;
        if (existing->get(r, "led_schedule/solarSimEnabled")) savedSolarSim     = r.boolValue;
        if (existing->get(r, "led_schedule/onHour"))          savedOnH          = r.intValue;
        if (existing->get(r, "led_schedule/onMinute"))        savedOnM          = r.intValue;
        if (existing->get(r, "led_schedule/offHour"))         savedOffH         = r.intValue;
        if (existing->get(r, "led_schedule/offMinute"))       savedOffM         = r.intValue;
        if (existing->get(r, "led_schedule/intensity"))       savedIntensity    = r.intValue;

        // Preserva operation_mode
        if (existing->get(r, "operation_mode/mode"))          savedMode = r.stringValue;

        Serial.println("[firebase] Dados do usuario preservados antes de recriar estrutura");
    }
    // ────────────────────────────────────────────────────────────────────────

    FirebaseJson json;
    
    FirebaseJson actuators;
    actuators.set("leds/ligado", false);
    actuators.set("leds/watts", 0);
    actuators.set("rele1", false);
    actuators.set("rele2", false);
    actuators.set("rele3", false);
    actuators.set("rele4", false);
    actuators.set("umidificador", false);
    json.set("atuadores", actuators);

    json.set("createdBy", creatorUser);
    json.set("currentUser", currentUser);
    json.set("lastUpdate", (int)getCurrentTimestamp());

    FirebaseJson sensors;
    sensors.set("tvocs", 0);
    sensors.set("co", 0);
    sensors.set("co2", 0);
    sensors.set("luminosidade", 0);
    sensors.set("temperatura", 0);
    sensors.set("umidade", 0);
    json.set("sensores", sensors);

    FirebaseJson sensorStatus;
    sensorStatus.set("dht22_sensorError", "OK");
    sensorStatus.set("ccs811_sensorError", "OK");
    sensorStatus.set("mq07_sensorError", "SensorError03");
    sensorStatus.set("ldr_sensorError", "OK");
    sensorStatus.set("waterlevel_sensorError", "OK");
    sensorStatus.set("lastUpdate", (int)getCurrentTimestamp());
    json.set("sensor_status", sensorStatus);

    // Usa setpoints preservados (ou defaults para estufa nova)
    FirebaseJson setpoints;
    setpoints.set("lux",     savedLux);
    setpoints.set("tMax",    savedTMax);
    setpoints.set("tMin",    savedTMin);
    setpoints.set("uMax",    savedUMax);
    setpoints.set("uMin",    savedUMin);
    setpoints.set("coSp",    savedCoSp);
    setpoints.set("co2Sp",   savedCo2Sp);
    setpoints.set("tvocsSp", savedTvocsSp);
    json.set("setpoints", setpoints);

    json.set("niveis/agua", false);
    json.set("debug_mode", false);

    FirebaseJson manualActuators;
    manualActuators.set("rele1", false);
    manualActuators.set("rele2", false);
    manualActuators.set("rele3", false);
    manualActuators.set("rele4", false);
    manualActuators.set("leds/ligado", false);
    manualActuators.set("leds/intensity", 0);
    manualActuators.set("umidificador", false);
    json.set("manual_actuators", manualActuators);

    FirebaseJson devmode;
    devmode.set("analogRead", false);
    devmode.set("boolean", false);
    devmode.set("pin", -1);
    devmode.set("pwm", false);
    devmode.set("pwmValue", 0);
    json.set("devmode", devmode);

    // Usa led_schedule preservado (ou defaults para estufa nova)
    FirebaseJson ledSched;
    ledSched.set("scheduleEnabled", savedSchedEnabled);
    ledSched.set("solarSimEnabled", savedSolarSim);
    ledSched.set("onHour",          savedOnH);
    ledSched.set("onMinute",        savedOnM);
    ledSched.set("offHour",         savedOffH);
    ledSched.set("offMinute",       savedOffM);
    ledSched.set("intensity",       savedIntensity);
    json.set("led_schedule", ledSched);

    // Usa operation_mode preservado (ou default para estufa nova)
    FirebaseJson opMode;
    opMode.set("mode",        savedMode);
    opMode.set("lastChanged", (int)getCurrentTimestamp());
    opMode.set("changedBy",   "esp32");
    json.set("operation_mode", opMode);

    FirebaseJson status;
    status.set("online", true);
    status.set("lastHeartbeat", (int)getCurrentTimestamp());
    status.set("ip", WiFi.localIP().toString());
    json.set("status", status);

    FirebaseJson otaNode;
    otaNode.set("available", false);
    otaNode.set("version",   "");
    otaNode.set("url",       "");
    otaNode.set("notes",     "Insira a URL HTTPS do .bin e mude available para true");
    json.set("ota", otaNode);

    if (Firebase.setJSON(fbdo, path.c_str(), json)) {
        if (hadExistingData) {
            Serial.println("[firebase] Greenhouse recreated — user data preserved (setpoints, led_schedule, mode)");
        } else {
            Serial.println("[firebase] Greenhouse created successfully with complete structure");
        }
        checkUserPermission(userUID, greenhouseId);
    } else {
        Serial.print("[firebase] Error creating greenhouse: ");
        Serial.println(fbdo.errorReason());
        
        if (fbdo.errorReason().indexOf("token") >= 0) {
            Serial.println("[firebase] Invalid token, trying to renew...");
            Firebase.refreshToken(&config);
            delay(1000);
            if (Firebase.ready() && Firebase.setJSON(fbdo, path.c_str(), json)) {
                if (hadExistingData) {
                    Serial.println("[firebase] Greenhouse recreated after token renewal — user data preserved");
                } else {
                    Serial.println("[firebase] Greenhouse created after renewing token");
                }
                checkUserPermission(userUID, greenhouseId);
            }
        }
    }
}

void FirebaseHandler::sendHeartbeat() {
    if (!authenticated || !Firebase.ready()) {
        return;
    }

    String path = getGreenhousesPath() + greenhouseId + "/status";
    
    FirebaseJson json;
    json.set("online", true);
    json.set("lastHeartbeat", (int)getCurrentTimestamp());
    json.set("ip", WiFi.localIP().toString());
    
    if (Firebase.updateNode(fbdo, path.c_str(), json)) {
        lastHeartbeatTime = millis();
        Serial.println("Heartbeat sent successfully");
    } else {
        Serial.println("Failed to send heartbeat: " + fbdo.errorReason());
    }
}

bool FirebaseHandler::greenhouseExists(const String& greenhouseId) {
    if (!authenticated) {
        Serial.println("User not authenticated.");
        return false;
    }

    String path = "/greenhouses/" + greenhouseId;
    
    if (Firebase.get(fbdo, path.c_str())) {
        if (fbdo.dataType() != "null") {
            Serial.println("Greenhouse found. Checking structure...");
            if (!isGreenhouseStructureComplete(greenhouseId)) {
                Serial.println("[firebase] WARN: Estrutura incompleta apos tentativa de reparo. "
                               "Continuando sem recriar para preservar dados do usuario.");
            } else {
                Serial.println("Greenhouse structure is complete.");
            }
            return true;
        }
    }
    
    Serial.println("Greenhouse not found.");
    return false;
}

bool FirebaseHandler::isGreenhouseStructureComplete(const String& greenhouseId) {
    if (!authenticated || !Firebase.ready()) {
        return false;
    }

    String path = "/greenhouses/" + greenhouseId;
    
    const char* requiredFields[] = {
        "atuadores",
        "atuadores/leds",
        "atuadores/leds/ligado",
        "atuadores/leds/watts",
        "atuadores/rele1",
        "atuadores/rele2", 
        "atuadores/rele3",
        "atuadores/rele4",
        "atuadores/umidificador",
        "sensores",
        "sensores/temperatura",
        "sensores/umidade",
        "sensores/co2",
        "sensores/co",
        "sensores/tvocs",
        "sensores/luminosidade",
        "sensor_status",
        "sensor_status/dht22_sensorError",
        "sensor_status/ccs811_sensorError",
        "sensor_status/mq07_sensorError",
        "sensor_status/ldr_sensorError",
        "sensor_status/waterlevel_sensorError",
        // setpoints REMOVIDOS: são dados do usuário, nunca reparados com defaults
        "niveis/agua",
        "createdBy",
        "currentUser",
        "lastUpdate",
        "status",
        "status/online",
        "status/lastHeartbeat",
        "led_schedule",
        "led_schedule/scheduleEnabled",
        "led_schedule/solarSimEnabled",
        "led_schedule/onHour",
        "led_schedule/offHour",
        "led_schedule/intensity",
        "operation_mode",
        "operation_mode/mode",
        "ota"
    };
    
    const int numFields = sizeof(requiredFields) / sizeof(requiredFields[0]);
    
    if (!Firebase.getJSON(fbdo, path.c_str())) {
        Serial.println("[firebase] Error loading greenhouse JSON: " + fbdo.errorReason());
        return false;
    }
    
    FirebaseJson *jsonPtr = fbdo.jsonObjectPtr();
    FirebaseJsonData result;
    
    bool needsUpdate = false;
    FirebaseJson updateJson;
    bool cannotRepair = false;

    auto addDefaultForField = [&](const String& field) {
        if (field == "atuadores") {
            FirebaseJson a;
            a.set("leds/ligado", false);
            a.set("leds/watts", 0);
            a.set("rele1", false); a.set("rele2", false);
            a.set("rele3", false); a.set("rele4", false);
            a.set("umidificador", false);
            updateJson.set("atuadores", a);
        } else if (field == "atuadores/leds/ligado") updateJson.set(field, false);
        else if (field == "atuadores/leds/watts") updateJson.set(field, 0);
        else if (field == "atuadores/rele1" || field == "atuadores/rele2" ||
                 field == "atuadores/rele3" || field == "atuadores/rele4" ||
                 field == "atuadores/umidificador") updateJson.set(field, false);
        else if (field == "sensores") {
            FirebaseJson s;
            s.set("temperatura", 0); s.set("umidade", 0);
            s.set("co2", 0); s.set("co", 0);
            s.set("tvocs", 0); s.set("luminosidade", 0);
            updateJson.set("sensores", s);
        } else if (field == "sensores/temperatura" || field == "sensores/umidade") updateJson.set(field, 0.0f);
        else if (field == "sensores/co2" || field == "sensores/co" ||
                 field == "sensores/tvocs" || field == "sensores/luminosidade") updateJson.set(field, 0);
        else if (field == "sensor_status") {
            FirebaseJson ss;
            ss.set("dht22_sensorError", "OK");
            ss.set("ccs811_sensorError", "OK");
            ss.set("mq07_sensorError", "SensorError03");
            ss.set("ldr_sensorError", "OK");
            ss.set("waterlevel_sensorError", "OK");
            ss.set("lastUpdate", (int)getCurrentTimestamp());
            updateJson.set("sensor_status", ss);
        } else if (field == "sensor_status/dht22_sensorError" || field == "sensor_status/ccs811_sensorError" ||
                   field == "sensor_status/ldr_sensorError" || field == "sensor_status/waterlevel_sensorError") {
            updateJson.set(field, "OK");
        } else if (field == "sensor_status/mq07_sensorError") updateJson.set(field, "SensorError03");
        // setpoints removidos — nunca reparados com defaults do firmware
        else if (field == "niveis/agua") updateJson.set(field, false);
        else if (field == "createdBy" || field == "currentUser") updateJson.set(field, userUID);
        else if (field == "lastUpdate") updateJson.set(field, (int)getCurrentTimestamp());
        else if (field == "status") {
            FirebaseJson st;
            st.set("online", true);
            st.set("lastHeartbeat", (int)getCurrentTimestamp());
            st.set("ip", WiFi.localIP().toString());
            updateJson.set("status", st);
        } else if (field == "status/online") updateJson.set(field, true);
        else if (field == "status/lastHeartbeat") updateJson.set(field, (int)getCurrentTimestamp());
        else if (field == "led_schedule") {
            FirebaseJson ls;
            ls.set("scheduleEnabled", false); ls.set("solarSimEnabled", false);
            ls.set("onHour", 6); ls.set("onMinute", 0);
            ls.set("offHour", 20); ls.set("offMinute", 0);
            ls.set("intensity", 255);
            updateJson.set("led_schedule", ls);
        } else if (field == "led_schedule/scheduleEnabled") updateJson.set(field, false);
        else if (field == "led_schedule/solarSimEnabled") updateJson.set(field, false);
        else if (field == "led_schedule/onHour") updateJson.set(field, 6);
        else if (field == "led_schedule/offHour") updateJson.set(field, 20);
        else if (field == "led_schedule/intensity") updateJson.set(field, 255);
        else if (field == "operation_mode") {
            FirebaseJson om;
            om.set("mode", "manual");
            om.set("lastChanged", (int)getCurrentTimestamp());
            om.set("changedBy", "esp32");
            updateJson.set("operation_mode", om);
        } else if (field == "operation_mode/mode") updateJson.set(field, "manual");
        else if (field == "ota") {
            FirebaseJson ota;
            ota.set("available", false); ota.set("version", "");
            ota.set("url", ""); ota.set("notes", "");
            updateJson.set("ota", ota);
        } else {
            cannotRepair = true;
            Serial.println("[repair] Campo obrigatorio sem default mapeado: " + field);
        }
    };

    for (int i = 0; i < numFields; i++) {
        if (!jsonPtr->get(result, requiredFields[i]) ||
            result.typeNum == FirebaseJson::JSON_NULL) {
            String missing = String(requiredFields[i]);
            Serial.println("[firebase] Missing/null field: " + missing);
            needsUpdate = true;
            addDefaultForField(missing);
        }
    }

    if (!jsonPtr->get(result, "debug_mode")) {
        updateJson.set("debug_mode", false);
        needsUpdate = true;
    }
    if (!jsonPtr->get(result, "manual_actuators")) {
        FirebaseJson ma;
        ma.set("rele1", false); ma.set("rele2", false);
        ma.set("rele3", false); ma.set("rele4", false);
        ma.set("leds/ligado", false); ma.set("leds/intensity", 0);
        ma.set("umidificador", false);
        updateJson.set("manual_actuators", ma);
        needsUpdate = true;
    }
    if (!jsonPtr->get(result, "devmode")) {
        FirebaseJson dm;
        dm.set("analogRead", false); dm.set("boolean", false);
        dm.set("pin", -1); dm.set("pwm", false); dm.set("pwmValue", 0);
        updateJson.set("devmode", dm);
        needsUpdate = true;
    }
    if (!jsonPtr->get(result, "led_schedule")) {
        FirebaseJson ls;
        ls.set("scheduleEnabled", false); ls.set("solarSimEnabled", false);
        ls.set("onHour", 6); ls.set("onMinute", 0);
        ls.set("offHour", 20); ls.set("offMinute", 0);
        ls.set("intensity", 255);
        updateJson.set("led_schedule", ls);
        needsUpdate = true;
    }
    if (!jsonPtr->get(result, "ota")) {
        FirebaseJson ota;
        ota.set("available", false); ota.set("version", "");
        ota.set("url", ""); ota.set("notes", "");
        updateJson.set("ota", ota);
        needsUpdate = true;
    }
    if (!jsonPtr->get(result, "operation_mode")) {
        FirebaseJson om;
        om.set("mode", "manual");
        om.set("lastChanged", (int)getCurrentTimestamp());
        om.set("changedBy", "esp32");
        updateJson.set("operation_mode", om);
        needsUpdate = true;
    }

    if (needsUpdate) {
        if (cannotRepair) {
            Serial.println("[repair] Falha: ha campos obrigatorios sem estrategia de reparo.");
            return false;
        }
        Serial.println("[repair] Repairing greenhouse structure...");
        if (Firebase.updateNode(fbdo, path.c_str(), updateJson)) {
            Serial.println("[repair] Greenhouse structure repaired successfully");
            return true;
        } else {
            Serial.println("[repair] Failed to repair: " + fbdo.errorReason());
            return false;
        }
    }
    
    Serial.println("[repair] All required fields are present and valid");
    return true;
}

bool FirebaseHandler::loadFirebaseCredentials(String& email, String& password) {
    Preferences prefs;
    // CORREÇÃO (v1.2.1): abrimos em modo escrita (false) em vez de read-only (true).
    //
    // Com mode=true, se o namespace "firebase-creds" ainda não existir (flash nova
    // ou apagada), o ESP-IDF loga [E][Preferences.cpp:50] nvs_open failed: NOT_FOUND
    // a cada 30 s (intervalo de reconexão), poluindo o serial e enganando a análise.
    //
    // Com mode=false, o namespace é criado silenciosamente se ausente. As chaves
    // "email" e "password" simplesmente retornam "" (getString default), e a função
    // retorna false sem nenhum erro de log — comportamento correto para "sem credenciais".
    if (!prefs.begin("firebase-creds", false)) {
        // Falha real (partição NVS corrompida) — rara, mas tratada.
        return false;
    }

    email    = prefs.getString("email",    "");
    password = prefs.getString("password", "");
    prefs.end();

    if (email.isEmpty() || password.isEmpty()) {
        return false;   // sem credenciais — situação normal após flash erase
    }

    Serial.println("[firebase] Credenciais carregadas da NVS");
    return true;
}

void FirebaseHandler::receiveSetpoints(ActuatorController& actuators) {
    if (!authenticated) {
        Serial.println("User not authenticated. Cannot receive setpoints.");
        return;
    }

    String path = "/greenhouses/" + greenhouseId + "/setpoints";

    if (!Firebase.getJSON(fbdo, path.c_str())) {
        Serial.println("[setpoints] Erro ao ler setpoints: " + fbdo.errorReason());
        return;
    }

    FirebaseJson*    json = fbdo.jsonObjectPtr();
    FirebaseJsonData result;

    int fieldsRead = 0;
    bool hasLux = false, hasTMax = false, hasTMin = false, hasUMax = false, hasUMin = false;
    bool hasCoSp = false, hasCo2Sp = false, hasTvocsSp = false;
    int   parsedLux = 0, parsedCoSp = 0, parsedCo2Sp = 0, parsedTvocsSp = 0;
    float parsedTMax = 0.0f, parsedTMin = 0.0f, parsedUMax = 0.0f, parsedUMin = 0.0f;

    if (json->get(result, "lux"))     { parsedLux     = result.intValue;   hasLux = true; fieldsRead++; }
    if (json->get(result, "tMax"))    { parsedTMax    = result.floatValue; hasTMax = true; fieldsRead++; }
    if (json->get(result, "tMin"))    { parsedTMin    = result.floatValue; hasTMin = true; fieldsRead++; }
    if (json->get(result, "uMax"))    { parsedUMax    = result.floatValue; hasUMax = true; fieldsRead++; }
    if (json->get(result, "uMin"))    { parsedUMin    = result.floatValue; hasUMin = true; fieldsRead++; }
    if (json->get(result, "coSp"))    { parsedCoSp    = result.intValue;   hasCoSp = true; fieldsRead++; }
    if (json->get(result, "co2Sp"))   { parsedCo2Sp   = result.intValue;   hasCo2Sp = true; fieldsRead++; }
    if (json->get(result, "tvocsSp")) { parsedTvocsSp = result.intValue;   hasTvocsSp = true; fieldsRead++; }

    if (fieldsRead == 0) {
        Serial.println("[setpoints] WARN: Nenhum campo encontrado no no /setpoints.");
        return;
    }

    static bool baselineReady = false;
    static int   currentLux = 5000, currentCoSp = 50, currentCo2Sp = 400, currentTvocsSp = 100;
    static float currentTMax = 30.0f, currentTMin = 20.0f, currentUMax = 80.0f, currentUMin = 60.0f;

    if (!baselineReady) {
        if (fieldsRead < 8) {
            Serial.printf("[setpoints] WARN: payload parcial sem baseline (%d/8). Ignorado por seguranca.\n", fieldsRead);
            return;
        }
        baselineReady = true;
    } else if (fieldsRead < 8) {
        Serial.printf("[setpoints] WARN: %d campo(s) ausentes - mantendo ultimo valor conhecido.\n", 8 - fieldsRead);
    }

    int prevLux = currentLux, prevCoSp = currentCoSp, prevCo2Sp = currentCo2Sp, prevTvocsSp = currentTvocsSp;
    float prevTMax = currentTMax, prevTMin = currentTMin, prevUMax = currentUMax, prevUMin = currentUMin;

    if (hasLux)     currentLux = parsedLux;
    if (hasTMax)    currentTMax = parsedTMax;
    if (hasTMin)    currentTMin = parsedTMin;
    if (hasUMax)    currentUMax = parsedUMax;
    if (hasUMin)    currentUMin = parsedUMin;
    if (hasCoSp)    currentCoSp = parsedCoSp;
    if (hasCo2Sp)   currentCo2Sp = parsedCo2Sp;
    if (hasTvocsSp) currentTvocsSp = parsedTvocsSp;

    bool changed = false;
    if (currentLux     != prevLux)                    changed = true;
    if (currentCoSp    != prevCoSp)                   changed = true;
    if (currentCo2Sp   != prevCo2Sp)                  changed = true;
    if (currentTvocsSp != prevTvocsSp)                changed = true;
    if (fabsf(currentTMax - prevTMax) > 0.01f)        changed = true;
    if (fabsf(currentTMin - prevTMin) > 0.01f)        changed = true;
    if (fabsf(currentUMax - prevUMax) > 0.01f)        changed = true;
    if (fabsf(currentUMin - prevUMin) > 0.01f)        changed = true;

    if (!changed) {
        return;
    }

    Serial.printf("[setpoints] Alterados (%d/8 campos lidos): "
                  "Lux=%d, Temp=[%.1f-%.1f], Hum=[%.1f-%.1f], CO=%d, CO2=%d, TVOCs=%d\n",
                  fieldsRead, currentLux, currentTMin, currentTMax, currentUMin, currentUMax,
                  currentCoSp, currentCo2Sp, currentTvocsSp);

    actuators.applySetpoints(currentLux, currentTMin, currentTMax,
                             currentUMin, currentUMax,
                             currentCoSp, currentCo2Sp, currentTvocsSp);
}

bool FirebaseHandler::getDebugMode() {
    if (!authenticated || !Firebase.ready()) {
        return false;
    }

    String path = "/greenhouses/" + greenhouseId + "/debug_mode";
    if (Firebase.getBool(fbdo, path.c_str())) {
        return fbdo.boolData();
    }
    return false;
}

void FirebaseHandler::getManualActuatorStates(bool& relay1, bool& relay2, bool& relay3, bool& relay4, bool& ledsOn, int& ledsIntensity, bool& humidifierOn) {
    relay1 = false; relay2 = false; relay3 = false; relay4 = false;
    ledsOn = false; ledsIntensity = 0; humidifierOn = false;
    
    if (!authenticated || !Firebase.ready()) {
        return;
    }

    String path = "/greenhouses/" + greenhouseId + "/manual_actuators";
    if (Firebase.getJSON(fbdo, path.c_str())) {
        FirebaseJson *json = fbdo.jsonObjectPtr();
        FirebaseJsonData result;

        if (json->get(result, "rele1")) relay1 = result.boolValue;
        if (json->get(result, "rele2")) relay2 = result.boolValue;
        if (json->get(result, "rele3")) relay3 = result.boolValue;
        if (json->get(result, "rele4")) relay4 = result.boolValue;
        if (json->get(result, "leds/ligado")) ledsOn = result.boolValue;
        if (json->get(result, "leds/intensity")) ledsIntensity = result.intValue;
        if (json->get(result, "umidificador")) humidifierOn = result.boolValue;
        
        Serial.printf("[debug] Manual states - R1:%d R2:%d R3:%d R4:%d LED:%d(%d) HUM:%d\n",
                     relay1, relay2, relay3, relay4, ledsOn, ledsIntensity, humidifierOn);
    } else {
        Serial.println("[debug] Failed to read manual actuator states: " + fbdo.errorReason());
    }
}

void FirebaseHandler::getDevModeSettings(bool& analogRead, bool& digitalWriteMode, int& pin, bool& pwm, int& pwmValue) {
    analogRead = false; digitalWriteMode = false; pin = -1; pwm = false; pwmValue = 0;
    
    if (!authenticated || !Firebase.ready()) {
        return;
    }

    String path = "/greenhouses/" + greenhouseId + "/devmode";
    if (Firebase.getJSON(fbdo, path.c_str())) {
        FirebaseJson *json = fbdo.jsonObjectPtr();
        FirebaseJsonData result;

        if (json->get(result, "analogRead")) analogRead = result.boolValue;
        if (json->get(result, "boolean")) digitalWriteMode = result.boolValue;
        if (json->get(result, "pin")) pin = result.intValue;
        if (json->get(result, "pwm")) pwm = result.boolValue;
        if (json->get(result, "pwmValue")) pwmValue = result.intValue;
    } else {
        Serial.println("[debug] ERRO - Falha ao ler configurações: " + fbdo.errorReason());
    }
}

// =============================================================================
// AGENDADOR DE LEDs
// =============================================================================

void FirebaseHandler::receiveLEDSchedule(ActuatorController& actuators) {
    if (!authenticated || !Firebase.ready()) return;

    String path = "/greenhouses/" + greenhouseId + "/led_schedule";
    if (!Firebase.getJSON(fbdo, path.c_str())) {
        Serial.println("[led] Falha ao ler led_schedule: " + fbdo.errorReason());
        return;
    }

    FirebaseJson*    json   = fbdo.jsonObjectPtr();
    FirebaseJsonData result;

    if (json->get(result, "scheduleEnabled")) actuators.ledScheduler.scheduleEnabled = result.boolValue;
    if (json->get(result, "solarSimEnabled")) actuators.ledScheduler.solarSimEnabled = result.boolValue;
    if (json->get(result, "onHour"))          actuators.ledScheduler.onHour          = result.intValue;
    if (json->get(result, "onMinute"))        actuators.ledScheduler.onMinute        = result.intValue;
    if (json->get(result, "offHour"))         actuators.ledScheduler.offHour         = result.intValue;
    if (json->get(result, "offMinute"))       actuators.ledScheduler.offMinute       = result.intValue;
    if (json->get(result, "intensity"))       actuators.ledScheduler.configIntensity = result.intValue;
}

// =============================================================================
// OTA
// =============================================================================

void FirebaseHandler::ensureOTANodeExists() {
    if (!authenticated || !Firebase.ready()) return;

    String path = "/greenhouses/" + greenhouseId + "/ota/available";
    if (Firebase.getBool(fbdo, path.c_str())) {
        return;
    }

    Serial.println("[ota] Nó OTA não encontrado, criando estrutura...");

    String basePath = "/greenhouses/" + greenhouseId + "/ota";
    FirebaseJson otaNode;
    otaNode.set("available", false);
    otaNode.set("version",   "");
    otaNode.set("url",       "");
    otaNode.set("notes",     "Insira a URL HTTPS do .bin e mude available para true");

    if (Firebase.setJSON(fbdo, basePath.c_str(), otaNode)) {
        Serial.println("[ota] No OTA criado no RTDB");
    } else {
        Serial.println("[ota] Falha ao criar no OTA: " + fbdo.errorReason());
    }
}

// =============================================================================
// LED SCHEDULE — criação autônoma pelo ESP32
// =============================================================================

void FirebaseHandler::ensureLEDScheduleExists(ActuatorController& actuators) {
    if (!authenticated || !Firebase.ready()) return;

    String path = "/greenhouses/" + greenhouseId + "/led_schedule";

    if (Firebase.get(fbdo, (path + "/scheduleEnabled").c_str()) &&
        fbdo.dataType() != "null") {
        return;
    }

    Serial.println("[led] Nó led_schedule não encontrado, criando...");

    FirebaseJson ls;
    ls.set("scheduleEnabled", actuators.ledScheduler.scheduleEnabled);
    ls.set("solarSimEnabled", actuators.ledScheduler.solarSimEnabled);
    ls.set("onHour",          actuators.ledScheduler.onHour);
    ls.set("onMinute",        actuators.ledScheduler.onMinute);
    ls.set("offHour",         actuators.ledScheduler.offHour);
    ls.set("offMinute",       actuators.ledScheduler.offMinute);
    ls.set("intensity",       actuators.ledScheduler.configIntensity);

    if (Firebase.setJSON(fbdo, path.c_str(), ls)) {
        Serial.println("[led] No led_schedule criado pelo ESP32");
    } else {
        Serial.println("[led] Falha ao criar led_schedule: " + fbdo.errorReason());
    }
}

// =============================================================================
// MODOS DE OPERAÇÃO
// =============================================================================

void FirebaseHandler::ensureOperationModeExists(ActuatorController& actuators) {
    if (!authenticated || !Firebase.ready()) return;

    String path = "/greenhouses/" + greenhouseId + "/operation_mode/mode";

    if (Firebase.get(fbdo, path.c_str()) && fbdo.dataType() != "null") {
        return;
    }

    Serial.println("[mode] Nó operation_mode não encontrado, criando...");

    String base = "/greenhouses/" + greenhouseId + "/operation_mode";
    FirebaseJson opMode;
    opMode.set("mode",        operationModeToString(actuators.getOperationMode()));
    opMode.set("lastChanged", (int)getCurrentTimestamp());
    opMode.set("changedBy",   "esp32");

    if (Firebase.setJSON(fbdo, base.c_str(), opMode)) {
        Serial.println("[mode] No operation_mode criado pelo ESP32");
    } else {
        Serial.println("[mode] Falha ao criar operation_mode: " + fbdo.errorReason());
    }
}

void FirebaseHandler::receiveOperationMode(ActuatorController& actuators) {
    if (!authenticated || !Firebase.ready()) return;

    String path = "/greenhouses/" + greenhouseId + "/operation_mode/mode";

    if (!Firebase.getString(fbdo, path.c_str())) {
        ensureOperationModeExists(actuators);
        return;
    }

    String modeStr = fbdo.stringData();
    OperationMode newMode = operationModeFromString(modeStr);

    if (newMode != actuators.getOperationMode()) {
        Serial.printf("[mode] App solicitou mudanca: %s -> %s\n",
                      operationModeLabel(actuators.getOperationMode()).c_str(),
                      operationModeLabel(newMode).c_str());
        actuators.applyOperationMode(newMode);

        if (newMode != MODE_MANUAL) {
            ensureLEDScheduleExists(actuators);
            String schedPath = "/greenhouses/" + greenhouseId + "/led_schedule";
            FirebaseJson ls;
            ls.set("scheduleEnabled", actuators.ledScheduler.scheduleEnabled);
            ls.set("solarSimEnabled", actuators.ledScheduler.solarSimEnabled);
            ls.set("onHour",          actuators.ledScheduler.onHour);
            ls.set("onMinute",        actuators.ledScheduler.onMinute);
            ls.set("offHour",         actuators.ledScheduler.offHour);
            ls.set("offMinute",       actuators.ledScheduler.offMinute);
            ls.set("intensity",       actuators.ledScheduler.configIntensity);
            Firebase.updateNode(fbdo, schedPath.c_str(), ls);
        }
    }
}

void FirebaseHandler::publishOperationMode(OperationMode mode) {
    if (!authenticated || !Firebase.ready()) return;
    if (mode == _lastPublishedMode) return;

    String base = "/greenhouses/" + greenhouseId + "/operation_mode";
    FirebaseJson opMode;
    opMode.set("mode",        operationModeToString(mode));
    opMode.set("lastChanged", (int)getCurrentTimestamp());
    opMode.set("changedBy",   "esp32");

    if (Firebase.updateNode(fbdo, base.c_str(), opMode)) {
        _lastPublishedMode = mode;
        Serial.printf("[mode] Modo publicado: %s\n", operationModeLabel(mode).c_str());
    } else {
        Serial.println("[mode] Falha ao publicar modo: " + fbdo.errorReason());
    }
}

// =============================================================================
// AUTO-REPAIR DO BANCO FIREBASE
// =============================================================================

void FirebaseHandler::repairMissingFields() {
    if (!authenticated || !Firebase.ready()) return;

    String base  = "/greenhouses/" + greenhouseId;
    bool   dirty = false;
    FirebaseJson patch;

    if (!Firebase.get(fbdo, base.c_str()) || fbdo.dataType() == "null") {
        Serial.println("[firebase] greenhouse ausente no RTDB — recriando estrutura completa");
        createInitialGreenhouse(userUID, userUID);
        return;
    }

    // SETPOINTS — verificação do nó inteiro, NUNCA campo por campo.
    //
    // POR QUE campo-por-campo estava quebrando:
    //   Firebase.get(fbdo, "setpoints/tMax") usa o mesmo objeto fbdo que todas
    //   as outras operações. Se qualquer get() anterior falhou (timeout, resposta
    //   parcial, SSL reset), o fbdo fica em estado de erro e os gets seguintes
    //   retornam false / dataType=="null" — mesmo que o campo exista no banco.
    //   Isso fazia o repair sobrescrever tMax=23 (usuário) com tMax=30 (default)
    //   a cada ciclo de 5 minutos que sofresse qualquer instabilidade de rede.
    //
    // CORREÇÃO: uma única leitura do nó pai /setpoints. Se existir com algum dado
    //   → pertence ao usuário, não toca. Só cria se o nó inteiro estiver ausente.
    if (!Firebase.get(fbdo, (base + "/setpoints").c_str()) ||
        fbdo.dataType() == "null") {
        FirebaseJson sp;
        sp.set("lux",     5000);
        sp.set("tMax",    30.0f);
        sp.set("tMin",    20.0f);
        sp.set("uMax",    80.0f);
        sp.set("uMin",    60.0f);
        sp.set("coSp",    50);
        sp.set("co2Sp",   400);
        sp.set("tvocsSp", 100);
        patch.set("setpoints", sp);
        dirty = true;
        Serial.println("[repair] WARN: No /setpoints ausente — criando com defaults");
    }

    if (!Firebase.get(fbdo, (base + "/led_schedule").c_str()) ||
        fbdo.dataType() == "null") {
        FirebaseJson ls;
        ls.set("scheduleEnabled", false); ls.set("solarSimEnabled", false);
        ls.set("onHour", 6); ls.set("onMinute", 0);
        ls.set("offHour", 20); ls.set("offMinute", 0);
        ls.set("intensity", 255);
        patch.set("led_schedule", ls);
        dirty = true;
        Serial.println("[repair] WARN: Campo ausente: led_schedule");
    }

    if (!Firebase.get(fbdo, (base + "/ota").c_str()) ||
        fbdo.dataType() == "null") {
        FirebaseJson ota;
        ota.set("available", false); ota.set("version", "");
        ota.set("url", ""); ota.set("notes", "");
        patch.set("ota", ota);
        dirty = true;
        Serial.println("[repair] WARN: Campo ausente: ota");
    }

    if (!Firebase.get(fbdo, (base + "/operation_mode").c_str()) ||
        fbdo.dataType() == "null") {
        FirebaseJson opMode;
        opMode.set("mode", "manual");
        opMode.set("lastChanged", (int)getCurrentTimestamp());
        opMode.set("changedBy", "esp32");
        patch.set("operation_mode", opMode);
        dirty = true;
        Serial.println("[repair] WARN: Campo ausente: operation_mode");
    }

    if (!Firebase.get(fbdo, (base + "/debug_mode").c_str()) ||
        fbdo.dataType() == "null") {
        patch.set("debug_mode", false);
        dirty = true;
        Serial.println("[repair] WARN: Campo ausente: debug_mode");
    }

    if (!Firebase.get(fbdo, (base + "/sensor_status").c_str()) ||
        fbdo.dataType() == "null") {
        FirebaseJson ss;
        ss.set("dht22_sensorError", "OK");
        ss.set("ccs811_sensorError", "OK");
        ss.set("mq07_sensorError", "SensorError03");
        ss.set("ldr_sensorError", "OK");
        ss.set("waterlevel_sensorError", "OK");
        ss.set("lastUpdate", (int)getCurrentTimestamp());
        patch.set("sensor_status", ss);
        dirty = true;
        Serial.println("[repair] WARN: Campo ausente: sensor_status");
    } else {
        struct SensorStatusDefault {
            const char* key;
            const char* value;
        };
        const SensorStatusDefault ssDefaults[] = {
            {"sensor_status/dht22_sensorError", "OK"},
            {"sensor_status/ccs811_sensorError", "OK"},
            {"sensor_status/mq07_sensorError", "SensorError03"},
            {"sensor_status/ldr_sensorError", "OK"},
            {"sensor_status/waterlevel_sensorError", "OK"},
        };
        for (const auto& f : ssDefaults) {
            if (!Firebase.get(fbdo, (base + "/" + String(f.key)).c_str()) || fbdo.dataType() == "null") {
                patch.set(f.key, f.value);
                dirty = true;
            }
        }
        if (!Firebase.get(fbdo, (base + "/sensor_status/lastUpdate").c_str()) || fbdo.dataType() == "null") {
            patch.set("sensor_status/lastUpdate", (int)getCurrentTimestamp());
            dirty = true;
        }
    }

    if (dirty) {
        if (Firebase.updateNode(fbdo, base.c_str(), patch)) {
            Serial.println("[repair] Campos ausentes restaurados com sucesso");
        } else {
            Serial.println("[repair] Falha ao restaurar campos: " + fbdo.errorReason());
        }
    } else {
        Serial.println("[repair] Estrutura do banco integra");
    }
}

// =============================================================================
// LOGGER REMOTO
// =============================================================================

extern "C" unsigned long _rlog_getTimestamp() {
    extern FirebaseHandler firebase;
    return firebase.getCurrentTimestamp();
}

void FirebaseHandler::initLogger(LogLevel minLevel) {
    if (!authenticated || greenhouseId.isEmpty()) {
        Serial.println("[rlog] initLogger chamado antes de autenticar — ignorado");
        return;
    }
    RemoteLogger::init(&fbdo, greenhouseId, minLevel);
    ensureLogNodeExists();
    RLOG_INFO("[system]", "Logger remoto inicializado");
}

void FirebaseHandler::ensureLogNodeExists() {
    RemoteLogger::ensureNodeExists();
}