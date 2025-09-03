#include "WiFiConfigurator.h"

void WiFiConfigurator::startAP(const char* apSSID, const char* apPassword) {
    WiFi.softAP(apSSID, apPassword);
    Serial.println("AP Mode Started");
    Serial.print("SSID: "); Serial.println(apSSID);
    Serial.print("IP: "); Serial.println(WiFi.softAPIP());
    
}

void WiFiConfigurator::piscaLED(bool on, int delayTime) {
    pinMode(LED_BUILTIN, OUTPUT);
    
    if (delayTime == 666666) {
        // Modo: LED permanentemente aceso
        digitalWrite(LED_BUILTIN, HIGH);
        return;
    }
    else if (delayTime == 777777) {
        // Modo: pulso duplo
        for (n = 0; n < 6; n++) {
            digitalWrite(LED_BUILTIN, HIGH);
            delay(100);
            digitalWrite(LED_BUILTIN, LOW);
            delay(300);
            digitalWrite(LED_BUILTIN, HIGH);
            delay(100);
            digitalWrite(LED_BUILTIN, LOW);
        }
        return;

    }
    else if (on) {
        // Modo: piscar com delay específico
        for (n = 0; n < 3; n++) {
            
            digitalWrite(LED_BUILTIN, HIGH);
            delay(delayTime);
            digitalWrite(LED_BUILTIN, LOW);
        }
    }
    else if (!on) {
        // Desligar LED
        digitalWrite(LED_BUILTIN, LOW);
    }
}
bool WiFiConfigurator::connectToWiFi(const char* ssid, const char* password, bool persistent) {
    

    Serial.println("Attempting WiFi connection...");
    Serial.print("SSID: "); Serial.println(ssid);
    
    WiFi.begin(ssid, password);
    unsigned long startTime = millis();
    
    while (WiFi.status() != WL_CONNECTED && 
        (millis() - startTime < WIFI_CONNECT_TIMEOUT)) {
        delay(500);
        Serial.print(".");
        
        // Piscar LED indicando tentativa de conexão

    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi Connected!");
        Serial.print("IP Address: "); Serial.println(WiFi.localIP());
        
        if(persistent) {
            saveCredentials(ssid, password);
        }
        
        digitalWrite(LED_BUILTIN, HIGH); // LED aceso indicando conexão
        return true;
    }
    
    Serial.println("\nConnection Failed!");

    return false;
}

void WiFiConfigurator::reconnectOrFallbackToAP(const char* apSSID, const char* apPassword, 
                                             const char* storedSSID, const char* storedPassword) {
    Serial.println("Starting connection sequence...");
    
    // Tenta reconectar com as credenciais armazenadas primeiro
    if(storedSSID && strlen(storedSSID) > 0) {
        Serial.println("Trying stored credentials...");
        if(connectToWiFi(storedSSID, storedPassword, false)) {
            return;
        }
    }
    
    Serial.println("Falling back to AP mode...");

    startAP(apSSID, apPassword);
}

void WiFiConfigurator::stopAP() {
    WiFi.softAPdisconnect(true);
    Serial.println("AP Mode Stopped");
}

bool WiFiConfigurator::isConnected() {
    return WiFi.status() == WL_CONNECTED;
}

String WiFiConfigurator::getLocalIP() {
    return WiFi.localIP().toString();
}

bool WiFiConfigurator::loadCredentials(String &ssid, String &password) {
    if(!preferences.begin("wifi-creds", true)) {
        Serial.println("Namespace doesn't exist, creating...");
        preferences.end();
        return false;
    }
    
    if(preferences.isKey("ssid")) {
        ssid = preferences.getString("ssid", "");
        password = preferences.getString("password", "");
        preferences.end();
        
        if(ssid.length() > 0) {
            Serial.println("Loaded WiFi credentials from NVS");
            return true;
        }
    }
    
    preferences.end();
    return false;
}

void WiFiConfigurator::saveCredentials(const char* ssid, const char* password) {
    if(!preferences.begin("wifi-creds", false)) {
        Serial.println("Failed to access NVS!");
        return;
    }
    
    preferences.putString("ssid", ssid);
    preferences.putString("password", password);
    preferences.end();
    
    Serial.println("Credentials saved to NVS");
}

void WiFiConfigurator::clearCredentials() {
    preferences.begin("wifi-creds", false);
    preferences.clear();
    preferences.end();
    nvs_flash_erase();
    nvs_flash_init();
    Serial.println("NVS cleared and reinitialized");
}