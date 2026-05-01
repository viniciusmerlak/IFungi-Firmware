#ifndef REMOTE_LOGGER_H
#define REMOTE_LOGGER_H

/**
 * @file RemoteLogger.h
 * @brief Logger remoto para Firebase RTDB com interceptação do Serial
 * @version 1.0
 * @date 2026
 *
 * @details Captura logs do sistema e os envia ao Firebase RTDB em duas
 * estruturas paralelas:
 *
 *  LISTA ROLANTE (recent_logs) — últimos MAX_LOGS entradas:
 *  ─────────────────────────────────────────────────────────
 *  /greenhouses/<ID>/logs/recent: {
 *    "0": { "ts": 1720000000, "dt": "2026-01-01T12:00:00Z",
 *            "lvl": "INFO", "msg": "[sensor] DHT22 OK" },
 *    "1": { ... },
 *    ...
 *  }
 *  /greenhouses/<ID>/logs/head: 42          ← índice do próximo slot
 *  /greenhouses/<ID>/logs/count: 42         ← total de logs já gravados
 *
 *  ÚLTIMOS ERROS PERSISTENTES (last_errors) — sobrevivem ao rolamento:
 *  ─────────────────────────────────────────────────────────────────────
 *  /greenhouses/<ID>/logs/last_errors: {
 *    "0": { "ts": ..., "lvl": "ERROR", "msg": "DHT22: INOPERANTE" },
 *    ...  (até MAX_PERSISTENT_ERRORS slots, rotativo)
 *  }
 *
 * NÍVEIS DE LOG:
 *  DEBUG   → detalhes de execução (uso intenso, filtrável)
 *  INFO    → eventos normais (boot, mudança de modo, sensores)
 *  WARN    → situações anormais mas recuperáveis
 *  ERROR   → falhas que afetam operação (DHT inoperante, Firebase offline)
 *  CRITICAL→ falhas que ameaçam a estufa (aquecimento fora de controle)
 *
 * FILA INTERNA (NVS):
 *  Logs gerados enquanto offline são armazenados na NVS (até NVS_QUEUE_SIZE)
 *  e enviados automaticamente quando o Firebase estiver disponível.
 *
 * USO:
 *  // Inclui o logger uma vez no setup:
 *  RemoteLogger::init(&firebase, actuators);
 *
 *  // Nos arquivos que precisam logar:
 *  #include "RemoteLogger.h"
 *  RLOG_INFO("[sensor]", "DHT22 inicializado com sucesso");
 *  RLOG_ERROR("[sensor]", "DHT22: INOPERANTE");
 *  RLOG_WARN("[peltier]", "Cooldown ativo — aquecimento bloqueado");
 *  RLOG_CRITICAL("[peltier]", "Temperatura fora de controle: 45C");
 *
 *  // No loop():
 *  RemoteLogger::flush();   // envia fila pendente ao Firebase
 */

#include <Arduino.h>
#include <FirebaseESP32.h>

// ─── Capacidades ──────────────────────────────────────────────────────────────

/// Slots na lista rolante do RTDB (cada slot ≈ 120 bytes → ~24KB no Firebase)
#define RLOG_MAX_LOGS             200

/// Slots de erros persistentes (ERROR + CRITICAL — sobrevivem ao rolamento)
#define RLOG_MAX_PERSISTENT       20

/// Fila offline em RAM (logs gerados antes do Firebase estar pronto)
#define RLOG_QUEUE_SIZE           50

/// Tamanho máximo de cada mensagem de log (truncada se maior)
#define RLOG_MSG_MAX_LEN          120

/// Intervalo mínimo entre envios ao Firebase (ms) — evita rate-limit
#define RLOG_MIN_FLUSH_INTERVAL   800

// ─── Níveis ───────────────────────────────────────────────────────────────────

enum LogLevel {
    LOG_DEBUG    = 0,
    LOG_INFO     = 1,
    LOG_WARN     = 2,
    LOG_ERROR    = 3,
    LOG_CRITICAL = 4
};

// ─── Macros de conveniência ───────────────────────────────────────────────────

#define RLOG_DEBUG(tag, msg)    RemoteLogger::log(LOG_DEBUG,    tag, msg)
#define RLOG_INFO(tag, msg)     RemoteLogger::log(LOG_INFO,     tag, msg)
#define RLOG_WARN(tag, msg)     RemoteLogger::log(LOG_WARN,     tag, msg)
#define RLOG_ERROR(tag, msg)    RemoteLogger::log(LOG_ERROR,    tag, msg)
#define RLOG_CRITICAL(tag, msg) RemoteLogger::log(LOG_CRITICAL, tag, msg)

/// Formata printf-style e loga no nível especificado
#define RLOG_FMT(level, tag, fmt, ...) do { \
    char _buf[RLOG_MSG_MAX_LEN]; \
    snprintf(_buf, sizeof(_buf), fmt, ##__VA_ARGS__); \
    RemoteLogger::log(level, tag, _buf); \
} while(0)

// ─── Estrutura de entrada de log ──────────────────────────────────────────────

struct LogEntry {
    unsigned long timestamp;        ///< Unix timestamp (0 se offline)
    LogLevel      level;
    char          tag[24];          ///< Ex: "[sensor]", "[peltier]"
    char          msg[RLOG_MSG_MAX_LEN];
};

// ─── Classe principal ─────────────────────────────────────────────────────────

class RemoteLogger {
public:

    /**
     * @brief Inicializa o logger com referência ao FirebaseData e ID da estufa
     *
     * @param fbdo      Ponteiro para o FirebaseData do sistema
     * @param ghId      ID da estufa (ex: "IFUNGI-AA:BB:CC:DD:EE:FF")
     * @param minLevel  Nível mínimo enviado ao Firebase (default: LOG_INFO)
     *                  Logs abaixo deste nível são apenas impressos no Serial.
     */
    static void init(FirebaseData* fbdo,
                     const String& ghId,
                     LogLevel minLevel = LOG_INFO);

    /**
     * @brief Registra um log
     *
     * @details Sempre imprime no Serial. Se nível >= minLevel e Firebase
     * disponível, enfileira para envio. Erros/Críticos também vão para
     * last_errors.
     *
     * @param level Nível do log
     * @param tag   Tag de origem (ex: "[sensor]")
     * @param msg   Mensagem
     */
    static void log(LogLevel level, const char* tag, const char* msg);
    static void log(LogLevel level, const char* tag, const String& msg) {
        log(level, tag, msg.c_str());
    }

    /**
     * @brief Envia entradas pendentes da fila ao Firebase
     *
     * @details Deve ser chamado no loop() principal. Envia no máximo
     * uma entrada por chamada para não bloquear o loop.
     * Retorna true se havia algo na fila.
     */
    static bool flush();

    /**
     * @brief Altera o nível mínimo de log em runtime
     */
    static void setMinLevel(LogLevel level) { _minLevel = level; }

    /**
     * @brief Retorna quantas entradas estão aguardando envio
     */
    static int queueSize() { return _queueCount; }

    /**
     * @brief Garante que o nó /logs existe no Firebase com estrutura inicial
     *
     * @details Chamado uma vez após autenticação. Cria head=0 e count=0 se
     * o nó não existir — não sobrescreve se já existir.
     */
    static void ensureNodeExists();

    // Nível de log configurável em compilação — define RLOG_MIN_LEVEL no platformio.ini
    // para filtrar DEBUG em produção: build_flags = -DRLOG_MIN_LEVEL=1
    #ifndef RLOG_MIN_LEVEL
    #define RLOG_MIN_LEVEL 1   // LOG_INFO por padrão
    #endif

private:
    static FirebaseData*  _fbdo;
    static String         _ghId;
    static LogLevel       _minLevel;

    // Fila circular em RAM
    static LogEntry       _queue[RLOG_QUEUE_SIZE];
    static int            _queueHead;
    static int            _queueTail;
    static int            _queueCount;

    // Índice rolante do RTDB
    static int            _rtdbHead;     ///< próximo slot a escrever (0..MAX_LOGS-1)
    static int            _rtdbCount;    ///< total de logs gravados (para o app saber se rolou)

    // Índice rolante dos erros persistentes
    static int            _errHead;

    static unsigned long  _lastFlush;
    static bool           _initialized;

    static const char*    _levelStr(LogLevel l);
    static bool           _sendEntry(const LogEntry& e);
    static bool           _sendPersistentError(const LogEntry& e);
    static String         _formatDateTime(unsigned long ts);
    static bool           _isFirebaseReady();
};

#endif // REMOTE_LOGGER_H
