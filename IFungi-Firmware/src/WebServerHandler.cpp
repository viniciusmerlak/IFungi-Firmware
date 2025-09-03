#include "WebServerHandler.h"
#include <Preferences.h>
const String WebServerHandler::FIREBASE_API_KEY = "AIzaSyDkPzzLHykaH16FsJpZYwaNkdTuOOmfnGE";
const String WebServerHandler::DATABASE_URL = "pfi-ifungi-default-rtdb.firebaseio.com";


String WebServerHandler::errorPage(const String& message) {
    return "<!DOCTYPE html>"
           "<html lang='pt-BR'>"
           "<head>"
           "<meta charset='UTF-8'>"
           "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
           "<title>Erro</title>"
           "<style>"
           "body { font-family: Arial, sans-serif; text-align: center; padding: 50px; background-color: #f5f5f5; }"
           ".error-box { background: white; padding: 30px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); max-width: 500px; margin: 0 auto; }"
           "h1 { color: #f44336; }"
           ".btn { background: #ff5722; color: white; padding: 10px 20px; text-decoration: none; border-radius: 5px; display: inline-block; margin-top: 20px; }"
           ".btn:hover { background: #e64a19; }"
           "</style>"
           "</head>"
           "<body>"
           "<div class='error-box'>"
           "<h1>❌ Erro</h1>"
           "<p>" + message + "</p>"
           "<a href='/firebase-config' class='btn'>Voltar</a>"
           "</div>"
           "</body>"
           "</html>";
}
void WebServerHandler::handleResetAuth() {
    firebaseHandler.resetAuthAttempts();
    Serial.println("Tentativas de autenticação resetadas!");
    server.send(200, "text/plain", "Tentativas resetadas com sucesso!");
}

bool WebServerHandler::getStoredFirebaseCredentials(String& email, String& password) {
    return loadFirebaseCredentials(email, password);
}

WebServerHandler::WebServerHandler(WiFiConfigurator& wifiConfig, FirebaseHandler& fbHandler) 
    : server(80), wifiConfigurator(wifiConfig), firebaseHandler(fbHandler), wifiConnected(false) {
}

void WebServerHandler::saveFirebaseCredentials(const String& email, const String& password) {
    Preferences preferences;
    if(!preferences.begin("firebase-creds", false)) {
        Serial.println("[ERRO] Falha ao acessar NVS para salvar credenciais");
        return;
    }
    
    preferences.putString("email", email);
    preferences.putString("password", password);
    preferences.end();
    Serial.println("Credenciais do Firebase salvas com sucesso");
}

bool WebServerHandler::loadFirebaseCredentials(String& email, String& password) {
    Preferences preferences;
    if(!preferences.begin("firebase-creds", true)) {
        Serial.println("[AVISO] Namespace 'firebase-creds' não encontrado");
        return false;
    }
    
    email = preferences.getString("email", "");
    password = preferences.getString("password", "");
    preferences.end();
    
    bool credentialsValid = !email.isEmpty() && !password.isEmpty();
    if(!credentialsValid) {
        static bool warningShown = false;
        if(!warningShown) {
            Serial.println("[AVISO] Credenciais do Firebase não configuradas");
            warningShown = true;
        }
    }
    return credentialsValid;
}

void WebServerHandler::begin(bool wifiConnected) {
    this->wifiConnected = wifiConnected;
    
    server.on("/", HTTP_GET, [this]() {
        if(this->wifiConnected) {
            this->handleFirebaseConfig();
        } else {
            this->handleWiFiConfig();
        }
    });
    
    server.on("/wifi-config", HTTP_POST, [this]() { this->handleWiFiConfig(); });
    server.on("/firebase-config", HTTP_POST, [this]() { this->handleFirebaseConfig(); });
    server.on("/reset-auth", HTTP_GET, [this]() { this->handleResetAuth(); });
    server.onNotFound([this]() { this->handleNotFound(); });
    
    server.begin();
}

void WebServerHandler::handleClient() {
    server.handleClient();
}

void WebServerHandler::handleRoot() {
    if(wifiConnected) {
        handleFirebaseConfig();
    } else {
        handleWiFiConfig();
    }
}

void WebServerHandler::handleWiFiConfig() {
    if(server.method() == HTTP_POST) {
        String ssid = server.arg("ssid");
        String password = server.arg("password");
        
        if(wifiConfigurator.connectToWiFi(ssid.c_str(), password.c_str(), true)) {
            wifiConfigurator.saveCredentials(ssid.c_str(), password.c_str());
            server.send(200, "text/html", 
                "<!DOCTYPE html>"
                "<html lang='pt-BR'>"
                "<head>"
                "<meta charset='UTF-8'>"
                "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
                "<title>Conexão WiFi</title>"
                "<style>"
                ":root{--primary:#4285F4;--secondary:#34A853;--light:#E8F0FE;--dark:#1A73E8;}"
                "*{margin:0;padding:0;box-sizing:border-box;font-family:'Segoe UI',Roboto,Oxygen,Ubuntu,sans-serif;}"
                "body{background:linear-gradient(135deg,#E8F0FE 0%,#D2E3FC 100%);min-height:100vh;display:flex;justify-content:center;align-items:center;padding:20px;text-align:center;}"
                ".wifi-card{background:white;border-radius:12px;box-shadow:0 8px 24px rgba(0,0,0,0.1);padding:2.5rem;max-width:450px;width:100%;animation:fadeIn 0.5s ease-out;}"
                "@keyframes fadeIn{from{opacity:0;}to{opacity:1;}}"
                "h1{color:var(--dark);margin-bottom:1rem;font-size:2rem;font-weight:600;}"
                "p{color:#5F6368;font-size:1.1rem;margin-bottom:1.5rem;}"
                ".wifi-icon{width:80px;height:80px;margin:0 auto 1.5rem;display:block;color:var(--primary);}"
                ".spinner{margin:2rem auto;width:50px;height:50px;border:5px solid #f3f3f3;border-top:5px solid var(--primary);border-radius:50%;animation:spin 1s linear infinite;}"
                "@keyframes spin{0%{transform:rotate(0deg);}100%{transform:rotate(360deg);}}"
                "</style>"
                "</head>"
                "<body>"
                "<div class='wifi-card'>"
                "<svg class='wifi-icon' xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='currentColor'>"
                "<path d='M12 3C7.79 3 3.7 4.41.38 7 4.41 12.06 7.89 16.37 12 21.5c4.08-5.08 7.49-9.42 11.62-14.5C20.32 4.41 16.22 3 12 3zm0 2c3.07 0 6.09.86 8.71 2.45l-3.21 3.98C16.26 10.74 14.37 10 12 10c-2.38 0-4.26.75-5.5 1.43L3.27 7.44C5.91 5.85 8.93 5 12 5z'/>"
                "</svg>"
                "<h1>WiFi Conectado!</h1>"
                "<p>Reiniciando para acessar modo normal...</p>"
                "<div class='spinner'></div>"
                "</div>"
                "</body>"
                "</html>"
            );
            delay(1000);
            ESP.restart();
        } else {
            server.send(200, "text/html", "<h1>Falha na conexão</h1><a href='/'>Tentar novamente</a>");
        }
    } else {
        String html = "<!DOCTYPE html>"
        "<html lang='pt-BR'>"
        "<head>"
        "<meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
        "<title>Configuração WiFi</title>"
        "<style>"
        ":root{--primary:#4361ee;--secondary:#3f37c9;--light:#f8f9fa;--dark:#212529;--success:#4cc9f0;}"
        "*{margin:0;padding:0;box-sizing:border-box;font-family:'Segoe UI',Tahoma,Geneva,Verdana,sans-serif;}"
        "body{background:linear-gradient(135deg,#f5f7fa 0%,#c3cfe2 100%);min-height:100vh;display:flex;justify-content:center;align-items:center;padding:20px;}"
        ".container{background:white;border-radius:15px;box-shadow:0 10px 30px rgba(0,0,0,0.1);width:100%;max-width:450px;padding:40px;text-align:center;animation:fadeIn 0.5s ease-in-out;}"
        "@keyframes fadeIn{from{opacity:0;transform:translateY(-20px);}to{opacity:1;transform:translateY(0);}}"
        "h1{color:var(--dark);margin-bottom:30px;font-weight:600;font-size:28px;}"
        ".logo{width:80px;height:80px;margin-bottom:20px;fill:var(--primary);}"
        ".form-group{margin-bottom:20px;text-align:left;}"
        "label{display:block;margin-bottom:8px;color:var(--dark);font-weight:500;}"
        "input{width:100%;padding:15px;border:2px solid #e9ecef;border-radius:8px;font-size:16px;transition:all 0.3s;}"
        "input:focus{border-color:var(--primary);outline:none;box-shadow:0 0 0 3px rgba(67,97,238,0.2);}"
        "input::placeholder{color:#adb5bd;}"
        "button{background:linear-gradient(to right,var(--primary),var(--secondary));color:white;border:none;padding:15px;width:100%;border-radius:8px;font-size:16px;font-weight:600;cursor:pointer;transition:all 0.3s;margin-top:10px;box-shadow:0 4px 6px rgba(0,0,0,0.1);}"
        "button:hover{background:linear-gradient(to right,var(--secondary),var(--primary));transform:translateY(-2px);box-shadow:0 6px 12px rgba(0,0,0,0.15);}"
        "button:active{transform:translateY(0);}"
        ".footer{margin-top:30px;color:#6c757d;font-size:14px;}"
        "@media (max-width:480px){.container{padding:30px 20px;}h1{font-size:24px;}}"
        "</style>"
        "</style>"
        "</head>"
        "<body>"
        "<div class='container'>"
        "<svg class='logo' xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'>"
        "<path d='M12 3C6.95 3 3.15 4.85 0 7.23L12 22.5 24 7.25C20.85 4.87 17.05 3 12 3zm0 2c3.45 0 6.55 1.34 8.9 3.48L12 18.5 3.1 8.48C5.45 6.34 8.55 5 12 5z'/>"
        "</svg>"
        "<h1>Configuração WiFi</h1>"
        "<form action='/wifi-config' method='post'>"
        "<div class='form-group'>"
        "<label for='ssid'>Nome da Rede (SSID)</label>"
        "<input type='text' id='ssid' name='ssid' placeholder='Digite o nome da rede WiFi' required>"
        "</div>"
        "<div class='form-group'>"
        "<label for='password'>Senha</label>"
        "<input type='password' id='password' name='password' placeholder='Digite a senha (se necessário)'>"
        "</div>"
        "<button type='submit'>Conectar</button>"
        "</form>"
        "<div class='footer'>Conecte-se à sua rede WiFi</div>"
        "</div>"
        "</body>"
        "</html>";

        server.send(200, "text/html", html);
    }
}

void WebServerHandler::handleFirebaseConfig() {
    // Se não estiver conectado ao WiFi, redireciona
    if(!wifiConnected) {
        server.sendHeader("Location", "/");
        server.send(302, "text/plain", "Redirecionando para configuração WiFi...");
        return;
    }

    if(server.method() == HTTP_POST) {
        // Obtém credenciais do formulário
        String email = server.arg("email");
        String password = server.arg("password");

        // Validação básica dos campos
        if(email.isEmpty() || password.isEmpty()) {
            server.send(400, "text/html", errorPage("Email e senha são obrigatórios"));
            return;
        }

        // Tenta autenticar primeiro (antes de salvar)
        if(firebaseHandler.authenticate(email, password)) {
            // Se autenticou, salva as credenciais
            saveFirebaseCredentials(email, password);
            
            // Inicializa o Firebase com as novas credenciais
            firebaseHandler.begin(FIREBASE_API_KEY, email, password, DATABASE_URL);
            
            // Verifica/cria a estufa
            firebaseHandler.verificarEstufa();

            // Página de sucesso com redirecionamento
            server.send(200, "text/html", 
                "<!DOCTYPE html>"
                "<html lang='pt-BR'>"
                "<head>"
                "<meta charset='UTF-8'>"
                "<meta http-equiv='refresh' content='3;url=/' />"
                "<title>Sucesso</title>"
                "<style>"
                "body { font-family: Arial, sans-serif; text-align: center; padding: 50px; background-color: #f5f5f5; }"
                ".success-box { background: white; padding: 30px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); max-width: 500px; margin: 0 auto; }"
                "h1 { color: #4CAF50; }"
                ".spinner { margin: 20px auto; width: 40px; height: 40px; border: 4px solid #f3f3f3; border-top: 4px solid #3498db; border-radius: 50%; animation: spin 1s linear infinite; }"
                "@keyframes spin { 0% { transform: rotate(0deg); } 100% { transform: rotate(360deg); } }"
                "</style>"
                "</head>"
                "<body>"
                "<div class='success-box'>"
                "<h1>✅ Autenticação Bem-sucedida</h1>"
                "<p>Credenciais salvas com sucesso!</p>"
                "<p>Redirecionando para a página principal...</p>"
                "<div class='spinner'></div>"
                "</div>"
                "</body>"
                "</html>"
            );
        } else {
            // Se falhar, limpa as credenciais inválidas
            Preferences preferences;
            preferences.begin("firebase-creds", false);
            preferences.clear();
            preferences.end();

            // Página de erro com opção para tentar novamente
            server.send(401, "text/html", 
                "<!DOCTYPE html>"
                "<html lang='pt-BR'>"
                "<head>"
                "<meta charset='UTF-8'>"
                "<title>Falha na Autenticação</title>"
                "<style>"
                "body { font-family: Arial, sans-serif; text-align: center; padding: 50px; background-color: #f5f5f5; }"
                ".error-box { background: white; padding: 30px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); max-width: 500px; margin: 0 auto; }"
                "h1 { color: #f44336; }"
                ".btn { background: #ff5722; color: white; padding: 10px 20px; text-decoration: none; border-radius: 5px; display: inline-block; margin-top: 20px; }"
                ".btn:hover { background: #e64a19; }"
                "</style>"
                "</head>"
                "<body>"
                "<div class='error-box'>"
                "<h1>❌ Falha na Autenticação</h1>"
                "<p>As credenciais fornecidas são inválidas ou não podem ser verificadas.</p>"
                "<p>Por favor, verifique seu email e senha e tente novamente.</p>"
                "<a href='/firebase-config' class='btn'>Tentar Novamente</a>"
                "</div>"
                "</body>"
                "</html>"
            );
        }
    } else {
        // Mostra o formulário de configuração do Firebase
        server.send(200, "text/html", 
            "<!DOCTYPE html>"
            "<html lang='pt-BR'>"
            "<head>"
            "<meta charset='UTF-8'>"
            "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
            "<title>Configuração do Firebase</title>"
            "<style>"
            ":root {"
            "  --primary: #FF5722;"
            "  --primary-dark: #E64A19;"
            "  --secondary: #607D8B;"
            "  --light: #f5f5f5;"
            "  --dark: #212121;"
            "  --error: #f44336;"
            "  --success: #4CAF50;"
            "}"
            "body {"
            "  background: linear-gradient(135deg, #f5f5f5 0%, #e0e0e0 100%);"
            "  min-height: 100vh;"
            "  display: flex;"
            "  justify-content: center;"
            "  align-items: center;"
            "  padding: 20px;"
            "  font-family: 'Segoe UI', Roboto, Oxygen, Ubuntu, sans-serif;"
            "}"
            ".container {"
            "  background: white;"
            "  border-radius: 12px;"
            "  box-shadow: 0 8px 24px rgba(0,0,0,0.1);"
            "  width: 100%;"
            "  max-width: 420px;"
            "  padding: 2.5rem;"
            "  text-align: center;"
            "}"
            "h1 {"
            "  color: var(--dark);"
            "  margin-bottom: 1.5rem;"
            "  font-size: 1.8rem;"
            "  font-weight: 600;"
            "}"
            ".logo {"
            "  width: 60px;"
            "  height: 60px;"
            "  margin-bottom: 1.2rem;"
            "}"
            ".form-group {"
            "  margin-bottom: 1.2rem;"
            "  text-align: left;"
            "}"
            "label {"
            "  display: block;"
            "  margin-bottom: 0.5rem;"
            "  color: var(--dark);"
            "  font-weight: 500;"
            "  font-size: 0.95rem;"
            "}"
            "input {"
            "  width: 100%;"
            "  padding: 0.8rem 1rem;"
            "  border: 2px solid #e0e0e0;"
            "  border-radius: 8px;"
            "  font-size: 1rem;"
            "  transition: all 0.3s ease;"
            "}"
            "input:focus {"
            "  border-color: var(--primary);"
            "  outline: none;"
            "  box-shadow: 0 0 0 3px rgba(255,87,34,0.2);"
            "}"
            "button {"
            "  background: linear-gradient(to right, var(--primary), var(--primary-dark));"
            "  color: white;"
            "  border: none;"
            "  padding: 0.9rem;"
            "  width: 100%;"
            "  border-radius: 8px;"
            "  font-size: 1rem;"
            "  font-weight: 600;"
            "  cursor: pointer;"
            "  transition: all 0.3s;"
            "  margin-top: 0.5rem;"
            "  box-shadow: 0 4px 6px rgba(0,0,0,0.1);"
            "}"
            "button:hover {"
            "  transform: translateY(-2px);"
            "  box-shadow: 0 6px 12px rgba(0,0,0,0.15);"
            "}"
            ".footer {"
            "  margin-top: 1.5rem;"
            "  color: #757575;"
            "  font-size: 0.85rem;"
            "}"
            "@media (max-width: 480px) {"
            "  .container { padding: 1.8rem; }"
            "}"
            "</style>"
            "</head>"
            "<body>"
            "<div class='container'>"
            "<svg class='logo' viewBox='0 0 24 24' xmlns='http://www.w3.org/2000/svg'>"
            "<path fill='#FFCA28' d='M3.89 15.672L6.255.461A.454.454 0 0 1 6.968.288l2.543 4.771z'/>"
            "<path fill='#FFA000' d='M16.678 3.11l-1.617-1.91a.456.456 0 0 0-.72 0L3.89 15.672 10.61 5.06z'/>"
            "<path fill='#F57C00' d='M6.965 18.374l-3.074-2.702a.453.453 0 0 1 0-.717L16.678 3.11l-3.21 13.16z'/>"
            "<path fill='#FFCA28' d='M18.352 8.331l-2.674-5.22-7.016 10.316 7.445 4.684a.457.457 0 0 0 .69-.415z'/>"
            "</svg>"
            "<h1>Configuração do Firebase</h1>"
            "<form action='/firebase-config' method='post'>"
            "<div class='form-group'>"
            "<label for='email'>Email</label>"
            "<input type='email' id='email' name='email' placeholder='seu@email.com' required>"
            "</div>"
            "<div class='form-group'>"
            "<label for='password'>Senha</label>"
            "<input type='password' id='password' name='password' placeholder='Digite sua senha' required>"
            "</div>"
            "<button type='submit'>Conectar ao Firebase</button>"
            "</form>"
            "<div class='footer'>Sistema de autenticação seguro</div>"
            "</div>"
            "</body>"
            "</html>"
        );
    }
}

void WebServerHandler::handleNotFound() {
    server.send(404, "text/plain", "Página não encontrada");
}