#ifndef GREENHOUSE_SYSTEM_H
#define GREENHOUSE_SYSTEM_H

#include "DeviceUtils.h"
#include <FirebaseESP32.h>
#include <nvs_flash.h>
#include "ActuatorController.h"
#include <Preferences.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

class ActuatorController;

class FirebaseHandler {
public:
    FirebaseHandler();
    ~FirebaseHandler();
    
    Preferences preferences;
    WiFiUDP ntpUDP;
    NTPClient timeClient;
    const char* NAMESPACE = "sensor_data";
    const int MAX_RECORDS = 50;
    bool nvsInitialized = false;

    bool initializeNVS();
    bool authenticate(const String& email, const String& password);
    void updateActuatorState(bool relay1, bool relay2, bool relay3, bool relay4, bool ledsOn, int ledsWatts, bool humidifierOn);
    bool checkUserPermission(const String& userUID, const String& greenhouseID);
    void createInitialGreenhouse(const String& creatorUser, const String& currentUser);
    bool greenhouseExists(const String& greenhouseId);
    void verifyGreenhouse();
    void sendSensorData(float temp, float humidity, int co2, int co, int lux, int tvocs, bool waterLevel);
    void receiveSetpoints(ActuatorController& actuators);
    bool loadFirebaseCredentials(String& email, String& password);
    void refreshTokenIfNeeded();
    String getFormattedDateTime();
    unsigned long getCurrentTimestamp();
    void saveDataLocally(float temp, float humidity, int co2, int co, int lux, int tvocs, unsigned long timestamp);
    void sendLocalData();
    bool isGreenhouseStructureComplete(const String& greenhouseId);

    FirebaseData fbdo;
    String greenhouseId;
    String userUID;
    bool authenticated = false;
    
    static String getGreenhousesPath() { return "/greenhouses/"; }
    static String getUsersPath() { return "/Users/"; }
    void sendHeartbeat();
    bool sendDataToHistory(float temp, float humidity, int co2, int co, int lux, int tvocs);
    
    bool isAuthenticated() const { return authenticated; }
    bool isFirebaseReady() const { return Firebase.ready(); }

    // 🔥 NOVAS FUNÇÕES PARA DEBUG E CALIBRAÇÃO
    bool getDebugMode();
    void getManualActuatorStates(bool& relay1, bool& relay2, bool& relay3, bool& relay4, bool& ledsOn, int& ledsIntensity, bool& humidifierOn);


private:
    String getMacAddress();

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
    const unsigned long HEARTBEAT_INTERVAL = 30000;
    unsigned long lastHistoryTime = 0;
    const unsigned long HISTORY_INTERVAL = 300000;
};

#endif