#include "DeviceUtils.h"

String getMacAddress() {
    uint8_t mac[6];
    char macStr[18] = {0};
    
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    
    return String(macStr);
}