#include "GreenhouseSystem.h"
#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>

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
        Serial.println("❌ Not authenticated or Firebase not ready to update actuators");
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

    json.set("lastUpdate", millis());
    json.set("atuadores", actuators); // 🔥 CORREÇÃO: Estrutura correta

    String path = FirebaseHandler::getGreenhousesPath() + greenhouseId;
    
    if (Firebase.updateNode(fbdo, path.c_str(), json)) {
        Serial.println("✅ Actuator states updated successfully in Firebase!");
        Serial.printf("   Relays: [%d,%d,%d,%d] LEDs: %s (%dW) Humidifier: %s\n", 
                     relay1, relay2, relay3, relay4, 
                     ledsOn ? "ON" : "OFF", ledsWatts,
                     humidifierOn ? "ON" : "OFF");
    } else {
        Serial.println("❌ Failed to update actuators: " + fbdo.errorReason());
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
        Serial.println("📴 Firebase not available for history");
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
        Serial.println("✅ Data saved to history successfully!");
        return true;
    } else {
        Serial.println("❌ Failed to save history: " + fbdo.errorReason());
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
    
    if (numRecords >= MAX_RECORDS) {
        for (int i = 1; i < MAX_RECORDS; i++) {
            preferences.putFloat(("temp_" + String(i - 1)).c_str(), preferences.getFloat(("temp_" + String(i)).c_str(), 0));
            preferences.putFloat(("umid_" + String(i - 1)).c_str(), preferences.getFloat(("umid_" + String(i)).c_str(), 0));
            preferences.putInt(("co2_" + String(i - 1)).c_str(), preferences.getInt(("co2_" + String(i)).c_str(), 0));
            preferences.putInt(("co_" + String(i - 1)).c_str(), preferences.getInt(("co_" + String(i)).c_str(), 0));
            preferences.putInt(("lux_" + String(i - 1)).c_str(), preferences.getInt(("lux_" + String(i)).c_str(), 0));
            preferences.putInt(("tvocs_" + String(i - 1)).c_str(), preferences.getInt(("tvocs_" + String(i)).c_str(), 0));
            preferences.putULong(("timestamp_" + String(i - 1)).c_str(), preferences.getULong(("timestamp_" + String(i)).c_str(), 0));
        }
        numRecords = MAX_RECORDS - 1;
    }
    
    int newIndex = numRecords;
    preferences.putFloat(("temp_" + String(newIndex)).c_str(), temp);
    preferences.putFloat(("umid_" + String(newIndex)).c_str(), humidity);
    preferences.putInt(("co2_" + String(newIndex)).c_str(), co2);
    preferences.putInt(("co_" + String(newIndex)).c_str(), co);
    preferences.putInt(("lux_" + String(newIndex)).c_str(), lux);
    preferences.putInt(("tvocs_" + String(newIndex)).c_str(), tvocs);
    preferences.putULong(("timestamp_" + String(newIndex)).c_str(), timestamp);
    
    preferences.putInt("num_registros", numRecords + 1);
    preferences.putULong("ultimo_timestamp", timestamp);
    
    preferences.end();
    
    Serial.println("Data saved locally. Record: " + String(newIndex));
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
    Serial.println("Trying to send " + String(numRecords) + " local records");
    
    preferences.end();
    
    if (!preferences.begin(NAMESPACE, false)) {
        Serial.println("Failed to open Preferences for writing");
        return;
    }
    
    for (int i = 0; i < numRecords; i++) {
        float temp = preferences.getFloat(("temp_" + String(i)).c_str(), 0);
        float humidity = preferences.getFloat(("umid_" + String(i)).c_str(), 0);
        int co2 = preferences.getInt(("co2_" + String(i)).c_str(), 0);
        int co = preferences.getInt(("co_" + String(i)).c_str(), 0);
        int lux = preferences.getInt(("lux_" + String(i)).c_str(), 0);
        int tvocs = preferences.getInt(("tvocs_" + String(i)).c_str(), 0);
        unsigned long timestamp = preferences.getULong(("timestamp_" + String(i)).c_str(), 0);
        
        if (Firebase.ready() && authenticated) {
            if (sendDataToHistory(temp, humidity, co2, co, lux, tvocs)) {
                preferences.remove(("temp_" + String(i)).c_str());
                preferences.remove(("umid_" + String(i)).c_str());
                preferences.remove(("co2_" + String(i)).c_str());
                preferences.remove(("co_" + String(i)).c_str());
                preferences.remove(("lux_" + String(i)).c_str());
                preferences.remove(("tvocs_" + String(i)).c_str());
                preferences.remove(("timestamp_" + String(i)).c_str());
            } else {
                Serial.println("Failed to send record " + String(i) + ", stopping...");
                break;
            }
        } else {
            Serial.println("Firebase not available, stopping send...");
            break;
        }
    }
    
    int newRecords = 0;
    for (int i = 0; i < numRecords; i++) {
        if (preferences.isKey(("temp_" + String(i)).c_str())) {
            if (i != newRecords) {
                preferences.putFloat(("temp_" + String(newRecords)).c_str(), preferences.getFloat(("temp_" + String(i)).c_str(), 0));
                preferences.putFloat(("umid_" + String(newRecords)).c_str(), preferences.getFloat(("umid_" + String(i)).c_str(), 0));
                preferences.putInt(("co2_" + String(newRecords)).c_str(), preferences.getInt(("co2_" + String(i)).c_str(), 0));
                preferences.putInt(("co_" + String(newRecords)).c_str(), preferences.getInt(("co_" + String(i)).c_str(), 0));
                preferences.putInt(("lux_" + String(newRecords)).c_str(), preferences.getInt(("lux_" + String(i)).c_str(), 0));
                preferences.putInt(("tvocs_" + String(newRecords)).c_str(), preferences.getInt(("tvocs_" + String(i)).c_str(), 0));
                preferences.putULong(("timestamp_" + String(newRecords)).c_str(), preferences.getULong(("timestamp_" + String(i)).c_str(), 0));
                
                preferences.remove(("temp_" + String(i)).c_str());
                preferences.remove(("umid_" + String(i)).c_str());
                preferences.remove(("co2_" + String(i)).c_str());
                preferences.remove(("co_" + String(i)).c_str());
                preferences.remove(("lux_" + String(i)).c_str());
                preferences.remove(("tvocs_" + String(i)).c_str());
                preferences.remove(("timestamp_" + String(i)).c_str());
            }
            newRecords++;
        }
    }
    
    preferences.putInt("num_registros", newRecords);
    preferences.end();
    
    Serial.println("Local sends completed. Remaining: " + String(newRecords) + " records.");
}

void FirebaseHandler::sendSensorData(float temp, float humidity, int co2, int co, int lux, int tvocs, bool waterLevel) {
    refreshTokenIfNeeded();
    if (!authenticated || !Firebase.ready()) {
        Serial.println("Not authenticated or invalid token");
        return;
    }

    FirebaseJson json;
    json.set("sensores/temperatura", temp);
    json.set("sensores/umidade", humidity);
    json.set("sensores/co2", co2);
    json.set("sensores/co", co);
    json.set("sensores/tvocs", tvocs);
    json.set("sensores/luminosidade", lux);
    json.set("lastUpdate", millis());
    json.set("niveis/agua", waterLevel);

    String path = "/greenhouses/" + greenhouseId;
    Firebase.updateNode(fbdo, path.c_str(), json);
    
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
    json.set("lastUpdate", (int)time(nullptr));

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


    

    // 🔥 NOVO: Status do sistema
    FirebaseJson status;
    status.set("online", true);
    status.set("lastHeartbeat", millis());
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
    json.set("lastHeartbeat", millis());
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
        "status/lastHeartbeat"
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
    
    // Verifica e cria campos de calibração de água se não existirem
    
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
        
        struct Setpoints {
            int lux;
            float tMax;
            float tMin;
            float uMax;
            float uMin;
            int coSp;
            int co2Sp;
            int tvocsSp;
        } setpoints;

        bool success = true;
        success &= json->get(result, "lux"); setpoints.lux = success ? result.intValue : 0;
        success &= json->get(result, "tMax"); setpoints.tMax = success ? result.floatValue : 0;
        success &= json->get(result, "tMin"); setpoints.tMin = success ? result.floatValue : 0;
        success &= json->get(result, "uMax"); setpoints.uMax = success ? result.floatValue : 0;
        success &= json->get(result, "uMin"); setpoints.uMin = success ? result.floatValue : 0;
        success &= json->get(result, "coSp"); setpoints.coSp = success ? result.intValue : 0;
        success &= json->get(result, "co2Sp"); setpoints.co2Sp = success ? result.intValue : 0;
        success &= json->get(result, "tvocsSp"); setpoints.tvocsSp = success ? result.intValue : 0;

        if (success) {
            Serial.println("Setpoints received successfully:");
            Serial.println("- Lux: " + String(setpoints.lux));
            Serial.println("- Temp Max: " + String(setpoints.tMax));
            Serial.println("- Temp Min: " + String(setpoints.tMin));
            Serial.println("- Humidity Max: " + String(setpoints.uMax));
            Serial.println("- Humidity Min: " + String(setpoints.uMin));
            Serial.println("- TVOCs: " + String(setpoints.tvocsSp));

            actuators.applySetpoints(setpoints.lux, setpoints.tMin, setpoints.tMax, 
                                setpoints.uMin, setpoints.uMax, setpoints.coSp, 
                                setpoints.co2Sp, setpoints.tvocsSp);
        } else {
            Serial.println("Some setpoints not found in JSON");
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





