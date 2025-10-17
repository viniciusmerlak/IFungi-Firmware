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
        Serial.printf("Falha crítica ao inicializar NVS: %d\n", err);
        return false;
    }
    
    nvsInitialized = true;
    Serial.println("✅ NVS inicializado com sucesso");
    return true;
}
void WiFiConfigurator::wifiHardReset() {
    Serial.println("🔄 Iniciando HARD RESET do WiFi...");
    
    // 1. Parar o WiFi
    Serial.println("1. Parando WiFi...");
    esp_wifi_stop();
    delay(1000);
    
    // 2. Desinicializar o driver WiFi
    Serial.println("2. Desinicializando driver WiFi...");
    esp_wifi_deinit();
    delay(1000);
    
    // 3. Desconectar e limpar configurações
    Serial.println("3. Limpando configurações WiFi...");
    WiFi.disconnect(true);
    delay(1000);
    
    // 4. Reset completo do modo
    Serial.println("4. Reset completo do modo WiFi...");
    WiFi.mode(WIFI_OFF);
    delay(1000);
    
    Serial.println("✅ HARD RESET do WiFi concluído");
}

bool WiFiConfigurator::emergencyAPMode(const char* apSSID, const char* apPassword) {
    Serial.println("🚨 Iniciando MODO DE EMERGÊNCIA para AP...");
    
    // Reset hard completo
    wifiHardReset();
    delay(2000);
    
    // Tentativa direta e simples
    Serial.println("Tentativa de inicialização direta do AP...");
    
    // Inicialização mínima do WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    
    if (ret != ESP_OK) {
        Serial.printf("❌ Falha na inicialização WiFi: %d\n", ret);
        return false;
    }
    
    // Configurar como AP
    ret = esp_wifi_set_mode(WIFI_MODE_AP);
    if (ret != ESP_OK) {
        Serial.printf("❌ Falha ao configurar modo AP: %d\n", ret);
        return false;
    }
    
    // Configurar SSID e senha
    wifi_config_t wifi_config = {};
    strcpy((char*)wifi_config.ap.ssid, apSSID);
    wifi_config.ap.ssid_len = strlen(apSSID);
    wifi_config.ap.channel = 1;
    wifi_config.ap.authmode = (apPassword && strlen(apPassword) >= 8) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.beacon_interval = 100;
    
    if (apPassword && strlen(apPassword) >= 8) {
        strcpy((char*)wifi_config.ap.password, apPassword);
    }
    
    ret = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (ret != ESP_OK) {
        Serial.printf("❌ Falha ao configurar AP: %d\n", ret);
        return false;
    }
    
    // Iniciar WiFi
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        Serial.printf("❌ Falha ao iniciar WiFi: %d\n", ret);
        return false;
    }
    
    Serial.println("✅ WiFi iniciado em modo AP (emergência)");
    
    // Aguardar estabilização
    delay(5000);
    
    // Verificar se está funcionando
    wifi_mode_t current_mode;
    esp_wifi_get_mode(&current_mode);
    
    if (current_mode == WIFI_MODE_AP) {
        Serial.println("📍 Modo AP confirmado");
        Serial.println("📡 SSID: " + String(apSSID));
        return true;
    } else {
        Serial.println("❌ Modo AP não confirmado");
        return false;
    }
}
bool WiFiConfigurator::autoConnect(const char* apSSID, const char* apPassword) {
    Serial.println("=== INICIANDO AUTO CONEXÃO WiFi ===");
    
    if (!initNVS()) {
        Serial.println("❌ Falha crítica: NVS não inicializado");
        // Tentar modo de emergência
        return emergencyAPMode(apSSID, apPassword);
    }
    
    // Tenta carregar credenciais salvas
    String storedSSID, storedPassword;
    if (loadCredentials(storedSSID, storedPassword)) {
        Serial.println("📡 Credenciais WiFi encontradas, tentando conectar...");
        Serial.println("SSID: " + storedSSID);
        
        if (connectToWiFi(storedSSID.c_str(), storedPassword.c_str(), 15000)) {
            Serial.println("✅ Conectado com credenciais salvas!");
            return true;
        } else {
            Serial.println("❌ Falha ao conectar com credenciais salvas");
            clearCredentials(); // Limpa credenciais inválidas
        }
    } else {
        Serial.println("📝 Nenhuma credencial WiFi salva encontrada");
    }
    
    // Se não conseguiu conectar, inicia AP
    Serial.println("🔄 Iniciando modo AP para configuração...");
    
    if (startAP(apSSID, apPassword)) {
        return false; // Retorna false porque está em modo AP, não conectado à rede
    } else {
        Serial.println("❌ Todas as tentativas de iniciar AP falharam");
        Serial.println("🔄 Reiniciando sistema em 5 segundos...");
        delay(5000);
        ESP.restart();
        return false;
    }
}
bool WiFiConfigurator::startAPFallback(const char* apSSID, const char* apPassword) {
    Serial.println("🔄 Usando método fallback para AP...");
    
    // Reset total do WiFi
    esp_wifi_stop();
    delay(1000);
    esp_wifi_deinit();
    delay(1000);
    
    // Reinicialização completa
    WiFi.begin();
    delay(2000);
    
    WiFi.mode(WIFI_AP);
    delay(1000);
    
    bool result = false;
    if (apPassword && strlen(apPassword) >= 8) {
        result = WiFi.softAP(apSSID, apPassword);
    } else {
        result = WiFi.softAP(apSSID);
    }
    
    if (result) {
        Serial.println("✅ AP iniciado via fallback");
        Serial.println("📍 IP: " + WiFi.softAPIP().toString());
        return false; // Retorna false porque está em modo AP
    } else {
        Serial.println("❌ Falha crítica mesmo no fallback");
        return false;
    }
}
bool WiFiConfigurator::connectToWiFi(const char* ssid, const char* password, unsigned long timeout) {
    Serial.println("🔗 Tentando conectar ao WiFi: " + String(ssid));
    
    if (strlen(ssid) == 0) {
        Serial.println("❌ Erro: SSID vazio");
        return false;
    }
    
    // Reset soft antes de tentar conectar
    wifiSoftReset();
    
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);
    
    Serial.print("⏳ Conectando");
    unsigned long startTime = millis();
    
    WiFi.begin(ssid, password);
    
    while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < timeout) {
        delay(500);
        Serial.print(".");
        
        if (millis() - startTime > timeout) {
            Serial.println("\n❌ Timeout na conexão WiFi");
            WiFi.disconnect(true);
            return false;
        }
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n✅ WiFi Conectado!");
        Serial.println("📡 IP: " + WiFi.localIP().toString());
        Serial.println("📶 RSSI: " + String(WiFi.RSSI()) + " dBm");
        
        saveCredentials(ssid, password);
        return true;
    }
    
    Serial.println("\n❌ Falha na conexão WiFi");
    WiFi.disconnect(true);
    return false;
}
void WiFiConfigurator::wifiSoftReset() {
    Serial.println("🔄 Iniciando SOFT RESET do WiFi...");
    
    // 1. Desconectar e limpar
    Serial.println("1. Desconectando WiFi...");
    WiFi.disconnect(true);
    delay(500);
    
    // 2. Reset do modo
    Serial.println("2. Resetando modo WiFi...");
    WiFi.mode(WIFI_OFF);
    delay(500);
    
    // 3. Reinicializar WiFi
    Serial.println("3. Reinicializando WiFi...");
    WiFi.mode(WIFI_STA);
    delay(500);
    
    Serial.println("✅ SOFT RESET do WiFi concluído");
}
bool WiFiConfigurator::startAP(const char* apSSID, const char* apPassword) {
    Serial.println("🌐 Iniciando Access Point...");
    
    // Tentativa 1: Método normal com Arduino WiFi
    Serial.println("Tentativa 1: Método Arduino WiFi");
    wifiSoftReset();
    
    WiFi.mode(WIFI_AP);
    delay(1000);
    
    bool apStarted = false;
    if (apPassword && strlen(apPassword) >= 8) {
        apStarted = WiFi.softAP(apSSID, apPassword);
        Serial.println("🔒 AP com senha: " + String(apSSID));
    } else {
        apStarted = WiFi.softAP(apSSID);
        Serial.println("🔓 AP aberto: " + String(apSSID));
    }
    
    if (apStarted) {
        delay(3000);
        Serial.println("📍 IP do AP: " + WiFi.softAPIP().toString());
        Serial.println("✅ AP iniciado com sucesso (método normal)");
        return true;
    }
    
    // Tentativa 2: Reset hard + método normal
    Serial.println("Tentativa 2: Reset hard + método normal");
    wifiHardReset();
    delay(2000);
    
    WiFi.mode(WIFI_AP);
    delay(1000);
    
    if (apPassword && strlen(apPassword) >= 8) {
        apStarted = WiFi.softAP(apSSID, apPassword);
    } else {
        apStarted = WiFi.softAP(apSSID);
    }
    
    if (apStarted) {
        delay(3000);
        Serial.println("📍 IP do AP: " + WiFi.softAPIP().toString());
        Serial.println("✅ AP iniciado com sucesso (após hard reset)");
        return true;
    }
    
    // Tentativa 3: Modo de emergência
    Serial.println("Tentativa 3: Modo de emergência");
    return emergencyAPMode(apSSID, apPassword);
}

bool WiFiConfigurator::loadCredentials(String &ssid, String &password) {
    if (!initNVS()) {
        Serial.println("❌ NVS não disponível para carregar credenciais");
        return false;
    }
    
    // Criar namespace se não existir
    if (!preferences.begin("wifi-creds", true)) {
        Serial.println("📝 Namespace 'wifi-creds' não encontrado - será criado automaticamente");
        preferences.end();
        
        // Criar o namespace
        if (!preferences.begin("wifi-creds", false)) {
            Serial.println("❌ Falha ao criar namespace 'wifi-creds'");
            return false;
        }
        preferences.end();
        return false;
    }
    
    bool hasSsid = preferences.isKey("ssid");
    bool hasPassword = preferences.isKey("password");
    
    if (hasSsid && hasPassword) {
        ssid = preferences.getString("ssid", "");
        password = preferences.getString("password", "");
        preferences.end();
        
        if (ssid.length() > 0 && password.length() > 0) {
            Serial.println("✅ Credenciais WiFi carregadas do NVS");
            return true;
        }
    }
    
    preferences.end();
    Serial.println("📝 Nenhuma credencial válida encontrada no NVS");
    return false;
}

void WiFiConfigurator::saveCredentials(const char* ssid, const char* password) {
    if (!initNVS()) {
        Serial.println("❌ NVS não disponível para salvar credenciais");
        return;
    }
    
    if (!preferences.begin("wifi-creds", false)) {
        Serial.println("❌ Falha ao abrir NVS para salvar credenciais");
        return;
    }
    
    preferences.putString("ssid", ssid);
    preferences.putString("password", password);
    
    // Force commit
    preferences.end();
    
    Serial.println("✅ Credenciais WiFi salvas no NVS: " + String(ssid));
}

void WiFiConfigurator::clearCredentials() {
    if (!preferences.begin("wifi-creds", false)) {
        return;
    }
    
    preferences.clear();
    preferences.end();
    
    Serial.println("🗑️ Credenciais WiFi limpas do NVS");
}

void WiFiConfigurator::piscaLED(bool on, int delayTime) {
    static int ledPin = -1;
    
    if (ledPin == -1) {
        ledPin = LED_BUILTIN;
        pinMode(ledPin, OUTPUT);
    }
    
    if (delayTime == 666666) {
        digitalWrite(ledPin, HIGH); // LED permanente
        return;
    }
    else if (delayTime == 777777) {
        // Piscar duplo rápido
        for (int n = 0; n < 3; n++) {
            digitalWrite(ledPin, HIGH); delay(100);
            digitalWrite(ledPin, LOW); delay(300);
            digitalWrite(ledPin, HIGH); delay(100);
            digitalWrite(ledPin, LOW); delay(500);
        }
        return;
    }
    else if (on) {
        // Piscar normal
        for (int n = 0; n < 2; n++) {
            digitalWrite(ledPin, HIGH);
            delay(delayTime);
            digitalWrite(ledPin, LOW);
            delay(delayTime);
        }
    }
    else {
        digitalWrite(ledPin, LOW);
    }
}

bool WiFiConfigurator::isConnected() {
    return WiFi.status() == WL_CONNECTED;
}

String WiFiConfigurator::getLocalIP() {
    if (WiFi.getMode() == WIFI_AP) {
        return WiFi.softAPIP().toString();
    }
    return WiFi.localIP().toString();
}

void WiFiConfigurator::stopAP() {
    WiFi.softAPdisconnect(true);
    delay(1000);
    Serial.println("📴 AP Mode Stopped");
}

void WiFiConfigurator::reconnectOrFallbackToAP(const char* apSSID, const char* apPassword, 
                                             const char* storedSSID, const char* storedPassword) {
    Serial.println("🔄 Starting connection sequence...");
    
    // Tenta reconectar com as credenciais armazenadas primeiro
    if(storedSSID && strlen(storedSSID) > 0) {
        Serial.println("🔄 Trying stored credentials...");
        if(connectToWiFi(storedSSID, storedPassword, 10000)) {
            return;
        }
    }
    
    Serial.println("🔄 Falling back to AP mode...");
    startAP(apSSID, apPassword);
}