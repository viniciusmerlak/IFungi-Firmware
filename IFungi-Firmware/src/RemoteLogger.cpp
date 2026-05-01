/**
 * @file RemoteLogger.cpp
 * @brief Implementação do logger remoto para Firebase RTDB
 * @version 1.0
 * @date 2026
 */

#include "RemoteLogger.h"

// ─── Inicialização dos membros estáticos ──────────────────────────────────────

FirebaseData*  RemoteLogger::_fbdo        = nullptr;
String         RemoteLogger::_ghId        = "";
LogLevel       RemoteLogger::_minLevel    = LOG_INFO;

LogEntry       RemoteLogger::_queue[RLOG_QUEUE_SIZE];
int            RemoteLogger::_queueHead   = 0;
int            RemoteLogger::_queueTail   = 0;
int            RemoteLogger::_queueCount  = 0;

int            RemoteLogger::_rtdbHead    = 0;
int            RemoteLogger::_rtdbCount   = 0;
int            RemoteLogger::_errHead     = 0;

unsigned long  RemoteLogger::_lastFlush   = 0;
bool           RemoteLogger::_initialized = false;

// ─── Utilitários privados ─────────────────────────────────────────────────────

const char* RemoteLogger::_levelStr(LogLevel l) {
    switch (l) {
        case LOG_DEBUG:    return "DEBUG";
        case LOG_INFO:     return "INFO";
        case LOG_WARN:     return "WARN";
        case LOG_ERROR:    return "ERROR";
        case LOG_CRITICAL: return "CRITICAL";
        default:           return "INFO";
    }
}

String RemoteLogger::_formatDateTime(unsigned long ts) {
    if (ts < 1609459200UL) return "";  // timestamp inválido
    time_t t = (time_t)ts;
    struct tm* tm = gmtime(&t);
    char buf[25];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", tm);
    return String(buf);
}

bool RemoteLogger::_isFirebaseReady() {
    return _fbdo != nullptr && Firebase.ready();
}

// ─── Inicialização ────────────────────────────────────────────────────────────

void RemoteLogger::init(FirebaseData* fbdo,
                        const String& ghId,
                        LogLevel minLevel) {
    _fbdo      = fbdo;
    _ghId      = ghId;
    _minLevel  = minLevel;

    // Tenta ler o head atual do Firebase para continuar de onde parou
    // (evita reiniciar do slot 0 a cada reboot, o que sobrescreveria logs recentes)
    if (_isFirebaseReady()) {
        String headPath = "/greenhouses/" + _ghId + "/logs/head";
        if (Firebase.getInt(*_fbdo, headPath.c_str())) {
            _rtdbHead = _fbdo->intData();
            if (_rtdbHead < 0 || _rtdbHead >= RLOG_MAX_LOGS) _rtdbHead = 0;
        }
        String countPath = "/greenhouses/" + _ghId + "/logs/count";
        if (Firebase.getInt(*_fbdo, countPath.c_str())) {
            _rtdbCount = _fbdo->intData();
        }
        String errPath = "/greenhouses/" + _ghId + "/logs/err_head";
        if (Firebase.getInt(*_fbdo, errPath.c_str())) {
            _errHead = _fbdo->intData();
            if (_errHead < 0 || _errHead >= RLOG_MAX_PERSISTENT) _errHead = 0;
        }
    }

    _initialized = true;
    Serial.printf("[rlog] Logger inicializado | ghId: %s | minLevel: %s | head: %d\n",
                  _ghId.c_str(), _levelStr(minLevel), _rtdbHead);
}

// ─── Garantia de nó no Firebase ───────────────────────────────────────────────

void RemoteLogger::ensureNodeExists() {
    if (!_isFirebaseReady() || _ghId.isEmpty()) return;

    String base = "/greenhouses/" + _ghId + "/logs";

    // Só cria se o nó não existir
    if (Firebase.get(*_fbdo, (base + "/head").c_str())) {
        if (_fbdo->dataType() != "null") {
            Serial.println("[rlog] Nó /logs já existe no Firebase");
            return;
        }
    }

    FirebaseJson meta;
    meta.set("head",     0);
    meta.set("count",    0);
    meta.set("err_head", 0);
    meta.set("max",      RLOG_MAX_LOGS);

    if (Firebase.updateNode(*_fbdo, base.c_str(), meta)) {
        Serial.println("[rlog] Nó /logs criado no Firebase");
    } else {
        Serial.println("[rlog] ERRO ao criar nó /logs: " + _fbdo->errorReason());
    }
}

// ─── Log principal ────────────────────────────────────────────────────────────

void RemoteLogger::log(LogLevel level, const char* tag, const char* msg) {
    // Sempre imprime no Serial
    Serial.printf("[%s] %s %s\n", _levelStr(level), tag, msg);

    // Filtra por nível mínimo antes de enfileirar para o Firebase
    if (!_initialized || level < _minLevel) return;

    // Enfileira na RAM (fila circular — descarta o mais antigo se cheia)
    LogEntry e;
    e.timestamp = 0;  // será preenchido pelo flush com o timestamp do Firebase
    e.level     = level;
    strncpy(e.tag, tag, sizeof(e.tag) - 1);
    e.tag[sizeof(e.tag) - 1] = '\0';
    strncpy(e.msg, msg, sizeof(e.msg) - 1);
    e.msg[sizeof(e.msg) - 1] = '\0';

    if (_queueCount < RLOG_QUEUE_SIZE) {
        _queue[_queueTail] = e;
        _queueTail = (_queueTail + 1) % RLOG_QUEUE_SIZE;
        _queueCount++;
    } else {
        // Fila cheia — descarta o mais antigo (overwrite head)
        _queue[_queueTail] = e;
        _queueTail = (_queueTail + 1) % RLOG_QUEUE_SIZE;
        _queueHead = (_queueHead + 1) % RLOG_QUEUE_SIZE;
        Serial.println("[rlog] WARN: fila cheia, log mais antigo descartado");
    }
}

// ─── Envio ao RTDB ────────────────────────────────────────────────────────────

/**
 * @brief Envia uma entrada na lista rolante do RTDB
 *
 * @details Escreve em /logs/recent/<slot> e atualiza /logs/head e /logs/count.
 * O slot é reutilizado ciclicamente (0..MAX_LOGS-1), criando o efeito rolante
 * sem precisar deletar entradas antigas.
 */
bool RemoteLogger::_sendEntry(const LogEntry& e) {
    if (!_isFirebaseReady() || _ghId.isEmpty()) return false;

    String base = "/greenhouses/" + _ghId + "/logs";
    String slotPath = base + "/recent/" + String(_rtdbHead);

    FirebaseJson entry;
    entry.set("ts",  (int)e.timestamp);
    entry.set("dt",  _formatDateTime(e.timestamp));
    entry.set("lvl", _levelStr(e.level));
    entry.set("tag", e.tag);
    entry.set("msg", e.msg);

    if (!Firebase.setJSON(*_fbdo, slotPath.c_str(), entry)) {
        Serial.println("[rlog] ERRO ao gravar log: " + _fbdo->errorReason());
        return false;
    }

    // Avança o índice ciclicamente e atualiza os metadados
    _rtdbHead = (_rtdbHead + 1) % RLOG_MAX_LOGS;
    _rtdbCount++;

    FirebaseJson meta;
    meta.set("head",  _rtdbHead);
    meta.set("count", _rtdbCount);

    Firebase.updateNode(*_fbdo, base.c_str(), meta);
    return true;
}

/**
 * @brief Grava um erro/crítico no nó persistente last_errors
 *
 * @details Separado da lista rolante para que erros importantes não
 * desapareçam quando a lista rolar. O app pode exibir estes erros
 * proeminentemente na interface.
 */
bool RemoteLogger::_sendPersistentError(const LogEntry& e) {
    if (!_isFirebaseReady() || _ghId.isEmpty()) return false;

    String path = "/greenhouses/" + _ghId + "/logs/last_errors/" + String(_errHead);

    FirebaseJson entry;
    entry.set("ts",  (int)e.timestamp);
    entry.set("dt",  _formatDateTime(e.timestamp));
    entry.set("lvl", _levelStr(e.level));
    entry.set("tag", e.tag);
    entry.set("msg", e.msg);

    if (!Firebase.setJSON(*_fbdo, path.c_str(), entry)) {
        return false;
    }

    _errHead = (_errHead + 1) % RLOG_MAX_PERSISTENT;

    String errHeadPath = "/greenhouses/" + _ghId + "/logs/err_head";
    Firebase.setInt(*_fbdo, errHeadPath.c_str(), _errHead);
    return true;
}

// ─── Flush (chamado no loop) ──────────────────────────────────────────────────

/**
 * @brief Processa uma entrada da fila por chamada
 *
 * @details Chamado a cada loop(). Respeita RLOG_MIN_FLUSH_INTERVAL para não
 * saturar o Firebase com writes simultâneos. Se o Firebase não estiver
 * disponível, a fila cresce em RAM até RLOG_QUEUE_SIZE entradas.
 *
 * @return true se havia entrada e foi processada (ou tentada)
 */
bool RemoteLogger::flush() {
    if (_queueCount == 0) return false;
    if (!_isFirebaseReady()) return false;

    unsigned long now = millis();
    if (now - _lastFlush < RLOG_MIN_FLUSH_INTERVAL) return false;
    _lastFlush = now;

    // Pega a entrada mais antiga da fila
    LogEntry e = _queue[_queueHead];
    _queueHead = (_queueHead + 1) % RLOG_QUEUE_SIZE;
    _queueCount--;

    // Obtém o timestamp agora que o Firebase está disponível
    // (evita depender de NTP em _log, que pode ser chamado antes do boot completo)
    if (e.timestamp == 0) {
        // Tenta obter via NTP — se indisponível usa millis como fallback relativo
        time_t t = time(nullptr);
        if (t >= 1609459200UL) {
            e.timestamp = (unsigned long)t;
        } else {
            e.timestamp = millis() / 1000UL;
        }
    }

    bool ok = _sendEntry(e);

    // Erros e críticos também vão para o nó persistente
    if (ok && (e.level >= LOG_ERROR)) {
        _sendPersistentError(e);
    }

    return ok;
}
