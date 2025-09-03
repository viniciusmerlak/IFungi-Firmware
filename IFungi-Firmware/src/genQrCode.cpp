#include "genQrCode.h"
#include <Arduino.h>  // Adicionar esta linha

void GenQR::generateQRCode(const String& id) {
    Serial.println("ID: " + id);

    // Criar QR Code
    QRCode qrcode;
    uint8_t qrcodeData[qrcode_getBufferSize(3)];
    qrcode_initText(&qrcode, qrcodeData, 3, 0, id.c_str());

    // Imprimir QR Code no Serial (para debug)
    for (uint8_t y = 0; y < qrcode.size; y++) {
        for (uint8_t x = 0; x < qrcode.size; x++) {
            Serial.print(qrcode_getModule(&qrcode, x, y) ? "██" : "  ");
        }
        Serial.println();
    }
}