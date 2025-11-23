/**
 * @file QRCodeGenerator.cpp
 * @brief Implementação do gerador de QR Code para identificação da estufa
 * @author Seu Nome
 * @date 2024
 * @version 1.0
 * 
 * @details Este arquivo implementa a geração de QR Codes para identificação
 * única da estufa, permitindo acesso rápido via dispositivos móveis.
 * 
 * @note Utiliza a biblioteca qrcode para geração dos códigos
 */

#include "QRCodeGenerator.h"
#include <Arduino.h>

/**
 * @brief Gera e exvia um QR Code contendo o ID da estufa
 * 
 * @param id String contendo o ID único da estufa a ser codificado
 * 
 * @details Gera um QR Code versão 3 (29x29 módulos) com correção de erro
 * baixa (ECC 0) e exibe no monitor serial usando caracteres de bloco.
 * 
 * @note O QR Code é exibido apenas no monitor serial para debug.
 * Em produção, pode ser adaptado para displays ou outras saídas.
 * 
 * @warning A geração de QR Codes maiores pode exigir mais memória RAM
 * 
 * @see QRCodeGenerator::generateQRCode()
 */
void QRCodeGenerator::generateQRCode(const String& id) {
    // Exibe o ID em texto claro
    Serial.println("ID: " + id);

    // Estrutura e buffer para o QR Code
    QRCode qrcode;
    uint8_t qrcodeData[qrcode_getBufferSize(3)];
    
    // Inicializa o QR Code com o texto
    qrcode_initText(&qrcode, qrcodeData, 3, 0, id.c_str());

    // Renderiza o QR Code no serial usando caracteres de bloco
    for (uint8_t y = 0; y < qrcode.size; y++) {
        for (uint8_t x = 0; x < qrcode.size; x++) {
            // Imprime bloco sólido para módulos escuros, espaço para claros
            Serial.print(qrcode_getModule(&qrcode, x, y) ? "██" : "  ");
        }
        Serial.println(); // Nova linha após cada linha do QR Code
    }
}