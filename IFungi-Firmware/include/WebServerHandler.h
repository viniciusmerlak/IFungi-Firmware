#ifndef WEBSERVER_HANDLER_H
#define WEBSERVER_HANDLER_H

#include <WebServer.h>
#include "WiFiConfigurator.h"
#include "FirebaseHandler.h"
#include <Preferences.h>

class WebServerHandler {
public:
    // Declaração das constantes (sem inicialização aqui)
    static const String FIREBASE_API_KEY;
    static const String DATABASE_URL;

    // Construtor
    WebServerHandler(WiFiConfigurator& wifiConfig, FirebaseHandler& fbHandler);
    
    // Métodos públicos
    void begin(bool wifiConnected);
    void handleClient();
    bool getStoredFirebaseCredentials(String& email, String& password);
    void handleResetAuth();
    String errorPage(const String& message);

private:
    // Membros privados
    WebServer server;
    WiFiConfigurator& wifiConfigurator;
    FirebaseHandler& firebaseHandler;
    bool wifiConnected;

    // Métodos privados
    void saveFirebaseCredentials(const String& email, const String& password);
    bool loadFirebaseCredentials(String& email, String& password);
    void handleRoot();
    void handleWiFiConfig();
    void handleFirebaseConfig();
    void handleNotFound();
};

#endif