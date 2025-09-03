#include "perdiavontadedeviver.h"

String getMacAddress() {
    uint8_t mac[6]; // Array para armazenar o endereço MAC (6 bytes)
    char macStr[18] = {0}; // String formatada (17 caracteres + null terminator)

    // Obtém o endereço MAC
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    // Formata o MAC no padrão XX:XX:XX:XX:XX:XX
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    return String(macStr);
}
