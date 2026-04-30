/**
 * @file OTAHandler.cpp
 * @brief Implementação do controlador OTA para o sistema IFungi Greenhouse
 * @version 1.1
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
 * SEGURANÇA:
 *  - Apenas URLs com prefixo "https://" são aceitas. HTTP puro é rejeitado
 *    porque permitiria um atacante na rede local servir firmware malicioso
 *    (man-in-the-middle). O ESP32 já tem suporte TLS nativo — não há motivo
 *    para aceitar conexões não cifradas durante um update de firmware.
 *  - client.setInsecure() não valida o certificado CA do servidor, mas ainda
 *    garante criptografia TLS. Para validação completa, use setCACert() com
 *    o certificado raiz do servidor de storage (ex: ISRG Root X1 para Let's
 *    Encrypt, ou DigiCert Global Root CA para Firebase Storage).
 *
 * @note A biblioteca Update.h (incluída no ESP32 Arduino Core) cuida de:
 *   - Selecionar a partição OTA inativa automaticamente
 *   - Validar o cabeçalho do binário ESP32
 *   - Marcar a partição como boot ativa após sucesso
 *   - Restaurar a partição anterior em caso de falha (rollback)
 */

#include "OTAHandler.h"
#include "GreenhouseSystem.h"   // FirebaseHandler está definido aqui

#ifndef IFUNGI_OTA_PASSWORD
    #error "IFUNGI_OTA_PASSWORD nao definida. Verifique seu arquivo .env"
#endif

// Inicialização do ponteiro estático (necessário para callback estático)
OTAHandler* OTAHandler::_instance = nullptr;

// =============================================================================
// INICIALIZAÇÃO
// =============================================================================

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

    Serial.printf("[ota] Handler inicializado. Versão atual: %s | Intervalo: %lums\n",
                  _currentVersion.c_str(), _checkInterval);
}

// =============================================================================
// LOOP PRINCIPAL
// =============================================================================

void OTAHandler::handle() {
    if (WiFi.status() != WL_CONNECTED) return;
    if (_firebase == nullptr || !_firebase->isAuthenticated()) return;
    if (isUpdating()) return;

    bool timeToCheck = (millis() - _lastCheck >= _checkInterval);

    if (_forceCheck || timeToCheck) {
        _lastCheck   = millis();
        _forceCheck  = false;
        _status      = CHECKING;

        Serial.println("[ota] Verificando atualizações no Firebase...");

        if (_checkForUpdate()) {
            Serial.printf("[ota] Nova versao disponivel: %s -> %s\n",
                          _currentVersion.c_str(), _pendingVersion.c_str());
            Serial.println("[ota] Iniciando download...");

            if (_downloadAndInstall(_pendingUrl)) {
                // ESP.restart() chamado internamente — nunca chega aqui em sucesso
            } else {
                Serial.println("[ota] ERROR: Falha na instalacao. Sistema continua operando.");
                _reportResult(false);
                _status = FAILED;
            }
        } else {
            _status = IDLE;
        }
    }
}

void OTAHandler::checkNow() {
    _forceCheck = true;
    Serial.println("[ota] Verificação imediata agendada.");
}

// =============================================================================
// VERIFICAÇÃO NO FIREBASE
// =============================================================================

bool OTAHandler::_checkForUpdate() {
    String basePath = "/greenhouses/" + _greenhouseId + "/ota/";

    if (!Firebase.getBool(_firebase->fbdo, basePath + "available")) {
        Serial.println("[ota] Nó OTA não encontrado no Firebase (normal na primeira execução).");
        return false;
    }

    bool available = _firebase->fbdo.boolData();
    if (!available) {
        Serial.println("[ota] Nenhuma atualização disponível.");
        return false;
    }

    if (!Firebase.getString(_firebase->fbdo, basePath + "version")) {
        Serial.println("[ota] WARN: Campo 'version' nao encontrado no no OTA.");
        return false;
    }
    _pendingVersion = _firebase->fbdo.stringData();

    if (_pendingVersion == _currentVersion) {
        Serial.printf("[ota] Versão %s já instalada. Limpando flag...\n", _currentVersion.c_str());
        Firebase.setBool(_firebase->fbdo, basePath + "available", false);
        return false;
    }

    if (!Firebase.getString(_firebase->fbdo, basePath + "url")) {
        Serial.println("[ota] WARN: Campo 'url' nao encontrado no no OTA.");
        return false;
    }
    _pendingUrl = _firebase->fbdo.stringData();

    if (_pendingUrl.length() == 0) {
        Serial.println("[ota] WARN: URL vazia no no OTA.");
        return false;
    }

    // SEGURANÇA: rejeita qualquer URL que não use HTTPS.
    // Um firmware enviado via HTTP puro pode ser interceptado e substituído
    // por um atacante na mesma rede (MITM). O ESP32 suporta TLS — não há
    // justificativa para aceitar HTTP em atualizações de firmware.
    if (!_pendingUrl.startsWith("https://")) {
        Serial.println("[ota] ERROR: URL rejeitada - deve comecar com 'https://'");
        Serial.println("[ota]    URL recebida: " + _pendingUrl.substring(0, 40) + "...");
        // Limpa o campo de URL inválida e desativa a flag para não repetir
        Firebase.setString(_firebase->fbdo, basePath + "url", "");
        Firebase.setBool(_firebase->fbdo, basePath + "available", false);
        Firebase.setString(_firebase->fbdo, basePath + "lastError",
                           "URL rejeitada: deve ser HTTPS. Recebido: " +
                           _pendingUrl.substring(0, 40));
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
 *  - Apenas URLs HTTPS são aceitas (verificado em _checkForUpdate)
 *  - Timeout de 30s no HTTP
 *  - Validação do Content-Length antes de iniciar
 *  - Verifica cabeçalho mágico do binário ESP32 (0xE9) via Update.h
 *  - Rollback automático pelo Update.h se a escrita falhar
 *
 * NOTA SOBRE client.setInsecure():
 *  Desativa verificação do certificado CA — garante criptografia TLS mas
 *  não autentica a identidade do servidor. Para autenticação completa,
 *  use client.setCACert(root_ca) com o certificado raiz do servidor.
 *  Em ambientes controlados (rede interna + URL conhecida), setInsecure()
 *  é aceitável. Em produção com URLs públicas, prefira setCACert().
 *
 * @param url URL HTTPS completa do arquivo .bin
 * @return true em caso de sucesso (seguido de reboot), false em falha
 */
bool OTAHandler::_downloadAndInstall(const String& url) {
    _status   = DOWNLOADING;
    _progress = 0;

    HTTPClient http;
    WiFiClientSecure client;

    client.setInsecure(); // veja nota no header desta função

    Serial.println("[ota] Conectando ao servidor...");
    Serial.println("[ota] URL: " + url.substring(0, 60) + (url.length() > 60 ? "..." : ""));

    http.begin(client, url);
    http.setTimeout(30000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    int httpCode = http.GET();

    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("[ota] ERROR HTTP: %d (%s)\n", httpCode, http.errorToString(httpCode).c_str());
        http.end();
        return false;
    }

    int contentLength = http.getSize();

    if (contentLength <= 0) {
        Serial.println("[ota] ERROR: Content-Length invalido ou ausente.");
        http.end();
        return false;
    }

    Serial.printf("[ota] Tamanho do firmware: %d bytes (%.1f KB)\n",
                  contentLength, contentLength / 1024.0f);

    if (!Update.begin(contentLength, U_FLASH)) {
        Serial.printf("[ota] ERROR ao iniciar Update: ");
        Update.printError(Serial);
        http.end();
        return false;
    }

    Update.onProgress(_onProgress);

    _status = WRITING;

    WiFiClient* stream = http.getStreamPtr();
    const size_t BUFFER_SIZE = 1024;
    uint8_t buffer[BUFFER_SIZE];
    size_t written = 0;

    Serial.println("[ota] Gravando firmware na flash...");

    while (http.connected() && written < (size_t)contentLength) {
        unsigned long waitStart = millis();
        while (stream->available() == 0) {
            if (millis() - waitStart > 5000) {
                Serial.println("[ota] ERROR: Timeout aguardando dados do stream.");
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
            Serial.printf("[ota] ERROR de escrita: leu %u, escreveu %u bytes.\n",
                          bytesRead, bytesWritten);
            Update.abort();
            http.end();
            return false;
        }

        written += bytesWritten;
    }

    http.end();

    if (!Update.end(true)) {
        Serial.print("[ota] ERROR na finalizacao: ");
        Update.printError(Serial);
        return false;
    }

    if (!Update.isFinished()) {
        Serial.println("[ota] ERROR: Update nao foi concluido corretamente.");
        return false;
    }

    Serial.println("\n[ota] Firmware gravado com sucesso");
    Serial.printf("[ota] %u bytes escritos de %d\n", written, contentLength);

    _status = SUCCESS;
    _reportResult(true);

    Serial.println("[ota] Reiniciando em 3 segundos...");
    delay(3000);
    ESP.restart();

    return true; // Nunca alcançado
}

// =============================================================================
// CALLBACK DE PROGRESSO
// =============================================================================

void OTAHandler::_onProgress(size_t progress, size_t total) {
    if (_instance == nullptr || total == 0) return;

    int percent = (int)((progress * 100) / total);
    _instance->_progress = percent;

    if (percent % 10 == 0) {
        Serial.printf("[ota] Progresso: [");
        for (int i = 0; i < 10; i++) {
            Serial.print(i < (percent / 10) ? "#" : "-");
        }
        Serial.printf("] %d%% (%u/%u bytes)\n", percent, progress, total);
    }
}

// =============================================================================
// RELATÓRIO DE RESULTADO NO FIREBASE
// =============================================================================

void OTAHandler::_reportResult(bool success) {
    if (_firebase == nullptr || !_firebase->isAuthenticated()) return;

    String basePath = "/greenhouses/" + _greenhouseId + "/ota/";

    if (success) {
        Firebase.setBool(_firebase->fbdo, basePath + "available", false);
        Firebase.setString(_firebase->fbdo, basePath + "lastInstalledVersion", _pendingVersion);
        Firebase.setInt(_firebase->fbdo, basePath + "lastUpdateTimestamp",
                        (int)_firebase->getCurrentTimestamp());

        Serial.println("[ota] Resultado reportado ao Firebase: SUCESSO");
    } else {
        Firebase.setString(_firebase->fbdo, basePath + "lastError",
                           "Falha no download/instalação - " + _pendingVersion);
        Firebase.setInt(_firebase->fbdo, basePath + "lastErrorTimestamp",
                        (int)_firebase->getCurrentTimestamp());

        Serial.println("[ota] Resultado reportado ao Firebase: FALHA");
    }
}
