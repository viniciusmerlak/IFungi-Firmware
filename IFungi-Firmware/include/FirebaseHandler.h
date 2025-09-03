#ifndef FIREBASE_HANDLER_H
#define FIREBASE_HANDLER_H

#include "perdiavontadedeviver.h"
#include <FirebaseESP32.h>
#include <nvs_flash.h>
#include "ActuatorController.h"
#include "WiFiConfigurator.h"

#include <Preferences.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Preferences.h>
class ActuatorController;

class FirebaseHandler {
public:
// Adicione estas constantes e variáveis à classe FirebaseHandler
    FirebaseHandler();
    ~FirebaseHandler();
    Preferences preferences;
    WiFiUDP ntpUDP;
    NTPClient timeClient;
    const char* NAMESPACE = "dados_sensores";
    const int MAX_REGISTROS = 50;
    bool nvsInicializada = false;

    unsigned long ntpLastUpdate = 0;
    const unsigned long NTP_UPDATE_INTERVAL = 3600000; // Atualizar a cada hora
    bool ntpInitialized = false;
    bool inicializarNVS();
    void refreshToken();
    void begin(const String& apiKey, const String& email, const String& password, const String& databaseUrl);
    bool isAuthenticated() const;
    bool authenticate(const String& email, const String& password);
    void resetAuthAttempts();
    void atualizarEstadoAtuadores(bool rele1, bool rele2, bool rele3, bool rele4, bool ledsLigado, int ledsWatts, bool umidLigado);
    bool permissaoUser(const String& userUID, const String& estufaID);
    void criarEstufaInicial(const String& usuarioCriador, const String& usuarioAtual);
    bool estufaExiste(const String& estufaId);
    void verificarEstufa();
    void verificarPermissoes();
    void setWiFiConfigurator(WiFiConfigurator* wifiConfig);
    void handleTokenError();
    void enviarDadosSensores(float temp, float umid, int co2, int co, int lux, int tvocs, bool waterLevel);
    void verificarComandos(ActuatorController& actuators);
    void RecebeSetpoint(ActuatorController& actuators);
    void seraQeuCrio();
    bool loadFirebaseCredentials(String& email, String& password);
    void refreshTokenIfNeeded();
    String getFormattedDateTime();
    unsigned long getCurrentTimestamp();
    void salvarDadosLocalmente(float temp, float umid, int co2, int co, int lux, int tvocs, unsigned long timestamp);
    void limitarHistorico();
    void enviarDadosLocais();
 
    FirebaseData fbdo;
    String estufaId;
    String userUID;
    bool authenticated = false;
    
    static String getEstufasPath() { return "/estufas/"; }
    static String getUsuariosPath() { return "/Usuarios/"; }
    void enviarHeartbeat();
    unsigned long getLastHeartbeatTime() const;
    bool enviarDadosParaHistorico(float temp, float umid, int co2, int co, int lux, int tvocs);
private:
    String getMacAddress();
    WiFiConfigurator* wifiConfig = nullptr;
    FirebaseAuth auth;
    FirebaseConfig config;
    const String FIREBASE_API_KEY = "AIzaSyDkPzzLHykaH16FsJpZYwaNkdTuOOmfnGE";
    const String DATABASE_URL = "pfi-ifungi-default-rtdb.firebaseio.com";

    bool initialized = false;
    unsigned long lastAuthAttempt = 0;
    int authAttempts = 0;
    static const int MAX_AUTH_ATTEMPTS = 3;
    static const unsigned long AUTH_RETRY_DELAY = 300000;
    static const int TOKEN_REFRESH_INTERVAL = 30 * 60 * 1000;
    unsigned long lastTokenRefresh = 0;
    unsigned long lastHeartbeatTime = 0;
    const unsigned long HEARTBEAT_INTERVAL = 30000; // 30 segundos
    unsigned long lastHistoricoTime = 0;
    const unsigned long HISTORICO_INTERVAL = 300000; // 5 minutos entre registros
};

#endif