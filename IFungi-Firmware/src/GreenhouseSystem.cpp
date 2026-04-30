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
    // config.api_key e auth.user.*  precisam de ponteiros válidos durante toda
    // a sessão Firebase — não apenas durante a chamada de Firebase.begin().
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
    
    Firebase.begin(&config, &auth);
    
    // Aguarda autenticação com timeout de 30 segundos
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
            
            verifyGreenhouse();
            checkUserPermission(userUID, greenhouseId);
            return true;
        }
        delay(500);
        Serial.print(".");
    }
    
    Serial.println("\n[firebase] ERRO - Autenticação expirada (timeout)");
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
        static unsigned long millisOffset = 0;
        if (millisOffset == 0) {
            if (initializeNVS() && preferences.begin(NAMESPACE, true)) {
                millisOffset = preferences.getULong("ultimo_timestamp", 0) - (millis() / 1000);
                preferences.end();
            }
        }
        return (millis() / 1000) + millisOffset;
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
    
    if (!preferences.begin(NAMESPACE, false)) {
        Serial.println("Failed to open Preferences for writing");
        return;
    }
    
    int numRecords = preferences.getInt("num_registros", 0);
    
    // Cada registro é serializado como um único JSON numa chave "reg_N".
    // 50 registros = 50 entradas + metadados → bem dentro do limite NVS.
    if (numRecords >= MAX_RECORDS) {
        // Descarta o registro mais antigo (índice 0) deslocando todos
        for (int i = 1; i < MAX_RECORDS; i++) {
            String src = preferences.getString(("reg_" + String(i)).c_str(), "");
            if (src.length() > 0) {
                preferences.putString(("reg_" + String(i - 1)).c_str(), src.c_str());
            }
        }
        numRecords = MAX_RECORDS - 1;
        preferences.remove(("reg_" + String(numRecords)).c_str());
    }
    
    // Chaves abreviadas para economizar espaço na NVS (limite de 64KB total)
    String record = "{\"t\":"   + String(temp, 1)     +
                    ",\"h\":"   + String(humidity, 1)  +
                    ",\"c2\":" + String(co2)           +
                    ",\"co\":" + String(co)            +
                    ",\"l\":"   + String(lux)           +
                    ",\"v\":"   + String(tvocs)         +
                    ",\"ts\":" + String(timestamp)     + "}";

    preferences.putString(("reg_" + String(numRecords)).c_str(), record.c_str());
    preferences.putInt("num_registros", numRecords + 1);
    preferences.putULong("ultimo_timestamp", timestamp);
    
    preferences.end();
    Serial.printf("[nvs] Registro %d salvo localmente (%d bytes)\n", numRecords, record.length());
}


bool FirebaseHandler::initializeNVS() {
    if (nvsInitialized) return true;
    
    if (!preferences.begin(NAMESPACE, true)) {
        Serial.println("Failed to open NVS namespace");
        return false;
    }
    
    size_t freeEntries = preferences.freeEntries();
    Serial.println("Free space in NVS: " + String(freeEntries));
    
    if (preferences.getInt("nvs_inicializada", 0) == 0) {
        Serial.println("NVS not initialized, setting default values...");
        
        preferences.end();
        if (!preferences.begin(NAMESPACE, false)) {
            Serial.println("Failed to open NVS for writing");
            return false;
        }
        
        preferences.putInt("num_registros", 0);
        preferences.putInt("nvs_inicializada", 1);
        
        Serial.println("NVS initialized successfully");
    }
    
    preferences.end();
    nvsInitialized = true;
    return true;
}

void FirebaseHandler::sendLocalData() {
    if (!initializeNVS()) {
        Serial.println("Could not send local data - NVS not available");
        return;
    }
    
    if (!preferences.begin(NAMESPACE, true)) {
        Serial.println("Failed to open Preferences for reading");
        return;
    }
    
    int numRecords = preferences.getInt("num_registros", 0);
    Serial.println("[nvs] Tentando enviar " + String(numRecords) + " registros locais");
    preferences.end();

    if (numRecords == 0) return;

    if (!preferences.begin(NAMESPACE, false)) {
        Serial.println("Failed to open Preferences for writing");
        return;
    }
    
    int sent = 0;
    // CORREÇÃO: marca quais índices foram enviados para recompactação posterior.
    // O loop anterior removia chaves inline enquanto iterava, o que causava saltos
    // de índice e perda de registros quando havia corrompidos intercalados.
    bool sentFlags[MAX_RECORDS] = {};

    for (int i = 0; i < numRecords; i++) {
        String keyName = "reg_" + String(i);
        String record  = preferences.getString(keyName.c_str(), "");

        if (record.length() == 0) {
            sentFlags[i] = true; // lacuna — descarta
            continue;
        }

        StaticJsonDocument<128> doc;
        DeserializationError err = deserializeJson(doc, record);
        if (err) {
            Serial.printf("[nvs] Registro %d corrompido, descartando: %s\n", i, err.c_str());
            sentFlags[i] = true; // descarta corrompido
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
    
    // CORREÇÃO: recompacta removendo apenas os registros marcados como enviados/descartados.
    // Garante que num_registros reflita exatamente os registros restantes válidos,
    // mesmo que haja corrompidos ou lacunas no meio da sequência.
    int newIndex = 0;
    for (int i = 0; i < numRecords; i++) {
        if (sentFlags[i]) {
            preferences.remove(("reg_" + String(i)).c_str());
            continue;
        }
        String record = preferences.getString(("reg_" + String(i)).c_str(), "");
        if (record.length() > 0) {
            if (i != newIndex) {
                preferences.putString(("reg_" + String(newIndex)).c_str(), record.c_str());
                preferences.remove(("reg_" + String(i)).c_str());
            }
            newIndex++;
        }
    }
    
    preferences.putInt("num_registros", newIndex);
    preferences.end();
    
    Serial.printf("[nvs] Envio concluído: %d enviados, %d restantes.\n", sent, newIndex);
}

void FirebaseHandler::sendSensorData(float temp, float humidity, int co2, int co, int lux, int tvocs, bool waterLevel) {
    refreshTokenIfNeeded();
    if (!authenticated || !Firebase.ready()) {
        Serial.println("[firebase] ERRO - Não autenticado para enviar dados");
        return;
    }

    FirebaseJson json;
    json.set("sensores/temperatura", temp);
    json.set("sensores/umidade", humidity);
    json.set("sensores/co2", co2);
    json.set("sensores/co", co);
    json.set("sensores/tvocs", tvocs);
    json.set("sensores/luminosidade", lux);
    json.set("lastUpdate", (int)getCurrentTimestamp());
    json.set("niveis/agua", waterLevel);

    String path = "/greenhouses/" + greenhouseId;
    if (Firebase.updateNode(fbdo, path.c_str(), json)) {
        Serial.println("[firebase] Dados dos sensores enviados com sucesso");
    } else {
        Serial.println("[firebase] ERRO - Falha ao enviar dados: " + fbdo.errorReason());
    }

    // Histórico: único caminho em MainController::handleHistoryAndLocalData() → sendDataToHistory()
    // (evita duplicata no RTDB; NVS/offline inalterados — saveDataLocal ainda via sendDataToHistory quando offline)
}

void FirebaseHandler::updateSensorHealth(bool dhtOk, bool ccsOk, bool mq7Ok, bool ldrOk, bool waterOk) {
    if (!authenticated || !Firebase.ready()) return;

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
    }
}

// =============================================================================
// CONTROLE DE PERMISSÕES — SUPORTE A MÚLTIPLAS ESTUFAS POR USUÁRIO
// =============================================================================

/**
 * @brief Verifica e concede permissão ao usuário para acessar a estufa
 *
 * @details A estrutura no Firebase usa um ARRAY JSON de IDs de estufas:
 *
 *   /Usuarios/<UID>/Estufas permitidas: ["IFUNGI-AA:BB:CC", "IFUNGI-DD:EE:FF"]
 *
 * Isso permite que um único usuário gerencie múltiplas estufas no app
 * sem sobrescrever as permissões existentes ao adicionar uma nova.
 *
 * MIGRAÇÃO AUTOMÁTICA: se o campo ainda estiver no formato antigo (string),
 * ele é migrado automaticamente para array na primeira chamada desta função.
 * Isso garante retrocompatibilidade com instalações anteriores.
 *
 * SEGURANÇA: esta função apenas verifica/adiciona permissão para a estufa
 * identificada pelo MAC address do próprio ESP32. Não é possível adicionar
 * permissão para outra estufa a partir deste código — o ID é gerado
 * deterministicamente a partir do hardware.
 *
 * @param userUID UID do usuário autenticado no Firebase
 * @param greenhouseID ID da estufa a verificar/adicionar
 * @return true se o usuário tem ou recebeu permissão
 */
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

    // ── Lê o campo atual de permissões ────────────────────────────────────────
    if (Firebase.get(fbdo, permissionsPath.c_str())) {
        String dataType = fbdo.dataType();

        // ── Caso 1: campo é um ARRAY (formato atual) ──────────────────────────
        if (dataType == "json" || dataType == "array") {
            // Verifica se a estufa já está no array
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
                // Estufa não encontrada — adiciona ao array existente
                arr->add(greenhouseID);
                if (writePermissionsArray(*arr)) {
                    Serial.println("Greenhouse added to existing permission list: " + greenhouseID);
                    return true;
                }
            }
        }

        // ── Caso 2: campo é uma STRING (formato legado — migra para array) ────
        else if (dataType == "string") {
            String existing = fbdo.stringData();
            existing.trim();
            Serial.println("[permission] Formato antigo detectado, migrando para array...");

            FirebaseJsonArray newArray;
            if (existing.length() > 0) {
                newArray.add(existing); // preserva a estufa já existente
            }

            // Adiciona a estufa atual se não for duplicata
            bool alreadyIn = (existing == greenhouseID);
            if (!alreadyIn) {
                newArray.add(greenhouseID);
            }

            // Substitui a string pelo array no Firebase
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

    // ── Campo não existe: cria o array com esta estufa ────────────────────────
    Serial.println("[permission] Criando permissão de acesso para: " + greenhouseID);
    FirebaseJsonArray newArray;
    newArray.add(greenhouseID);
    if (writePermissionsArray(newArray)) {
        Serial.println("[permission] Permissao criada com sucesso");
        return true;
    }

    // Fallback: tenta criar o nó do usuário inteiro
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
    sensorStatus.set("mq07_sensorError", "SensorError03"); // warmup inicial
    sensorStatus.set("ldr_sensorError", "OK");
    sensorStatus.set("waterlevel_sensorError", "OK");
    sensorStatus.set("lastUpdate", (int)getCurrentTimestamp());
    json.set("sensor_status", sensorStatus);

    FirebaseJson setpoints;
    setpoints.set("lux", 5000);
    setpoints.set("tMax", 30.0);
    setpoints.set("tMin", 20.0);
    setpoints.set("uMax", 80.0);
    setpoints.set("uMin", 60.0);
    setpoints.set("coSp", 50);
    setpoints.set("co2Sp", 400);
    setpoints.set("tvocsSp", 100);
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

    FirebaseJson ledSched;
    ledSched.set("scheduleEnabled", false);
    ledSched.set("solarSimEnabled", false);
    ledSched.set("onHour",    6);
    ledSched.set("onMinute",  0);
    ledSched.set("offHour",  20);
    ledSched.set("offMinute", 0);
    ledSched.set("intensity", 255);
    json.set("led_schedule", ledSched);

    FirebaseJson opMode;
    opMode.set("mode",        "manual");
    opMode.set("lastChanged", (int)getCurrentTimestamp());
    opMode.set("changedBy",   "esp32");
    json.set("operation_mode", opMode);

    FirebaseJson status;
    status.set("online", true);
    status.set("lastHeartbeat", (int)getCurrentTimestamp());
    status.set("ip", WiFi.localIP().toString());
    json.set("status", status);

    // Nó OTA com campos iniciais — URL preenchida externamente pelo desenvolvedor
    FirebaseJson otaNode;
    otaNode.set("available", false);
    otaNode.set("version",   "");
    otaNode.set("url",       "");
    otaNode.set("notes",     "Insira a URL HTTPS do .bin e mude available para true");
    json.set("ota", otaNode);

    String path = "/greenhouses/" + greenhouseId;
    if (Firebase.setJSON(fbdo, path.c_str(), json)) {
        Serial.println("[firebase] Greenhouse created successfully with complete structure");
        // CORREÇÃO: checkUserPermission chamado apenas aqui, não duplicado em verifyGreenhouse.
        // O fluxo correto é: authenticate() → verifyGreenhouse() → createInitialGreenhouse()
        // → checkUserPermission(). Chamar antes de criar causava race condition.
        checkUserPermission(userUID, greenhouseId);
    } else {
        Serial.print("[firebase] Error creating greenhouse: ");
        Serial.println(fbdo.errorReason());
        
        if (fbdo.errorReason().indexOf("token") >= 0) {
            Serial.println("[firebase] Invalid token, trying to renew...");
            Firebase.refreshToken(&config);
            delay(1000);
            if (Firebase.ready() && Firebase.setJSON(fbdo, path.c_str(), json)) {
                Serial.println("[firebase] Greenhouse created after renewing token");
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
            
            // CORREÇÃO: isGreenhouseStructureComplete() já chama updateNode() internamente
            // para adicionar campos faltantes. Se retornar false, a estufa está tão
            // corrompida que precisa ser recriada — mas NÃO chamamos createInitialGreenhouse()
            // aqui para evitar sobrescrever dados do usuário desnecessariamente.
            // A função repair já cobre campos individuais faltantes.
            if (!isGreenhouseStructureComplete(greenhouseId)) {
                Serial.println("Greenhouse structure incomplete after repair attempt.");
                // Tenta recriar apenas se a estrutura está completamente inválida
                // (isGreenhouseStructureComplete() já tentou reparar — se falhou é crítico)
                createInitialGreenhouse(userUID, userUID);
            } else {
                Serial.println("Greenhouse structure is complete.");
            }
            return true;
        }
    }
    
    // Estufa não existe — createInitialGreenhouse será chamada pelo caller (verifyGreenhouse)
    Serial.println("Greenhouse not found.");
    return false;
}

bool FirebaseHandler::isGreenhouseStructureComplete(const String& greenhouseId) {
    if (!authenticated || !Firebase.ready()) {
        return false;
    }

    String path = "/greenhouses/" + greenhouseId;
    
    // Lista de campos obrigatórios
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
        "setpoints",
        "setpoints/lux",
        "setpoints/tMax",
        "setpoints/tMin",
        "setpoints/uMax",
        "setpoints/uMin",
        "setpoints/coSp",
        "setpoints/co2Sp",
        "setpoints/tvocsSp",
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
        else if (field == "setpoints") {
            FirebaseJson sp;
            sp.set("lux", 5000); sp.set("tMax", 30.0f); sp.set("tMin", 20.0f);
            sp.set("uMax", 80.0f); sp.set("uMin", 60.0f);
            sp.set("coSp", 50); sp.set("co2Sp", 400); sp.set("tvocsSp", 100);
            updateJson.set("setpoints", sp);
        } else if (field == "setpoints/lux") updateJson.set(field, 5000);
        else if (field == "setpoints/tMax") updateJson.set(field, 30.0f);
        else if (field == "setpoints/tMin") updateJson.set(field, 20.0f);
        else if (field == "setpoints/uMax") updateJson.set(field, 80.0f);
        else if (field == "setpoints/uMin") updateJson.set(field, 60.0f);
        else if (field == "setpoints/coSp") updateJson.set(field, 50);
        else if (field == "setpoints/co2Sp") updateJson.set(field, 400);
        else if (field == "setpoints/tvocsSp") updateJson.set(field, 100);
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

    // Verifica campos obrigatórios
    for (int i = 0; i < numFields; i++) {
        if (!jsonPtr->get(result, requiredFields[i]) ||
            result.typeNum == FirebaseJson::JSON_NULL) {
            String missing = String(requiredFields[i]);
            Serial.println("[firebase] Missing/null field: " + missing);
            needsUpdate = true;
            addDefaultForField(missing);
        }
    }

    // Verifica e cria campos opcionais de debug/dev
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
            return true; // reparou com sucesso
        } else {
            Serial.println("[repair] Failed to repair: " + fbdo.errorReason());
            return false; // falha no repair → caller decide se recria
        }
    }
    
    Serial.println("[repair] All required fields are present and valid");
    return true;
}

bool FirebaseHandler::loadFirebaseCredentials(String& email, String& password) {
    Preferences preferences;
    if(!preferences.begin("firebase-creds", true)) {
        Serial.println("[error] Failed to open preferences");
        return false;
    }
    
    email = preferences.getString("email", "");
    password = preferences.getString("password", "");
    preferences.end();
    
    if(email.isEmpty() || password.isEmpty()) {
        Serial.println("No Firebase credentials found");
        return false;
    }
    
    Serial.println("Firebase credentials loaded from NVS");
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

    // Snapshot local de setpoints aplicados para evitar regressao por payload parcial.
    // Regra de seguranca:
    // - Antes de ter baseline completo (8 campos), ignoramos payload parcial.
    // - Depois do baseline, payload parcial atualiza apenas os campos presentes.
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
        // Firebase tem os mesmos valores — não chama applySetpoints, não grava NVS
        return;
    }

    // Valores mudaram: aplica e atualiza o cache
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

void FirebaseHandler::getDevModeSettings(bool& analogRead, bool& digitalWrite, int& pin, bool& pwm, int& pwmValue) {
    analogRead = false; digitalWrite = false; pin = -1; pwm = false; pwmValue = 0;
    
    if (!authenticated || !Firebase.ready()) {
        return;
    }

    String path = "/greenhouses/" + greenhouseId + "/devmode";
    if (Firebase.getJSON(fbdo, path.c_str())) {
        FirebaseJson *json = fbdo.jsonObjectPtr();
        FirebaseJsonData result;

        if (json->get(result, "analogRead")) analogRead = result.boolValue;
        if (json->get(result, "boolean")) digitalWrite = result.boolValue;
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
        return; // Nó já existe
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

    struct { const char* key; float defVal; bool isFloat; } spFields[] = {
        {"setpoints/lux",     5000, false},
        {"setpoints/tMax",    30.0, true },
        {"setpoints/tMin",    20.0, true },
        {"setpoints/uMax",    80.0, true },
        {"setpoints/uMin",    60.0, true },
        {"setpoints/coSp",   50,    false},
        {"setpoints/co2Sp",  400,   false},
        {"setpoints/tvocsSp",100,   false},
    };
    for (auto& f : spFields) {
        if (!Firebase.get(fbdo, (base + "/" + f.key).c_str()) ||
            fbdo.dataType() == "null") {
            if (f.isFloat) patch.set(f.key, f.defVal);
            else            patch.set(f.key, (int)f.defVal);
            dirty = true;
            Serial.printf("[repair] WARN: Campo ausente: %s\n", f.key);
        }
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
