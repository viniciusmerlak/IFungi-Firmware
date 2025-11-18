#include "QRCodeGenerator.h"
#include <Arduino.h>

void QRCodeGenerator::generateQRCode(const String& id) {
    Serial.println("ID: " + id);

    QRCode qrcode;
    uint8_t qrcodeData[qrcode_getBufferSize(3)];
    qrcode_initText(&qrcode, qrcodeData, 3, 0, id.c_str());

    for (uint8_t y = 0; y < qrcode.size; y++) {
        for (uint8_t x = 0; x < qrcode.size; x++) {
            Serial.print(qrcode_getModule(&qrcode, x, y) ? "██" : "  ");
        }
        Serial.println();
    }
}