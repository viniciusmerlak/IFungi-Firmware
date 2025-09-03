#ifndef WIFI_CONFIGURATOR_H
#define WIFI_CONFIGURATOR_H

#include <WiFi.h>
#include <Preferences.h>
#include <nvs_flash.h>

class WiFiConfigurator {
int n ;
public:
    int LED_BUILTIN = 2;
    
    void startAP(const char* apSSID, const char* apPassword = nullptr);
    bool connectToWiFi(const char* ssid, const char* password, bool persist);
    void reconnectOrFallbackToAP(const char* apSSID, const char* apPassword, const char* storedSSID, const char* storedPassword);
    void stopAP();
    bool isConnected();
    String getLocalIP();
    void piscaLED(bool on, int delayTime);
    unsigned long WIFI_CONNECT_TIMEOUT = 15000; // 30 seconds
    void saveCredentials(const char* ssid, const char* password);
    bool loadCredentials(String &ssid, String &password);
    void clearCredentials();

private:
    Preferences preferences;
};

#endif