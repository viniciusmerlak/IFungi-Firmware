#ifndef GEN_QR_CODE_H
#define GEN_QR_CODE_H

#include <Arduino.h>
#include <qrcode.h>

class GenQR {
public:
    void generateQRCode(const String& id);
};

#endif