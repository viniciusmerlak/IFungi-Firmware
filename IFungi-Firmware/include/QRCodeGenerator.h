#ifndef QR_CODE_GENERATOR_H
#define QR_CODE_GENERATOR_H

#include <Arduino.h>
#include <qrcode.h>

class QRCodeGenerator {
public:
    void generateQRCode(const String& id);
};

#endif