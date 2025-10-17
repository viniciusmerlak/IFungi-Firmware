#ifndef WIFI_CONFIGURATOR_H
#define WIFI_CONFIGURATOR_H

#include <WiFi.h>
#include <Preferences.h>
#include <nvs_flash.h>
#include "esp_wifi.h"

class WiFiConfigurator {
private:
    int n;
    Preferences preferences;
    bool nvsInitialized = false;
    
    bool initNVS();
    bool startAPFallback(const char* apSSID, const char* apPassword);

public:
    int LED_BUILTIN = 2;
    unsigned long WIFI_CONNECT_TIMEOUT = 15000;
    
    // Métodos públicos
    bool autoConnect(const char* apSSID, const char* apPassword = nullptr);
    bool startAP(const char* apSSID, const char* apPassword = nullptr);
    bool connectToWiFi(const char* ssid, const char* password, unsigned long timeout);
    void reconnectOrFallbackToAP(const char* apSSID, const char* apPassword, 
                                const char* storedSSID, const char* storedPassword);
    void stopAP();
    bool isConnected();
    String getLocalIP();
    void piscaLED(bool on, int delayTime);
    void saveCredentials(const char* ssid, const char* password);
    bool loadCredentials(String &ssid, String &password);
    void clearCredentials();
    
    // Novas funções de reset do WiFi
    void wifiHardReset();
    void wifiSoftReset();
    bool emergencyAPMode(const char* apSSID, const char* apPassword = nullptr);
};

#endif