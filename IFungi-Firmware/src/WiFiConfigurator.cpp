#include "WiFiConfigurator.h"

bool WiFiConfigurator::initNVS() {
    if (nvsInitialized) return true;
    
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        Serial.println("NVS corrompido, limpando e reinicializando...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    
    if (err != ESP_OK) {
        Serial.printf("Falha ao inicializar NVS: %d\n", err);
        return false;
    }
    
    nvsInitialized = true;
    Serial.println("NVS inicializado com sucesso");
    return true;
}

bool WiFiConfigurator::autoConnect(const char* apSSID, const char* apPassword) {
    Serial.println("=== INICIANDO AUTO CONEXÃO WiFi ===");
    
    if (!initNVS()) {
        Serial.println("Falha crítica: NVS não inicializado");
        return false;
    }
    
    // Tenta carregar credenciais salvas
    String storedSSID, storedPassword;
    if (loadCredentials(storedSSID, storedPassword)) {
        Serial.println("Credenciais WiFi encontradas, tentando conectar...");
        Serial.println("SSID: " + storedSSID);
        
        if (connectToWiFi(storedSSID.c_str(), storedPassword.c_str(), false)) {
            Serial.println("✅ Conectado com credenciais salvas!");
            return true;
        } else {
            Serial.println("❌ Falha ao conectar com credenciais salvas");
            clearCredentials(); // Limpa credenciais inválidas
        }
    } else {
        Serial.println("Nenhuma credencial WiFi salva encontrada");
    }
    
    // Se não conseguiu conectar, inicia AP
    Serial.println("🔄 Iniciando modo AP para configuração...");
    startAP(apSSID, apPassword);
    return false;
}

bool WiFiConfigurator::connectToWiFi(const char* ssid, const char* password, bool persistent) {
    Serial.println("Tentando conectar ao WiFi: " + String(ssid));
    
    if (strlen(ssid) == 0) {
        Serial.println("Erro: SSID vazio");
        return false;
    }
    
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);
    
    WiFi.begin(ssid, password);
    
    unsigned long startTime = millis();
    int attempts = 0;
    const int maxAttempts = 30; // 15 segundos
    
    while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts) {
        delay(500);
        Serial.print(".");
        attempts++;
        digitalWrite(LED_BUILTIN, attempts % 2); // Piscar LED
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n✅ WiFi Conectado!");
        Serial.println("📡 IP: " + WiFi.localIP().toString());
        Serial.println("📶 RSSI: " + String(WiFi.RSSI()) + " dBm");
        
        if (persistent) {
            saveCredentials(ssid, password);
        }
        
        digitalWrite(LED_BUILTIN, HIGH);
        return true;
    }
    
    Serial.println("\n❌ Falha na conexão WiFi");
    digitalWrite(LED_BUILTIN, LOW);
    return false;
}

void WiFiConfigurator::startAP(const char* apSSID, const char* apPassword) {
    Serial.println("🌐 Iniciando Access Point...");
    
    WiFi.mode(WIFI_AP);
    
    if (apPassword && strlen(apPassword) >= 8) {
        WiFi.softAP(apSSID, apPassword);
        Serial.println("🔒 AP com senha: " + String(apSSID));
    } else {
        WiFi.softAP(apSSID);
        Serial.println("🔓 AP aberto: " + String(apSSID));
    }
    
    delay(100); // Estabilização
    
    Serial.println("📍 IP do AP: " + WiFi.softAPIP().toString());
    Serial.println("📡 MAC: " + WiFi.softAPmacAddress());
}

bool WiFiConfigurator::loadCredentials(String &ssid, String &password) {
    if (!initNVS()) return false;
    
    if (!preferences.begin("wifi-creds", true)) {
        Serial.println("Namespace 'wifi-creds' não encontrado");
        return false;
    }
    
    bool hasSsid = preferences.isKey("ssid");
    bool hasPassword = preferences.isKey("password");
    
    if (hasSsid && hasPassword) {
        ssid = preferences.getString("ssid", "");
        password = preferences.getString("password", "");
        preferences.end();
        
        if (ssid.length() > 0) {
            Serial.println("Credenciais WiFi carregadas");
            return true;
        }
    }
    
    preferences.end();
    return false;
}

void WiFiConfigurator::saveCredentials(const char* ssid, const char* password) {
    if (!initNVS()) return;
    
    if (!preferences.begin("wifi-creds", false)) {
        Serial.println("Falha ao abrir NVS para salvar");
        return;
    }
    
    preferences.putString("ssid", ssid);
    preferences.putString("password", password);
    preferences.end();
    
    Serial.println("✅ Credenciais WiFi salvas no NVS");
}

void WiFiConfigurator::clearCredentials() {
    if (!preferences.begin("wifi-creds", false)) {
        return;
    }
    
    preferences.clear();
    preferences.end();
    
    Serial.println("Credenciais WiFi limpas do NVS");
}

void WiFiConfigurator::piscaLED(bool on, int delayTime) {
    pinMode(LED_BUILTIN, OUTPUT);
    
    if (delayTime == 666666) {
        digitalWrite(LED_BUILTIN, HIGH);
        return;
    }
    else if (delayTime == 777777) {
        for (n = 0; n < 6; n++) {
            digitalWrite(LED_BUILTIN, HIGH); delay(100);
            digitalWrite(LED_BUILTIN, LOW); delay(300);
            digitalWrite(LED_BUILTIN, HIGH); delay(100);
            digitalWrite(LED_BUILTIN, LOW);
        }
        return;
    }
    else if (on) {
        for (n = 0; n < 3; n++) {
            digitalWrite(LED_BUILTIN, HIGH);
            delay(delayTime);
            digitalWrite(LED_BUILTIN, LOW);
        }
    }
    else {
        digitalWrite(LED_BUILTIN, LOW);
    }
}

bool WiFiConfigurator::isConnected() {
    return WiFi.status() == WL_CONNECTED;
}

String WiFiConfigurator::getLocalIP() {
    return WiFi.localIP().toString();
}

void WiFiConfigurator::stopAP() {
    WiFi.softAPdisconnect(true);
    Serial.println("AP Mode Stopped");
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