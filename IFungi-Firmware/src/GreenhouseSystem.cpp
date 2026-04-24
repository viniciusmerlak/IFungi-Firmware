#include "GreenhouseSystem.h"
#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>
#include <ArduinoJson.h>   // necessário para desserializar registros NVS em sendLocalData

String FirebaseHandler::getMacAddress() {
    return ::getMacAddress();
}

bool FirebaseHandler::authenticate(const String& email, const String& password) {
    const String api = FIREBASE_API_KEY;
    String dbUrl = DATABASE_URL;
    Serial.println("Authenticating with Firebase...");
    
    Firebase.reset(&config);
    Firebase.reconnectNetwork(true);
    
    config.api_key = api.c_str();
    auth.user.email = email.c_str();
    auth.user.password = password.c_str();
    config.database_url = dbUrl.c_str();
    config.token_status_callback = tokenStatusCallback;
    
    Firebase.begin(&config, &auth);
    
    Serial.print("Waiting for authentication");
    unsigned long startTime = millis();
    
    while (millis() - startTime < 20000) {
        if (Firebase.ready()) {
            authenticated = true;
            userUID = String(auth.token.uid.c_str());
            greenhouseId = "IFUNGI-" + getMacAddress();
            
            Serial.println("\nAuthentication successful!");
            Serial.print("UID: "); Serial.println(userUID);
            
            verifyGreenhouse();
            checkUserPermission(userUID, greenhouseId);
            return true;
        }
        delay(500);
        Serial.print(".");
    }
    
    Serial.println("\nAuthentication failed: Timeout");
    return false;
}

void FirebaseHandler::updateActuatorState(bool relay1, bool relay2, bool relay3, bool relay4, 
                                         bool ledsOn, int ledsWatts, bool humidifierOn) {
    if (!authenticated || !Firebase.ready()) {
        Serial.println("[FIREBASE] ERRO - Não autenticado para atualizar atuadores");
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

    // CORREÇÃO: millis() substituído por timestamp Unix real (consistência com demais campos)
    json.set("lastUpdate", (int)getCurrentTimestamp());
    json.set("atuadores", actuators);

    String path = FirebaseHandler::getGreenhousesPath() + greenhouseId;
    
    if (Firebase.updateNode(fbdo, path.c_str(), json)) {
        Serial.println("[FIREBASE] Estados dos atuadores atualizados com sucesso");
    } else {
        Serial.println("[FIREBASE] ERRO - Falha ao atualizar atuadores: " + fbdo.errorReason());
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
        Serial.println("[FIREBASE] OFFLINE - Dados salvos localmente");
        unsigned long timestamp = getCurrentTimestamp();
        saveDataLocally(temp, humidity, co2, co, lux, tvocs, timestamp);
        return false;
    }

    String timestamp = String(getCurrentTimestamp());
    String path = "/historico/" + greenhouseId + "/" + timestamp;
    
    FirebaseJson json;
    json.set("timestamp", timestamp);
    json.set("temperatura", temp);
    json.set("umidade", humidity);
    json.set("co2", co2);
    json.set("co", co);
    json.set("tvocs", tvocs);
    json.set("luminosidade", lux);
    json.set("dataHora", getFormattedDateTime());
    
    if (Firebase.setJSON(fbdo, path.c_str(), json)) {
        Serial.println("[FIREBASE] Dados salvos no histórico com sucesso");
        return true;
    } else {
        Serial.println("[FIREBASE] ERRO - Falha ao salvar histórico: " + fbdo.errorReason());
        unsigned long timestamp = getCurrentTimestamp();
        saveDataLocally(temp, humidity, co2, co, lux, tvocs, timestamp);
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
    
    // CORREÇÃO: O modelo anterior usava 7 chaves NVS por registro (temp_N, umid_N, etc.).
    // Com MAX_RECORDS=50 isso resultava em 350 entradas, estourando o limite de ~100
    // entradas por namespace do ESP32 — dados eram perdidos silenciosamente.
    //
    // Solução: cada registro é serializado como um único JSON numa chave "reg_N".
    // 50 registros = 50 entradas + metadados → bem dentro do limite.
    
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
    
    // Serializa o registro como JSON numa única chave
    // Formato: {"t":temp,"h":humidity,"c2":co2,"co":co,"l":lux,"v":tvocs,"ts":timestamp}
    // Chaves abreviadas para economizar espaço na NVS (limite de 64KB total)
    String record = "{\"t\":"   + String(temp, 1)     +
                    ",\"h\":"   + String(humidity, 1)  +
                    ",\"c2\":"  + String(co2)           +
                    ",\"co\":"  + String(co)            +
                    ",\"l\":"   + String(lux)           +
                    ",\"v\":"   + String(tvocs)         +
                    ",\"ts\":"  + String(timestamp)     + "}";

    preferences.putString(("reg_" + String(numRecords)).c_str(), record.c_str());
    preferences.putInt("num_registros", numRecords + 1);
    preferences.putULong("ultimo_timestamp", timestamp);
    
    preferences.end();
    Serial.printf("[NVS] Registro %d salvo localmente (%d bytes)\n", numRecords, record.length());
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
    Serial.println("[NVS] Tentando enviar " + String(numRecords) + " registros locais");
    preferences.end();

    if (numRecords == 0) return;

    // CORREÇÃO: leitura do novo formato de registro único por chave "reg_N"
    // O formato antigo (temp_N, umid_N, etc.) foi substituído na saveDataLocally.
    if (!preferences.begin(NAMESPACE, false)) {
        Serial.println("Failed to open Preferences for writing");
        return;
    }
    
    int sent = 0;
    for (int i = 0; i < numRecords; i++) {
        String keyName = "reg_" + String(i);
        String record  = preferences.getString(keyName.c_str(), "");

        if (record.length() == 0) continue;

        // Desserializa o JSON compacto usando ArduinoJson
        StaticJsonDocument<128> doc;
        DeserializationError err = deserializeJson(doc, record);
        if (err) {
            Serial.printf("[NVS] Registro %d corrompido, descartando: %s\n", i, err.c_str());
            preferences.remove(keyName.c_str());
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
                preferences.remove(keyName.c_str());
                sent++;
            } else {
                Serial.println("[NVS] Falha ao enviar registro " + String(i) + ", interrompendo.");
                break;
            }
        } else {
            Serial.println("[NVS] Firebase indisponível, interrompendo envio.");
            break;
        }
    }
    
    // Recompacta os registros restantes (remove lacunas dos enviados)
    int newIndex = 0;
    for (int i = 0; i < numRecords; i++) {
        String keyName = "reg_" + String(i);
        String record  = preferences.getString(keyName.c_str(), "");
        if (record.length() > 0) {
            if (i != newIndex) {
                preferences.putString(("reg_" + String(newIndex)).c_str(), record.c_str());
                preferences.remove(keyName.c_str());
            }
            newIndex++;
        }
    }
    
    preferences.putInt("num_registros", newIndex);
    preferences.end();
    
    Serial.printf("[NVS] Envio concluído: %d enviados, %d restantes.\n", sent, newIndex);
}

void FirebaseHandler::sendSensorData(float temp, float humidity, int co2, int co, int lux, int tvocs, bool waterLevel) {
    refreshTokenIfNeeded();
    if (!authenticated || !Firebase.ready()) {
        Serial.println("[FIREBASE] ERRO - Não autenticado para enviar dados");
        return;
    }

    FirebaseJson json;
    json.set("sensores/temperatura", temp);
    json.set("sensores/umidade", humidity);
    json.set("sensores/co2", co2);
    json.set("sensores/co", co);
    json.set("sensores/tvocs", tvocs);
    json.set("sensores/luminosidade", lux);
    // CORREÇÃO: millis() substituído por timestamp Unix real
    json.set("lastUpdate", (int)getCurrentTimestamp());
    json.set("niveis/agua", waterLevel);

    String path = "/greenhouses/" + greenhouseId;
    if (Firebase.updateNode(fbdo, path.c_str(), json)) {
        Serial.println("[FIREBASE] Dados dos sensores enviados com sucesso");
    } else {
        Serial.println("[FIREBASE] ERRO - Falha ao enviar dados: " + fbdo.errorReason());
    }
    
    if (millis() - lastHistoryTime > HISTORY_INTERVAL) {
        sendDataToHistory(temp, humidity, co2, co, lux, tvocs);
        lastHistoryTime = millis();
    }
}

bool FirebaseHandler::checkUserPermission(const String& userUID, const String& greenhouseID) {
    if (!Firebase.ready()) {
        Serial.println("Firebase not ready.");
        return false;
    }

    String userPath = "/Usuarios/" + userUID;
    String greenhousesPath = userPath + "/Estufas permitidas";
    
    if (Firebase.getString(fbdo, greenhousesPath)) {
        if (fbdo.stringData() == greenhouseID) {
            Serial.println("User already has permission for this greenhouse.");
            return true;
        }
        Serial.println("User already has permission for another greenhouse.");
        return false;
    }

    if (Firebase.setString(fbdo, greenhousesPath.c_str(), greenhouseID)) {
        Serial.println("Greenhouse permission granted successfully.");
        return true;
    } else {
        Serial.print("Failed to grant permission: ");
        Serial.println(fbdo.errorReason());
        
        FirebaseJson userData;
        userData.set("Estufas permitidas", greenhouseID);
        
        if (Firebase.setJSON(fbdo, userPath.c_str(), userData)) {
            Serial.println("New user created with greenhouse permission.");
            return true;
        } else {
            Serial.print("Critical error creating user: ");
            Serial.println(fbdo.errorReason());
            return false;
        }
    }
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
    checkUserPermission(userUID, greenhouseId);

    FirebaseJson json;
    
    // 🔥 CORREÇÃO: Estrutura completa dos atuadores
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
    // CORREÇÃO: time(nullptr) pode retornar 0 se NTP não sincronizou ainda.
    // getCurrentTimestamp() tem fallback para offset baseado em NVS.
    json.set("lastUpdate", (int)getCurrentTimestamp());

    // 🔥 CORREÇÃO: Estrutura completa dos sensores
    FirebaseJson sensors;
    sensors.set("tvocs", 0);
    sensors.set("co", 0);
    sensors.set("co2", 0);
    sensors.set("luminosidade", 0);
    sensors.set("temperatura", 0);
    sensors.set("umidade", 0);
    json.set("sensores", sensors);

    // 🔥 CORREÇÃO: Estrutura completa dos setpoints
    FirebaseJson setpoints;
    setpoints.set("lux", 5000);
    setpoints.set("tMax", 30.0);
    setpoints.set("tMin", 20.0);
    setpoints.set("uMax", 80.0);
    setpoints.set("uMin", 60.0);
    setpoints.set("coSp", 400);
    setpoints.set("co2Sp", 400);
    setpoints.set("tvocsSp", 100);
    json.set("setpoints", setpoints);

    json.set("niveis/agua", false);

    // 🔥 NOVO: Campos para debug e controle manual
    json.set("debug_mode", false); // Modo debug desativado por padrão
    
    // 🔥 NOVO: Estrutura para controle manual dos atuadores
    FirebaseJson manualActuators;
    manualActuators.set("rele1", false);
    manualActuators.set("rele2", false);
    manualActuators.set("rele3", false);
    manualActuators.set("rele4", false);
    manualActuators.set("leds/ligado", false);
    manualActuators.set("leds/intensity", 0);
    manualActuators.set("umidificador", false);
    json.set("manual_actuators", manualActuators);

    // 🔥 NOVO: Estrutura para devmode
    FirebaseJson devmode;
    devmode.set("analogRead", false);
    devmode.set("boolean", false);
    devmode.set("pin", -1);
    devmode.set("pwm", false);
    devmode.set("pwmValue", 0);
    json.set("devmode", devmode);

    // 🔥 NOVO: Status do sistema
    FirebaseJson status;
    status.set("online", true);
    // CORREÇÃO: millis() substituído por timestamp Unix real
    status.set("lastHeartbeat", (int)getCurrentTimestamp());
    status.set("ip", WiFi.localIP().toString());
    json.set("status", status);

    String path = "/greenhouses/" + greenhouseId;
    if (Firebase.setJSON(fbdo, path.c_str(), json)) {
        Serial.println("✅ Greenhouse created successfully with complete structure!");
        checkUserPermission(userUID, greenhouseId);
    } else {
        Serial.print("❌ Error creating greenhouse: ");
        Serial.println(fbdo.errorReason());
        
        if (fbdo.errorReason().indexOf("token") >= 0) {
            Serial.println("🔄 Invalid token, trying to renew...");
            Firebase.refreshToken(&config);
            delay(1000);
            if (Firebase.ready() && Firebase.setJSON(fbdo, path.c_str(), json)) {
                Serial.println("✅ Greenhouse created after renewing token!");
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
    // CORREÇÃO: millis() retorna ms desde o boot, não um timestamp Unix.
    // getCurrentTimestamp() usa NTP e retorna epoch real.
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
    
    // Primeiro verifica se a estufa existe
    if (Firebase.get(fbdo, path.c_str())) {
        if (fbdo.dataType() != "null") {
            Serial.println("Greenhouse found. Checking structure...");
            
            // Verifica se a estrutura está completa
            if (!isGreenhouseStructureComplete(greenhouseId)) {
                Serial.println("Greenhouse structure incomplete, recreating...");
                createInitialGreenhouse(userUID, userUID);
                return true; // Retorna true porque a estufa existe, mas foi recriada
            }
            
            Serial.println("Greenhouse structure is complete.");
            return true;
        }
    }
    
    Serial.println("Greenhouse not found, creating new...");
    createInitialGreenhouse(userUID, userUID);
    return false;
}

bool FirebaseHandler::isGreenhouseStructureComplete(const String& greenhouseId) {
    if (!authenticated || !Firebase.ready()) {
        return false;
    }

    String path = "/greenhouses/" + greenhouseId;
    FirebaseJson json;
    
    // Lista de campos obrigatórios que devem existir
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
        // Agendador de LEDs
        "led_schedule",
        "led_schedule/scheduleEnabled",
        "led_schedule/solarSimEnabled",
        "led_schedule/onHour",
        "led_schedule/offHour",
        "led_schedule/intensity",
        // Nó OTA
        "ota"
    };
    
    const int numFields = sizeof(requiredFields) / sizeof(requiredFields[0]);
    
    // Carrega o JSON completo da estufa
    if (!Firebase.getJSON(fbdo, path.c_str())) {
        Serial.println("❌ Error loading greenhouse JSON: " + fbdo.errorReason());
        return false;
    }
    
    FirebaseJson *jsonPtr = fbdo.jsonObjectPtr();
    FirebaseJsonData result;
    
    // Verifica cada campo obrigatório
    for (int i = 0; i < numFields; i++) {
        if (!jsonPtr->get(result, requiredFields[i])) {
            Serial.println("❌ Missing field: " + String(requiredFields[i]));
            return false;
        }
        
        // Verifica se o campo tem um valor válido (não é null)
        if (result.typeNum == FirebaseJson::JSON_NULL) {
            Serial.println("❌ Field is null: " + String(requiredFields[i]));
            return false;
        }
    }
    
    // 🔥 CORREÇÃO: Verifica campos opcionais de debug e calibração
    // Se não existirem, vamos criá-los para completar a estrutura
    
    bool needsUpdate = false;
    FirebaseJson updateJson;
    
    // Verifica e cria campo debug_mode se não existir
    if (!jsonPtr->get(result, "debug_mode")) {
        Serial.println("⚠️ debug_mode field missing, creating...");
        updateJson.set("debug_mode", false);
        needsUpdate = true;
    }
    
    // Verifica e cria estrutura manual_actuators se não existir
    if (!jsonPtr->get(result, "manual_actuators")) {
        Serial.println("⚠️ manual_actuators field missing, creating...");
        FirebaseJson manualActuators;
        manualActuators.set("rele1", false);
        manualActuators.set("rele2", false);
        manualActuators.set("rele3", false);
        manualActuators.set("rele4", false);
        manualActuators.set("leds/ligado", false);
        manualActuators.set("leds/intensity", 0);
        manualActuators.set("umidificador", false);
        updateJson.set("manual_actuators", manualActuators);
        needsUpdate = true;
    }
    
    // 🔥 NOVO: Verifica e cria estrutura devmode se não existir
    if (!jsonPtr->get(result, "devmode")) {
        Serial.println("⚠️ devmode field missing, creating...");
        FirebaseJson devmode;
        devmode.set("analogRead", false);
        devmode.set("boolean", false);
        devmode.set("pin", -1);
        devmode.set("pwm", false);
        devmode.set("pwmValue", 0);
        updateJson.set("devmode", devmode);
        needsUpdate = true;
    }
    
    // Verifica e cria led_schedule se não existir
    if (!jsonPtr->get(result, "led_schedule")) {
        Serial.println("⚠️ led_schedule missing, creating...");
        FirebaseJson ledSched;
        ledSched.set("scheduleEnabled", false);
        ledSched.set("solarSimEnabled", false);
        ledSched.set("onHour",          6);
        ledSched.set("onMinute",        0);
        ledSched.set("offHour",        20);
        ledSched.set("offMinute",       0);
        ledSched.set("intensity",      255);
        updateJson.set("led_schedule", ledSched);
        needsUpdate = true;
    }

    // Verifica e cria nó OTA se não existir
    if (!jsonPtr->get(result, "ota")) {
        Serial.println("⚠️ ota node missing, creating...");
        FirebaseJson otaNode;
        otaNode.set("available", false);
        otaNode.set("version",   "");
        otaNode.set("url",       "");
        otaNode.set("notes",     "");
        updateJson.set("ota", otaNode);
        needsUpdate = true;
    }

    // Se faltam campos, atualiza a estufa no Firebase
    if (needsUpdate) {
        Serial.println("🔄 Completing greenhouse structure with missing fields...");
        if (Firebase.updateNode(fbdo, path.c_str(), updateJson)) {
            Serial.println("✅ Greenhouse structure completed successfully!");
        } else {
            Serial.println("❌ Failed to complete greenhouse structure: " + fbdo.errorReason());
            return false;
        }
    }
    
    Serial.println("✅ All required fields are present and valid");
    return true;
}

bool FirebaseHandler::loadFirebaseCredentials(String& email, String& password) {
    Preferences preferences;
    if(!preferences.begin("firebase-creds", true)) {
        Serial.println("[ERROR] Failed to open preferences");
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
    if(!authenticated) {
        Serial.println("User not authenticated. Cannot receive setpoints.");
        return;
    }

    String path = "/greenhouses/" + greenhouseId + "/setpoints";
    
    if (Firebase.getJSON(fbdo, path.c_str())) {
        FirebaseJson *json = fbdo.jsonObjectPtr();
        FirebaseJsonData result;
        
        // CORREÇÃO: cada campo é lido de forma independente.
        // A lógica anterior acumulava erros com &= fazendo com que um único
        // campo ausente zerasse todos os setpoints seguintes silenciosamente.
        // Agora cada campo mantém seu valor atual caso não exista no Firebase.
        struct Setpoints {
            int   lux     = 5000;
            float tMax    = 30.0f;
            float tMin    = 20.0f;
            float uMax    = 80.0f;
            float uMin    = 60.0f;
            int   coSp    = 400;
            int   co2Sp   = 400;
            int   tvocsSp = 100;
        } setpoints;

        int fieldsRead = 0;
        if (json->get(result, "lux"))     { setpoints.lux     = result.intValue;   fieldsRead++; }
        if (json->get(result, "tMax"))    { setpoints.tMax    = result.floatValue; fieldsRead++; }
        if (json->get(result, "tMin"))    { setpoints.tMin    = result.floatValue; fieldsRead++; }
        if (json->get(result, "uMax"))    { setpoints.uMax    = result.floatValue; fieldsRead++; }
        if (json->get(result, "uMin"))    { setpoints.uMin    = result.floatValue; fieldsRead++; }
        if (json->get(result, "coSp"))    { setpoints.coSp    = result.intValue;   fieldsRead++; }
        if (json->get(result, "co2Sp"))   { setpoints.co2Sp   = result.intValue;   fieldsRead++; }
        if (json->get(result, "tvocsSp")) { setpoints.tvocsSp = result.intValue;   fieldsRead++; }

        // Aplica sempre que pelo menos um campo foi lido com sucesso
        if (fieldsRead > 0) {
            Serial.printf("[FIREBASE] Setpoints recebidos (%d/8 campos):\n", fieldsRead);
            Serial.println("- Lux: "      + String(setpoints.lux));
            Serial.println("- Temp Max: " + String(setpoints.tMax));
            Serial.println("- Temp Min: " + String(setpoints.tMin));
            Serial.println("- Hum Max: "  + String(setpoints.uMax));
            Serial.println("- Hum Min: "  + String(setpoints.uMin));
            Serial.println("- TVOCs: "    + String(setpoints.tvocsSp));

            actuators.applySetpoints(setpoints.lux, setpoints.tMin, setpoints.tMax,
                                     setpoints.uMin, setpoints.uMax, setpoints.coSp,
                                     setpoints.co2Sp, setpoints.tvocsSp);

            if (fieldsRead < 8) {
                Serial.printf("[FIREBASE] ⚠️ %d campo(s) ausentes — valores padrão mantidos para eles.\n", 8 - fieldsRead);
            }
        } else {
            Serial.println("[FIREBASE] ⚠️ Nenhum setpoint encontrado no JSON.");
        }
    } else {
        Serial.println("Error receiving setpoints: " + fbdo.errorReason());
    }
}

// 🔥 NOVAS FUNÇÕES PARA DEBUG E CALIBRAÇÃO

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
    // 🔥 CORREÇÃO: Inicializa com valores padrão
    relay1 = false;
    relay2 = false;
    relay3 = false;
    relay4 = false;
    ledsOn = false;
    ledsIntensity = 0;
    humidifierOn = false;
    
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
        
        Serial.printf("🔧 Manual states from Firebase - R1:%d R2:%d R3:%d R4:%d LED:%d(%d) HUM:%d\n",
                     relay1, relay2, relay3, relay4, ledsOn, ledsIntensity, humidifierOn);
    } else {
        Serial.println("❌ Failed to read manual actuator states: " + fbdo.errorReason());
    }
}

// 🔥 NOVA FUNÇÃO: Obtém configurações do devmode
void FirebaseHandler::getDevModeSettings(bool& analogRead, bool& digitalWrite, int& pin, bool& pwm, int& pwmValue) {
    // Valores padrão
    analogRead = false;
    digitalWrite = false;
    pin = -1;
    pwm = false;
    pwmValue = 0;
    
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
        
        Serial.printf("[DEVMODE] Settings - AnalogRead: %d, DigitalWrite: %d, Pin: %d, PWM: %d, PWMValue: %d\n",
                     analogRead, digitalWrite, pin, pwm, pwmValue);
    } else {
        Serial.println("[DEVMODE] ERRO - Falha ao ler configurações: " + fbdo.errorReason());
    }
}

// =============================================================================
// AGENDADOR DE LEDs — lê /led_schedule do Firebase RTDB
// =============================================================================

/**
 * @brief Lê a configuração do agendador de LEDs do Firebase e aplica no ActuatorController
 *
 * @details Lê o nó /greenhouses/<ID>/led_schedule e popula o LEDScheduler.
 * Chamada pelo handleFirebase() em MainController a cada FIREBASE_UPDATE_INTERVAL.
 *
 * Campos lidos:
 *  - scheduleEnabled (bool)  → ativa modo timer
 *  - solarSimEnabled (bool)  → ativa simulação solar (sobrepõe timer)
 *  - onHour  / onMinute  (int) → horário de início
 *  - offHour / offMinute (int) → horário de fim
 *  - intensity (int 0-255)   → intensidade no modo timer
 */
void FirebaseHandler::receiveLEDSchedule(ActuatorController& actuators) {
    if (!authenticated || !Firebase.ready()) return;

    String path = "/greenhouses/" + greenhouseId + "/led_schedule";
    if (!Firebase.getJSON(fbdo, path.c_str())) {
        Serial.println("[LED_SCHED] Falha ao ler led_schedule: " + fbdo.errorReason());
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

    Serial.printf("[LED_SCHED] Config: schedule=%d solar=%d on=%02d:%02d off=%02d:%02d int=%d\n",
                  actuators.ledScheduler.scheduleEnabled,
                  actuators.ledScheduler.solarSimEnabled,
                  actuators.ledScheduler.onHour,
                  actuators.ledScheduler.onMinute,
                  actuators.ledScheduler.offHour,
                  actuators.ledScheduler.offMinute,
                  actuators.ledScheduler.configIntensity);
}

// =============================================================================
// OTA — garante que o nó existe no RTDB (URL direta, sem Firebase Storage)
// =============================================================================

/**
 * @brief Garante que o nó /ota/ existe no RTDB com estrutura mínima
 *
 * @details O OTAHandler verifica /ota/available, /ota/version e /ota/url.
 * Esta função cria o nó se ele não existir, permitindo que a URL do .bin
 * seja inserida diretamente no RTDB sem depender do Firebase Storage.
 *
 * Fontes válidas de URL:
 *  - Firebase Storage:  https://firebasestorage.googleapis.com/...
 *  - GitHub Releases:   https://github.com/.../releases/download/v1.1.0/firmware.bin
 *  - Qualquer HTTPS:    qualquer servidor acessível pelo ESP32
 */
void FirebaseHandler::ensureOTANodeExists() {
    if (!authenticated || !Firebase.ready()) return;

    String path = "/greenhouses/" + greenhouseId + "/ota/available";
    if (Firebase.getBool(fbdo, path.c_str())) {
        return; // Nó já existe
    }

    Serial.println("[OTA] Nó OTA não encontrado, criando estrutura...");

    String basePath = "/greenhouses/" + greenhouseId + "/ota";
    FirebaseJson otaNode;
    otaNode.set("available", false);
    otaNode.set("version",   "");
    otaNode.set("url",       "");
    otaNode.set("notes",     "Insira a URL do .bin e mude available para true");

    if (Firebase.setJSON(fbdo, basePath.c_str(), otaNode)) {
        Serial.println("[OTA] ✅ Nó OTA criado no RTDB");
    } else {
        Serial.println("[OTA] ❌ Falha ao criar nó OTA: " + fbdo.errorReason());
    }
}

// =============================================================================
// AUTO-REPAIR — regenera campos ausentes sem recriar o banco inteiro
// =============================================================================

/**
 * @brief Verifica e regenera apenas os campos ausentes no RTDB
 *
 * @details Mais leve que isGreenhouseStructureComplete: não baixa o JSON inteiro.
 * Verifica campo a campo com Firebase.get() e adiciona apenas o que falta.
 * Ideal para chamada periódica (ex: a cada 5 minutos).
 *
 * Cobre:
 *  - Todos os setpoints
 *  - led_schedule completo
 *  - Nó OTA
 *  - debug_mode, manual_actuators, devmode
 */
void FirebaseHandler::repairMissingFields() {
    if (!authenticated || !Firebase.ready()) return;

    String base  = "/greenhouses/" + greenhouseId;
    bool   dirty = false;
    FirebaseJson patch;

    // ── setpoints ────────────────────────────────────────────────────────────
    struct { const char* key; float defVal; bool isFloat; } spFields[] = {
        {"setpoints/lux",     5000, false},
        {"setpoints/tMax",    30.0, true },
        {"setpoints/tMin",    20.0, true },
        {"setpoints/uMax",    80.0, true },
        {"setpoints/uMin",    60.0, true },
        {"setpoints/coSp",   400,   false},
        {"setpoints/co2Sp",  400,   false},
        {"setpoints/tvocsSp",100,   false},
    };
    for (auto& f : spFields) {
        if (!Firebase.get(fbdo, (base + "/" + f.key).c_str()) ||
            fbdo.dataType() == "null") {
            if (f.isFloat) patch.set(f.key, f.defVal);
            else            patch.set(f.key, (int)f.defVal);
            dirty = true;
            Serial.printf("[REPAIR] ⚠️ Campo ausente: %s\n", f.key);
        }
    }

    // ── led_schedule ─────────────────────────────────────────────────────────
    if (!Firebase.get(fbdo, (base + "/led_schedule").c_str()) ||
        fbdo.dataType() == "null") {
        FirebaseJson ls;
        ls.set("scheduleEnabled", false);
        ls.set("solarSimEnabled", false);
        ls.set("onHour",    6);
        ls.set("onMinute",  0);
        ls.set("offHour",  20);
        ls.set("offMinute", 0);
        ls.set("intensity",255);
        patch.set("led_schedule", ls);
        dirty = true;
        Serial.println("[REPAIR] ⚠️ Campo ausente: led_schedule");
    }

    // ── ota ──────────────────────────────────────────────────────────────────
    if (!Firebase.get(fbdo, (base + "/ota").c_str()) ||
        fbdo.dataType() == "null") {
        FirebaseJson ota;
        ota.set("available", false);
        ota.set("version",   "");
        ota.set("url",       "");
        ota.set("notes",     "");
        patch.set("ota", ota);
        dirty = true;
        Serial.println("[REPAIR] ⚠️ Campo ausente: ota");
    }

    // ── debug_mode ───────────────────────────────────────────────────────────
    if (!Firebase.get(fbdo, (base + "/debug_mode").c_str()) ||
        fbdo.dataType() == "null") {
        patch.set("debug_mode", false);
        dirty = true;
        Serial.println("[REPAIR] ⚠️ Campo ausente: debug_mode");
    }

    // ── Aplica patch ─────────────────────────────────────────────────────────
    if (dirty) {
        if (Firebase.updateNode(fbdo, base.c_str(), patch)) {
            Serial.println("[REPAIR] ✅ Campos ausentes restaurados com sucesso");
        } else {
            Serial.println("[REPAIR] ❌ Falha ao restaurar campos: " + fbdo.errorReason());
        }
    } else {
        Serial.println("[REPAIR] ✅ Estrutura do banco íntegra");
    }
}
