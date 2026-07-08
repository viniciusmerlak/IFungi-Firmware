# Fluxo atual do sistema IFungi Firmware

Este documento descreve o funcionamento atual do firmware a partir do codigo em `src/` e `include/`.

## Visao geral

O firmware roda em um ESP32 usando Arduino/PlatformIO. A arquitetura e organizada em controladores:

- `MainController.cpp`: ponto de entrada, boot, tasks e loop principal.
- `SensorController`: leitura e saude dos sensores.
- `ActuatorController`: controle automatico/manual de reles, Peltier, LEDs, umidificador e exaustor.
- `FirebaseHandler` (`GreenhouseSystem`): autenticacao, sincronizacao com RTDB, NVS, historico, setpoints, modos, OTA e reparos.
- `LEDScheduler`: agenda fixa ou simulacao solar dos LEDs.
- `OTAHandler`: atualizacao remota do firmware.
- `RemoteLogger`: fila de logs e envio ao Firebase.
- `QRCodeGenerator` e `DeviceUtils`: geracao de QR e ID por MAC.

O sistema tem dois regimes principais:

1. **Online/autenticado**: o `loop()` principal le sensores, controla atuadores, sincroniza Firebase, historico, debug, modo de operacao, reparos e OTA.
2. **Offline/portal/sem credenciais**: a `lifeSupportTask` assume sensores e atuadores para manter a estufa funcionando sem chamar Firebase fora da `loopTask`.

## Fluxo de boot

Entrada: `setup()` em `src/MainController.cpp`.

Sequencia:

1. Inicializa Serial e imprime a versao.
2. Cria `sensorMutex`, usado para proteger `sensors.update()`.
3. Chama `setupLEDTask()`.
   - Configura `LED_BUILTIN`.
   - Cria `ledTask` no core 0.
4. Chama `firebase.initializeNVS()`.
   - Cria/prepara namespace NVS `sensor_data`.
   - Inicializa contadores de registros locais.
5. Chama `setupSensorsAndActuators()`.
   - `sensors.begin()`.
   - `actuators.begin(4, 23, 14, 18, 19, 13)`.
   - Tenta `actuators.loadSetpointsNVS()`.
   - Se nao houver setpoints na NVS, aplica defaults temporarios sem persistir.
   - Injeta ponteiro do Firebase em `actuators.setFirebaseHandler(&firebase)`.
6. Ativa `lifeSupportTaskRunning = true`.
7. Cria `lifeSupportTask` no core 0.
8. Chama `setupWiFiAndFirebase()`.
   - Conecta WiFi via WiFiManager.
   - Carrega/salva credenciais Firebase via NVS.
   - Autentica no Firebase.
   - Verifica/cria estrutura da estufa.
   - Cria/garante nos de OTA, LED schedule, modo de operacao e logs.
   - Envia dados locais pendentes e heartbeat.
   - Se tudo der certo: `lifeSupportTaskRunning = false`.
   - Se falhar: suporte de vida continua ativo.
9. Define `greenhouseID = "IFUNGI-" + getMacAddress()`.
10. Gera QR code com `qrGenerator.generateQRCode(greenhouseID)`.
11. Inicializa OTA com `otaHandler.begin(&firebase, greenhouseID, FIRMWARE_VERSION, 60000)`.
12. Loga boot completo via `RLOG_FMT`.

## Tasks paralelas

### `ledTask`

Roda no core 0 e indica estado:

- Sem WiFi: LED desligado.
- Portal/AP: pisca rapido.
- WiFi conectado sem Firebase: pisca lento.
- WiFi + Firebase autenticado: LED aceso.

### `lifeSupportTask`

Roda no core 0 e so age quando `lifeSupportTaskRunning == true`.

Responsabilidade: manter a estufa operacional durante portal cativo, falha de WiFi, credenciais ausentes ou Firebase indisponivel.

Ciclo:

1. A cada `SENSOR_READ_INTERVAL` (2 s):
   - Tenta `xSemaphoreTake(sensorMutex)`.
   - Chama `sensors.update()`.
   - Libera mutex.
2. A cada `ACTUATOR_CONTROL_INTERVAL` (5 s):
   - Chama `actuators.applyLEDSchedule(firebase.getCurrentTimestamp())`.
   - Chama `actuators.controlAutomatically(...)` com `allowFirebaseWrite=false`.

Ponto importante: essa task nao escreve no Firebase. O codigo evita chamadas TLS/lwIP fora da `loopTask`.

### `ledPwmTask`

Criada dentro de `ActuatorController::begin()`, tambem no core 0.

Ela faz rampa suave da intensidade dos LEDs:

- Compara `currentLEDIntensity` com `targetLEDIntensity`.
- Sobe/desce em passos de 5.
- Escreve PWM invertido no hardware (`255` no pino = apagado, `0` = brilho maximo).

## Loop principal

Entrada: `loop()` em `src/MainController.cpp`.

Quando `lifeSupportTaskRunning == false`, o loop executa:

1. `handleSensors()`
2. `handleActuators()`
3. `handleFirebase()`
4. `handleHistoryAndLocalData()`
5. `handleDebugAndCalibration()`
6. `handleOperationMode()`
7. `handleRepairAndOTA()`
8. `verifyConnectionStatus()`
9. `otaHandler.handle()`

Sempre executa, mesmo em modo offline:

1. `handleWiFiReconnection()`
2. `RemoteLogger::flush()`
3. `delay(10)`

## Fluxo dos sensores

Inicializacao: `SensorController::begin()`.

Sensores/pinos:

- MQ-7 em GPIO 35.
- DHT22 em GPIO 33.
- LDR em GPIO 34.
- Nivel de agua em GPIO 32.
- CCS811 via biblioteca Adafruit.

No boot:

1. Configura pinos.
2. Inicia MQ-7 com warmup de 120 s.
3. Inicia DHT22 e tenta ate 5 leituras validas.
4. Se DHT falhar:
   - `dhtOK = false`.
   - `temperature = NAN`.
   - `humidity = 100.0`.
   - Isso bloqueia Peltier e desliga umidificador por seguranca.
5. Inicia CCS811 com ate 3 tentativas.
6. Zera contadores e leituras.

Atualizacao: `SensorController::update()`, chamada a cada 2 s.

O que acontece:

- Sempre le LDR e MQ-7 ADC.
- A cada 2 ciclos, le DHT22.
  - Com 3 falhas consecutivas, marca DHT como inoperante.
  - Tenta recuperacao a cada 30 s.
- A cada 3 ciclos, le CCS811.
  - Com 3 falhas, marca CCS811 como inoperante.
  - Tenta recuperacao a cada 30 s.
- MQ-7 retorna `0` durante warmup; depois estima ppm por curva `Rs/R0`.
- Nivel de agua esta temporariamente desabilitado no codigo:
  - `waterLevel = false`, interpretado como agua OK.

## Fluxo dos atuadores

Inicializacao: `ActuatorController::begin(...)`.

Mapeamento atual chamado pelo setup:

- LED: GPIO 4.
- Rele 1: GPIO 23.
- Rele 2: GPIO 14.
- Rele 3: GPIO 18.
- Rele 4: GPIO 19.
- Servo: GPIO 13.

Estados:

- Rele 1 e 2 controlam Peltier.
- Rele 3 controla umidificador.
- Rele 4 controla exaustor.
- Servo abre/fecha passagem do exaustor.
- LED usa PWM com rampa por task.

Controle automatico: `ActuatorController::controlAutomatically(...)`.

Sequencia interna:

1. Se `debugMode == true`, retorna sem controle automatico.
2. Atualiza permissao de escrita Firebase (`allowFirebaseWrite`).
3. Controla Peltier:
   - Se modo atual desabilita Peltier: desliga.
   - Se DHT nao esta saudavel: desliga por seguranca.
   - Se temperatura e `NAN`: desliga por seguranca.
   - Se temperatura < `tempMin - 0.5`: aquece.
   - Se temperatura > `tempMax + 0.5`: resfria.
   - Se temperatura voltou para faixa: desliga.
   - Aquecimento continuo tem limite de 5 min e cooldown de 1 min.
4. Controla umidificador:
   - Se modo desabilita: desliga.
   - Se agua baixa: desliga.
   - Se DHT/umidade invalida: desliga.
   - Se umidade < `humidityMin - 2`: liga.
   - Se umidade > `humidityMax + 2`: desliga.
   - Dentro da faixa, mantem estado.
5. Controla LEDs:
   - Se modo desabilita: desliga.
   - Se scheduler ativo: segue agenda/simulacao solar.
   - Senao usa LDR: luz abaixo do setpoint liga em intensidade 255.
6. Atualiza Firebase com estado dos atuadores, se permitido.
7. Controla exaustor:
   - Se modo forca exaustor: abre servo e liga rele 4.
   - Se CO, CO2 ou TVOCs acima dos setpoints: abre/liga.
   - Caso contrario: fecha/desliga.

## Modos de operacao

Lidos em `handleOperationMode()` a cada 5 s via `firebase.receiveOperationMode(actuators)`.

O Firebase usa:

`/greenhouses/<ID>/operation_mode/mode`

Valores aceitos:

- `manual`
- `incubacao`
- `frutificacao`
- `secagem`
- `manutencao`

Quando muda:

1. `FirebaseHandler::receiveOperationMode()` converte string para enum.
2. Chama `actuators.applyOperationMode(newMode)`.
3. Se modo nao for manual, garante/aplica agenda de LED no Firebase.

Efeito dos presets:

- Manual: usa setpoints do app, sem restricoes especiais.
- Incubacao: umidificador e LEDs desligados; Peltier ativo; exaustor so por gases.
- Frutificacao: umidificador ativo; LEDs em ciclo 06:00-18:00; Peltier ativo; CO2 mais baixo.
- Secagem: umidificador/LEDs/Peltier desligados; exaustor forcado.
- Manutencao: tudo desligado, faixas largas para evitar acionamentos.

## Firebase e sincronizacao

Autenticacao: `FirebaseHandler::authenticate(email, password)`.

O que faz:

1. Configura API key, URL do banco, email e senha.
2. Chama `Firebase.begin(&config, &auth)`.
3. Aguarda `Firebase.ready()` por ate 30 s.
4. Quando autentica:
   - `authenticated = true`.
   - `userUID = auth.token.uid`.
   - `greenhouseId = "IFUNGI-" + MAC`.

Depois da autenticacao, `setupWiFiAndFirebase()` chama:

- `verifyGreenhouse()`
- `checkUserPermission(...)`
- `ensureOTANodeExists()`
- `ensureLEDScheduleExists(actuators)`
- `ensureOperationModeExists(actuators)`
- `initLogger(LOG_INFO)`
- `sendLocalData()`
- `sendHeartbeat()`

### `handleFirebase()`

Roda a cada 5 s quando online e autenticado.

Chamadas:

1. `firebase.sendSensorData(...)`
   - Atualiza leituras atuais no RTDB.
2. `firebase.updateSensorHealth(...)`
   - Publica saude dos sensores.
3. `firebase.updateActuatorState(...)`
   - Publica estado atual dos atuadores.
4. Se tudo foi OK:
   - `firebase.receiveSetpoints(actuators)`
   - `firebase.receiveLEDSchedule(actuators)`
5. Se 3 falhas consecutivas ocorrerem:
   - `firebase.recoverFbdo()`

Tambem envia heartbeat a cada 30 s por `firebase.sendHeartbeat()`.

### Setpoints

`FirebaseHandler::receiveSetpoints(actuators)` le os setpoints do Firebase.

Quando detecta mudanca:

1. Chama `actuators.applySetpoints(...)`.
2. Persiste na NVS (`persistToNVS=true`).

Regra importante: Firebase e a fonte de verdade. A NVS acelera boot/offline, mas os valores do Firebase sobrescrevem divergencias assim que lidos com sucesso.

### Dados locais e historico

`handleHistoryAndLocalData()`:

- A cada 5 min chama `sendDataToHistory()`.
- A cada 60 s, se offline, chama `saveDataLocally()`.

Se online:

- `FirebaseHandler::sendDataToHistory(...)` grava em `/historico/<greenhouseId>/<timestamp>`.

Se offline/falhou:

- `FirebaseHandler::saveDataLocally(...)` grava ate 50 registros na NVS.
- Ao reconectar, `sendLocalData()` tenta enviar registros pendentes e compacta/remover os enviados.

## WiFi e reconexao

Inicializacao: `setupWiFiAndFirebase()`.

Fluxo:

1. Cria `WiFiManager` local.
2. Define timeout do portal em 180 s e conexao em 30 s.
3. Preenche portal com email salvo, se existir.
4. Chama `wm.autoConnect(...)`.
5. Se falhar, abre portal cativo.
6. Se conectar, tenta carregar/salvar credenciais Firebase.
7. Autentica no Firebase com ate 2 tentativas.
8. Se credenciais forem invalidas:
   - Limpa NVS `firebase-creds`.
   - Abre portal.
   - Mantem suporte de vida ativo.

Reconexao continua: `handleWiFiReconnection()`.

- Se WiFi cair: `WiFi.reconnect()`.
- Se WiFi voltou mas Firebase nao autenticou:
  - Carrega credenciais.
  - Tenta `firebase.authenticate(...)`.
  - Se der certo, inicializa logger, heartbeat e desliga `lifeSupportTaskRunning`.

Verificacao periodica: `verifyConnectionStatus()`.

- A cada 30 s verifica WiFi.
- Ao reconectar, envia dados locais pendentes.
- Se Firebase autenticado mas nao pronto, tenta `refreshTokenIfNeeded()`.

## Debug, manual e dev mode

Entrada: `handleDebugAndCalibration()`, a cada 2 s.

Fluxo:

1. Le `firebase.getDebugMode()`.
2. Se mudou:
   - `actuators.setDebugMode(currentDebugMode)`.
   - Ao sair do debug, atualiza estados no Firebase.
3. Se debug ativo:
   - Le `firebase.getDevModeSettings(...)`.
   - Aplica em `actuators.setDevModeSettings(...)`.
   - Le `firebase.getManualActuatorStates(...)`.
   - Corrige caso invalido: rele2 ligado com rele1 desligado.
   - Se houve mudanca, chama `actuators.setManualStates(...)`.
4. Sempre chama `actuators.handleDevMode()`.

No `ActuatorController`, debug mode:

- Bloqueia temporariamente escrita Firebase para evitar disputa.
- Permite controle manual dos reles e LEDs.
- Dev mode permite `analogRead`, `digitalWrite` ou PWM em GPIO escolhido, bloqueando pinos criticos.

## LED schedule

Firebase:

`/greenhouses/<ID>/led_schedule`

Campos:

- `scheduleEnabled`
- `solarSimEnabled`
- `onHour`
- `onMinute`
- `offHour`
- `offMinute`
- `intensity`

Fluxo:

1. `firebase.receiveLEDSchedule(actuators)` atualiza `actuators.ledScheduler`.
2. `actuators.applyLEDSchedule(timestamp)` chama `ledScheduler.update(timestamp, debugMode)`.
3. Se scheduler ativo e debug desligado:
   - `wantsLEDsOn()` e `getIntensity()` definem alvo do LED.
4. Mudancas sao persistidas em NVS por `persistLEDScheduleIfChanged()`.

Prioridade do scheduler:

1. Debug ativo: scheduler fica inativo.
2. Simulacao solar ativa: ignora timer simples.
3. Timer simples ativo: liga na janela configurada com intensidade fixa.
4. Nenhum ativo: controle automatico usa LDR.

## OTA

Inicializacao:

`otaHandler.begin(&firebase, greenhouseID, FIRMWARE_VERSION, 60000)`

Execucao:

`otaHandler.handle()` no loop online/autenticado.

Fluxo:

1. A cada 60 s, chama `_checkForUpdate()`.
2. Le `/greenhouses/<ID>/ota/available`.
3. Se `available == true`, le `version` e `url`.
4. Se versao for igual a atual, limpa `available`.
5. Rejeita URL que nao comece com `https://`.
6. Se valida, chama `_downloadAndInstall(url)`.
7. Baixa via `HTTPClient` + `WiFiClientSecure`.
8. Escreve na particao OTA com `Update.write(...)`.
9. Se sucesso:
   - `_reportResult(true)`.
   - `ESP.restart()`.
10. Se falha:
   - `_reportResult(false)`.
   - Status `FAILED`.

## RemoteLogger

Inicializacao:

`firebase.initLogger(LOG_INFO)` chama `RemoteLogger::init(&logFbdo, greenhouseId, LOG_INFO)` e garante `/logs`.

Uso:

- Macros `RLOG_INFO`, `RLOG_WARN`, `RLOG_ERROR`, `RLOG_CRITICAL`, `RLOG_FMT`.
- Sempre imprime no Serial.
- Logs com nivel >= minimo entram em fila RAM.

Flush:

`RemoteLogger::flush()` e chamado em todo `loop()`.

O que faz:

1. Se fila vazia ou Firebase indisponivel, retorna.
2. Respeita intervalo minimo de 800 ms.
3. Envia uma entrada por chamada para `/greenhouses/<ID>/logs/recent/<slot>`.
4. Atualiza `head` e `count`.
5. Erros e criticos tambem vao para `/logs/last_errors/<slot>`.

## QR Code e ID

`DeviceUtils::getMacAddress()` obtem o MAC do ESP32.

No setup:

- `greenhouseID = "IFUNGI-" + getMacAddress()`.
- `qrGenerator.generateQRCode(greenhouseID)`.

O QR e gerado no Serial, representando o ID da estufa.

## Tabela de chamadas principais

| Quando | Funcao chamada | O que aciona depois |
| --- | --- | --- |
| Boot | `setup()` | NVS, LED task, sensores, atuadores, life support, WiFi/Firebase, QR, OTA |
| Boot | `setupSensorsAndActuators()` | `sensors.begin()`, `actuators.begin()`, setpoints NVS/defaults |
| Boot/reconexao | `setupWiFiAndFirebase()` | WiFiManager, credenciais, `firebase.authenticate()`, nos RTDB |
| Sempre | `ledTask()` | Pisca LED conforme WiFi/Firebase |
| Offline/portal | `lifeSupportTask()` | `sensors.update()`, `actuators.controlAutomatically(..., false)` |
| Loop online | `handleSensors()` | `sensors.update()` protegido por mutex |
| Loop online | `handleActuators()` | LED schedule e controle automatico |
| Loop online | `handleFirebase()` | Sensores, saude, atuadores, setpoints, LED schedule, heartbeat |
| Loop online | `handleHistoryAndLocalData()` | Historico remoto ou NVS local |
| Loop online | `handleDebugAndCalibration()` | Debug/manual/dev mode |
| Loop online | `handleOperationMode()` | Le modo do Firebase e aplica preset |
| Loop online | `handleRepairAndOTA()` | Repara campos e garante nos auxiliares |
| Loop online | `otaHandler.handle()` | Verifica, baixa e instala firmware |
| Loop sempre | `handleWiFiReconnection()` | Reconecta WiFi/Firebase |
| Loop sempre | `RemoteLogger::flush()` | Envia logs pendentes |

## Resumo do fluxo em texto

Ao ligar, o ESP32 prepara NVS, sensores e atuadores, cria tasks auxiliares e ja deixa a `lifeSupportTask` ativa. Enquanto o WiFiManager tenta conectar ou abre portal, a estufa continua lendo sensores e acionando atuadores localmente. Quando WiFi e Firebase autenticam com sucesso, o sistema cria/verifica a estrutura da estufa no RTDB, envia dados pendentes, inicia logger e desativa o suporte de vida. A partir dai, o `loop()` principal passa a ler sensores, controlar atuadores, sincronizar dados e estados com Firebase, receber setpoints, receber agenda de LEDs, aplicar modos de operacao, enviar historico, verificar OTA e manter heartbeat. Se rede ou autenticacao falham, o sistema tenta reconectar sem parar o funcionamento local.

