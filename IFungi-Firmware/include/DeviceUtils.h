#ifndef DEVICE_UTILS_H
#define DEVICE_UTILS_H

#include <WiFi.h>
#include <Arduino.h>

/**
 * @file DeviceUtils.h
 * @brief Utilitários para informações do dispositivo
 * 
 * Este arquivo contém funções utilitárias para obter informações
 * específicas do dispositivo, como endereço MAC.
 */

/**
 * @brief Obtém o endereço MAC da interface WiFi
 * @return String contendo o endereço MAC no formato XX:XX:XX:XX:XX:XX
 * 
 * @note Utilizado para gerar IDs únicos para a estufa
 */
String getMacAddress();

#endif