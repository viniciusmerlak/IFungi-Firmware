#include "DeviceUtils.h"

/**
 * @brief Obtém o endereço MAC da interface WiFi
 * @return String contendo o endereço MAC no formato XX:XX:XX:XX:XX:XX
 * 
 * @note Utiliza a função esp_read_mac() para ler o MAC da STA WiFi
 *       e formata em string legível
 */
String getMacAddress() {
    uint8_t mac[6];
    char macStr[18] = {0};
    
    // Lê o endereço MAC da interface WiFi STA
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    
    // Formata o MAC como string no formato padrão
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    
    return String(macStr);
}