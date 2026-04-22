#ifndef OTA_HANDLER_H
#define OTA_HANDLER_H

#include <Arduino.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>

/**
 * @file OTAHandler.h
 * @brief Controlador de atualização OTA (Over-The-Air) para o sistema IFungi
 * @version 1.0
 * @date 2026
 *
 * @details Gerencia o ciclo completo de atualização remota de firmware:
 * - Verificação periódica de novas versões no Firebase RTDB
 * - Download do binário via HTTPS
 * - Gravação na partição OTA inativa
 * - Validação e reinicialização automática
 * - Rollback automático em caso de falha
 *
 * @note Requer particionamento com suporte a OTA (min_spiffs ou equivalente).
 *       O nó Firebase usado é: /greenhouses/<ID>/ota/
 *
 * Estrutura esperada no Firebase RTDB:
 * @code
 * /greenhouses/IFUNGI-XX:XX:XX:XX:XX:XX/ota: {
 *   "available": true,
 *   "version": "1.2.0",
 *   "url": "https://storage.googleapis.com/seu-bucket/firmware.bin",
 *   "checksum": "opcional-md5",
 *   "notes": "Descrição opcional da atualização"
 * }
 * @endcode
 *
 * Após uma atualização bem-sucedida, o handler escreve de volta:
 * @code
 * /greenhouses/<ID>/ota: {
 *   "available": false,
 *   "lastUpdate": "1.2.0",
 *   "lastUpdateTime": <timestamp>
 * }
 * @endcode
 */

// Forward declaration para evitar dependência circular com GreenhouseSystem.h
class FirebaseHandler;

/**
 * @class OTAHandler
 * @brief Gerencia atualizações OTA remotas integradas ao Firebase RTDB
 */
class OTAHandler {
public:

    /**
     * @enum OTAStatus
     * @brief Estados possíveis do processo OTA
     */
    enum OTAStatus {
        IDLE,           ///< Aguardando / sem atualização pendente
        CHECKING,       ///< Verificando Firebase por nova versão
        DOWNLOADING,    ///< Baixando firmware
        WRITING,        ///< Gravando na flash
        SUCCESS,        ///< Atualização concluída, aguardando reboot
        FAILED          ///< Falha no processo
    };

    /**
     * @brief Inicializa o handler OTA
     * @param firebaseHandler Ponteiro para o FirebaseHandler existente
     * @param greenhouseId ID da estufa (ex: "IFUNGI-AA:BB:CC:DD:EE:FF")
     * @param currentVersion Versão atual do firmware (ex: "1.0.0")
     * @param checkIntervalMs Intervalo entre verificações em ms (padrão: 60s)
     */
    void begin(FirebaseHandler* firebaseHandler,
               const String& greenhouseId,
               const String& currentVersion,
               unsigned long checkIntervalMs = 60000);

    /**
     * @brief Deve ser chamado no loop() principal
     *
     * @details Executa verificações periódicas e, quando necessário,
     * inicia o processo de download e instalação. Não bloqueia o loop
     * exceto durante a fase de gravação na flash (~10-30s).
     *
     * @note Durante o download/escrita, os sensores continuam operando
     * pois o ESP32 tem dois cores. A escrita OTA usa o core de
     * background e apenas bloqueia brevemente para cada bloco.
     */
    void handle();

    /**
     * @brief Força uma verificação imediata, ignorando o intervalo
     * @note Útil para teste ou acionamento manual via Firebase
     */
    void checkNow();

    // Getters de estado
    OTAStatus getStatus() const { return _status; }
    String getCurrentVersion() const { return _currentVersion; }
    String getPendingVersion() const { return _pendingVersion; }
    int getProgress() const { return _progress; }
    bool isUpdating() const { return _status == DOWNLOADING || _status == WRITING; }

private:
    FirebaseHandler* _firebase = nullptr;
    String _greenhouseId;
    String _currentVersion;
    String _pendingVersion;
    String _pendingUrl;

    OTAStatus _status = IDLE;
    int _progress = 0;

    unsigned long _checkInterval = 60000;
    unsigned long _lastCheck = 0;
    bool _forceCheck = false;

    /**
     * @brief Consulta o nó /ota/ no Firebase RTDB
     * @return true se há atualização disponível para versão diferente da atual
     */
    bool _checkForUpdate();

    /**
     * @brief Realiza o download e escrita do firmware
     * @param url URL HTTPS do arquivo .bin
     * @return true se download e escrita foram bem-sucedidos
     */
    bool _downloadAndInstall(const String& url);

    /**
     * @brief Reporta o resultado da atualização de volta ao Firebase
     * @param success true = escreveu sucesso, false = escreveu falha
     */
    void _reportResult(bool success);

    /**
     * @brief Callback de progresso chamado pelo Update.h durante a escrita
     * @param progress Bytes escritos até o momento
     * @param total Total de bytes a escrever
     */
    static void _onProgress(size_t progress, size_t total);

    // Ponteiro estático para o callback ter acesso à instância
    static OTAHandler* _instance;
};

#endif // OTA_HANDLER_H
