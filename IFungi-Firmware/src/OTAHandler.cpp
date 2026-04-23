/**
 * @file OTAHandler.cpp
 * @brief Implementação do controlador OTA para o sistema IFungi Greenhouse
 * @version 1.0
 * @date 2026
 *
 * @details Fluxo completo de atualização:
 *
 *  [IDLE] ---(intervalo atingido)---> [CHECKING]
 *    |                                     |
 *    |                           (Firebase: available=true)
 *    |                                     |
 *    |                               [DOWNLOADING]
 *    |                                     |
 *    |                             (stream OK, Update.h)
 *    |                                     |
 *    |                                 [WRITING]
 *    |                                     |
 *    |                           (Update.end() == true)
 *    |                                     |
 *    +<--(falha em qualquer etapa)---   [SUCCESS]
 *    |                                     |
 *  [FAILED]                         ESP.restart()
 *
 * @note A biblioteca Update.h (já incluída no ESP32 Arduino Core) cuida de:
 *   - Selecionar a partição OTA inativa automaticamente
 *   - Validar o cabeçalho do binário ESP32
 *   - Marcar a partição como boot ativa após sucesso
 *   - Restaurar a partição anterior em caso de falha (rollback)
 */

#include "OTAHandler.h"
#include "GreenhouseSystem.h"   // FirebaseHandler está definido aqui

// Validação em tempo de compilação
#ifndef IFUNGI_OTA_PASSWORD
    #error "IFUNGI_OTA_PASSWORD nao definida. Verifique seu arquivo .env"
#endif



// Inicialização do ponteiro estático (necessário para callback estático)
OTAHandler* OTAHandler::_instance = nullptr;

// =============================================================================
// INICIALIZAÇÃO
// =============================================================================

/**
 * @brief Inicializa o OTAHandler com as dependências do sistema
 */
void OTAHandler::begin(FirebaseHandler* firebaseHandler,
                       const String& greenhouseId,
                       const String& currentVersion,
                       unsigned long checkIntervalMs) {
    _firebase        = firebaseHandler;
    _greenhouseId    = greenhouseId;
    _currentVersion  = currentVersion;
    _checkInterval   = checkIntervalMs;
    _status          = IDLE;
    _lastCheck       = 0;
    _instance        = this;

    Serial.printf("[OTA] Handler inicializado. Versão atual: %s | Intervalo: %lums\n",
                  _currentVersion.c_str(), _checkInterval);
}

// =============================================================================
// LOOP PRINCIPAL
// =============================================================================

/**
 * @brief Gerencia o ciclo OTA — deve ser chamado no loop() principal
 */
void OTAHandler::handle() {
    // Só opera com WiFi e Firebase ativos
    if (WiFi.status() != WL_CONNECTED) return;
    if (_firebase == nullptr || !_firebase->isAuthenticated()) return;
    // Não re-entra durante download/escrita
    if (isUpdating()) return;

    bool timeToCheck = (millis() - _lastCheck >= _checkInterval);

    if (_forceCheck || timeToCheck) {
        _lastCheck   = millis();
        _forceCheck  = false;
        _status      = CHECKING;

        Serial.println("[OTA] Verificando atualizações no Firebase...");

        if (_checkForUpdate()) {
            Serial.printf("[OTA] Nova versão disponível: %s → %s\n",
                          _currentVersion.c_str(), _pendingVersion.c_str());
            Serial.println("[OTA] Iniciando download...");

            if (_downloadAndInstall(_pendingUrl)) {
                // _downloadAndInstall chama ESP.restart() internamente após sucesso
                // Este ponto nunca é alcançado em caso de sucesso
            } else {
                Serial.println("[OTA] ❌ Falha na instalação. Sistema continua operando normalmente.");
                _reportResult(false);
                _status = FAILED;
            }
        } else {
            _status = IDLE;
        }
    }
}

/**
 * @brief Força verificação imediata na próxima chamada de handle()
 */
void OTAHandler::checkNow() {
    _forceCheck = true;
    Serial.println("[OTA] Verificação imediata agendada.");
}

// =============================================================================
// VERIFICAÇÃO NO FIREBASE
// =============================================================================

/**
 * @brief Consulta o nó /greenhouses/<ID>/ota/ no Firebase RTDB
 *
 * @details Lê os campos:
 *  - available (bool): indica se há atualização
 *  - version   (String): versão do novo firmware
 *  - url       (String): URL HTTPS do arquivo .bin
 *
 * @return true se available=true E version != versão atual
 */
bool OTAHandler::_checkForUpdate() {
    String basePath = "/greenhouses/" + _greenhouseId + "/ota/";

    // -------------------------------------------------------------------------
    // Lê o campo "available"
    // -------------------------------------------------------------------------
    if (!Firebase.getBool(_firebase->fbdo, basePath + "available")) {
        // Nó pode não existir ainda — não é erro crítico
        Serial.println("[OTA] Nó OTA não encontrado no Firebase (normal na primeira execução).");
        return false;
    }

    bool available = _firebase->fbdo.boolData();
    if (!available) {
        Serial.println("[OTA] Nenhuma atualização disponível.");
        return false;
    }

    // -------------------------------------------------------------------------
    // Lê a versão alvo
    // -------------------------------------------------------------------------
    if (!Firebase.getString(_firebase->fbdo, basePath + "version")) {
        Serial.println("[OTA] ⚠️ Campo 'version' não encontrado no nó OTA.");
        return false;
    }
    _pendingVersion = _firebase->fbdo.stringData();

    // Evita re-instalação da versão já em execução
    if (_pendingVersion == _currentVersion) {
        Serial.printf("[OTA] Versão %s já instalada. Limpando flag...\n", _currentVersion.c_str());
        Firebase.setBool(_firebase->fbdo, basePath + "available", false);
        return false;
    }

    // -------------------------------------------------------------------------
    // Lê a URL do binário
    // -------------------------------------------------------------------------
    if (!Firebase.getString(_firebase->fbdo, basePath + "url")) {
        Serial.println("[OTA] ⚠️ Campo 'url' não encontrado no nó OTA.");
        return false;
    }
    _pendingUrl = _firebase->fbdo.stringData();

    if (_pendingUrl.length() == 0) {
        Serial.println("[OTA] ⚠️ URL vazia no nó OTA.");
        return false;
    }

    return true;
}

// =============================================================================
// DOWNLOAD E INSTALAÇÃO
// =============================================================================

/**
 * @brief Faz o download via HTTPS e grava na partição OTA inativa
 *
 * @details Usa HTTPClient em modo stream para não precisar alocar o binário
 * inteiro na RAM. O Update.h recebe os bytes em blocos de 1KB e grava
 * diretamente na flash.
 *
 * Proteções implementadas:
 *  - Timeout de 30s no HTTP
 *  - Validação do Content-Length antes de iniciar
 *  - Verifica cabeçalho mágico do binário ESP32 (0xE9)
 *  - Rollback automático pelo Update.h se a escrita falhar
 *
 * @param url URL HTTPS completa do arquivo .bin
 * @return true em caso de sucesso (seguido de reboot), false em falha
 */
bool OTAHandler::_downloadAndInstall(const String& url) {
    _status   = DOWNLOADING;
    _progress = 0;

    HTTPClient http;
    WiFiClientSecure client;

    // Sem verificação de certificado CA para simplicidade.
    // Em produção, considere usar setCACert() com o certificado da Google/Firebase.
    client.setInsecure();

    Serial.println("[OTA] Conectando ao servidor...");
    Serial.println("[OTA] URL: " + url);

    http.begin(client, url);
    http.setTimeout(30000); // 30 segundos de timeout

    // Segue redirecionamentos HTTP (necessário para URLs do Firebase Storage)
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    int httpCode = http.GET();

    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("[OTA] ❌ Erro HTTP: %d (%s)\n", httpCode, http.errorToString(httpCode).c_str());
        http.end();
        return false;
    }

    // Obtém tamanho do arquivo
    int contentLength = http.getSize();

    if (contentLength <= 0) {
        Serial.println("[OTA] ❌ Content-Length inválido ou ausente.");
        http.end();
        return false;
    }

    Serial.printf("[OTA] Tamanho do firmware: %d bytes (%.1f KB)\n",
                  contentLength, contentLength / 1024.0f);

    // -------------------------------------------------------------------------
    // Inicia o processo de escrita OTA
    // Update.UPDATE_FIRMWARE = seleciona partição de app (não SPIFFS)
    // -------------------------------------------------------------------------
    if (!Update.begin(contentLength, U_FLASH)) {
        Serial.printf("[OTA] ❌ Erro ao iniciar Update: ");
        Update.printError(Serial);
        http.end();
        return false;
    }

    // Registra callback de progresso
    Update.onProgress(_onProgress);

    _status = WRITING;

    // -------------------------------------------------------------------------
    // Stream: lê em blocos de 1KB e escreve na flash
    // -------------------------------------------------------------------------
    WiFiClient* stream = http.getStreamPtr();
    const size_t BUFFER_SIZE = 1024;
    uint8_t buffer[BUFFER_SIZE];
    size_t written = 0;

    Serial.println("[OTA] Gravando firmware na flash...");

    while (http.connected() && written < (size_t)contentLength) {
        // Aguarda dados disponíveis (timeout 5s)
        unsigned long waitStart = millis();
        while (stream->available() == 0) {
            if (millis() - waitStart > 5000) {
                Serial.println("[OTA] ❌ Timeout aguardando dados do stream.");
                Update.abort();
                http.end();
                return false;
            }
            delay(1);
        }

        size_t bytesRead = stream->readBytes(buffer,
                           min(BUFFER_SIZE, (size_t)(contentLength - written)));

        if (bytesRead == 0) continue;

        size_t bytesWritten = Update.write(buffer, bytesRead);

        if (bytesWritten != bytesRead) {
            Serial.printf("[OTA] ❌ Erro de escrita: leu %u, escreveu %u bytes.\n",
                          bytesRead, bytesWritten);
            Update.abort();
            http.end();
            return false;
        }

        written += bytesWritten;
    }

    http.end();

    // -------------------------------------------------------------------------
    // Finaliza e valida a imagem gravada
    // -------------------------------------------------------------------------
    if (!Update.end(true)) {
        Serial.print("[OTA] ❌ Falha na finalização: ");
        Update.printError(Serial);
        return false;
    }

    if (!Update.isFinished()) {
        Serial.println("[OTA] ❌ Update não foi concluído corretamente.");
        return false;
    }

    // -------------------------------------------------------------------------
    // Sucesso — reporta ao Firebase e reinicia
    // -------------------------------------------------------------------------
    Serial.println("\n[OTA] ✅ Firmware gravado com sucesso!");
    Serial.printf("[OTA] %u bytes escritos de %d\n", written, contentLength);

    _status = SUCCESS;
    _reportResult(true);

    Serial.println("[OTA] Reiniciando em 3 segundos...");
    delay(3000);
    ESP.restart();

    return true; // Nunca alcançado
}

// =============================================================================
// CALLBACK DE PROGRESSO
// =============================================================================

/**
 * @brief Callback estático chamado pelo Update.h durante a escrita
 * @param progress Bytes gravados até o momento
 * @param total Total de bytes a gravar
 */
void OTAHandler::_onProgress(size_t progress, size_t total) {
    if (_instance == nullptr || total == 0) return;

    int percent = (int)((progress * 100) / total);
    _instance->_progress = percent;

    // Imprime barra de progresso a cada 10%
    if (percent % 10 == 0) {
        Serial.printf("[OTA] Progresso: [");
        for (int i = 0; i < 10; i++) {
            Serial.print(i < (percent / 10) ? "█" : "░");
        }
        Serial.printf("] %d%% (%u/%u bytes)\n", percent, progress, total);
    }
}

// =============================================================================
// RELATÓRIO DE RESULTADO NO FIREBASE
// =============================================================================

/**
 * @brief Atualiza o nó /ota/ no Firebase com o resultado da atualização
 *
 * @details Em caso de sucesso, limpa a flag "available" e registra a versão
 * instalada e o timestamp. Em caso de falha, mantém "available" = true para
 * permitir nova tentativa, e registra o erro.
 *
 * @param success true = instalação bem-sucedida
 */
void OTAHandler::_reportResult(bool success) {
    if (_firebase == nullptr || !_firebase->isAuthenticated()) return;

    String basePath = "/greenhouses/" + _greenhouseId + "/ota/";

    if (success) {
        // Limpa a flag de atualização disponível
        Firebase.setBool(_firebase->fbdo, basePath + "available", false);
        // Registra versão instalada
        Firebase.setString(_firebase->fbdo, basePath + "lastInstalledVersion", _pendingVersion);
        // Registra timestamp da instalação
        Firebase.setInt(_firebase->fbdo, basePath + "lastUpdateTimestamp",
                        (int)_firebase->getCurrentTimestamp());

        Serial.println("[OTA] ✅ Resultado reportado ao Firebase: SUCESSO");
    } else {
        // Mantém available=true para nova tentativa
        // Registra a falha para diagnóstico
        Firebase.setString(_firebase->fbdo, basePath + "lastError",
                           "Falha no download/instalação - " + _pendingVersion);
        Firebase.setInt(_firebase->fbdo, basePath + "lastErrorTimestamp",
                        (int)_firebase->getCurrentTimestamp());

        Serial.println("[OTA] ⚠️ Resultado reportado ao Firebase: FALHA");
    }
}
