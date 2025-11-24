# IFungi-Firmware

IFungi é uma estufa automatizada que controla temperatura, umidade, gases e iluminação remotamente com um custo acessível. Este repositório contém o código do ESP-32 que controla a estufa, desenvolvido utilizando PlatformIO e VSCode. Inclui documentação utilizando Doxygen.

## Arquitetura do Sistema

O sistema IFungi é composto por módulos especializados que trabalham em conjunto para automatizar o controle ambiental da estufa:

### Módulos Principais

- **MainController.cpp** - Controlador principal que orquestra todos os componentes
- **SensorController.cpp** - Gerencia leituras de todos os sensores ambientais
- **ActuatorController.cpp** - Controla todos os atuadores e lógica de automação
- **GreenhouseSystem.cpp** - Handler para comunicação com Firebase
- **DeviceUtils.cpp** - Utilitários do dispositivo (MAC address, etc.)
- **QRCodeGenerator.cpp** - Geração de QR Code para identificação

## Funcionalidades

### Controle Automático Inteligente
- **Controle de Temperatura**: Sistema Peltier bidirecional (aquecimento/resfriamento) com proteção térmica
- **Controle de Umidade**: Umidificador com proteção por sensor de nível de água
- **Controle de Iluminação**: LEDs com efeito fade in/out baseado em luminosidade
- **Ventilação e Qualidade do Ar**: Exaustor com dampers servo-controlados baseado em níveis de CO2, CO e TVOCs
- **Sistema de Histerese**: Evita oscilações frequentes dos atuadores

### Comunicação e Conectividade
- **WiFi Manager**: Configuração via portal web com fallback para modo AP
- **Firebase Realtime**: Sincronização em tempo real de dados e estados
- **Modo Offline**: Funcionamento autônomo com armazenamento local em NVS
- **Heartbeat**: Monitoramento contínuo do status do sistema
- **ID Único**: Identificação baseada em MAC address com QR Code

### Modos de Operação
- **Modo Automático**: Controle baseado em setpoints pré-definidos
- **Modo Debug**: Controle manual completo dos atuadores via Firebase
- **Modo Desenvolvimento**: Operações avançadas para teste de hardware
- **Modo Calibração**: Ajuste fino de sensores e parâmetros

## Hardware

### Sensores
- **DHT22**: Temperatura e Umidade
- **CCS811**: CO2 e TVOCs (Compostos Orgânicos Voláteis)
- **LDR**: Sensor de Luminosidade
- **MQ-7**: Sensor de Monóxido de Carbono
- **Sensor de Água**: Nível do reservatório

### Atuadores
- **Módulo Peltier**: Controle bidirecional de temperatura (2 relés)
- **Umidificador**: Controle de umidade relativa
- **LEDs Grow**: Iluminação com controle PWM
- **Exaustor + Dampers**: Ventilação com servo motor
- **Sistema de Relés**: Controle de potência (4 relés)

### Controlador
- **ESP32**: Microcontrolador principal com WiFi e Bluetooth
- **Conversor Analógico-Digital**: Leitura de sensores analógicos
- **Driver PWM**: Controle de intensidade luminosa

## Configuração

### Pré-requisitos
- PlatformIO IDE (recomendado) ou Arduino IDE
- ESP32 DevKit ou similar
- Biblioteca Firebase ESP Client
- Sensores e atuadores conforme lista acima

### Configuração Inicial
1. Clone o repositório
2. Configure as credenciais do Firebase via portal web
3. Carregue o código no ESP32
4. Acesse o portal de configuração via WiFi
5. O sistema estará operacional automaticamente

## Estrutura de Dados no Firebase

### Estrutura Principal
```
/greenhouses/IFUNGI-{MAC_ADDRESS}/
├── atuadores/
│   ├── leds/{ligado, watts}
│   ├── rele1, rele2, rele3, rele4
│   └── umidificador
├── sensores/
│   ├── temperatura, umidade
│   ├── co2, co, tvocs
│   └── luminosidade
├── setpoints/
│   ├── lux, tMin, tMax, uMin, uMax
│   └── coSp, co2Sp, tvocsSp
├── manual_actuators/ (modo debug)
├── devmode/ (modo desenvolvimento)
├── status/{online, lastHeartbeat, ip}
└── niveis/agua
```

### Histórico de Dados
```
/historico/IFUNGI-{MAC_ADDRESS}/{timestamp}
├── temperatura, umidade
├── co2, co, tvocs
├── luminosidade
└── dataHora (ISO 8601)
```

## Fluxo de Operação

1. **Inicialização**: Carrega configurações da NVS, conecta WiFi, autentica Firebase
2. **Leitura de Sensores**: Coleta dados ambientais a cada 2 segundos
3. **Processamento**: Aplica lógica de controle baseada em setpoints
4. **Atuação**: Controla atuadores conforme necessidades detectadas
5. **Sincronização**: Envia dados para Firebase e recebe comandos
6. **Backup**: Armazena dados localmente em caso de offline

## Recursos de Segurança

- **Proteção Térmica Peltier**: Limite de tempo de operação e cooldown obrigatório
- **Sensor de Água**: Desliga umidificador automaticamente em nível baixo
- **Watchdog Interno**: Recuperação de travamentos do sistema
- **Validação de Dados**: Verificação de integridade das leituras dos sensores
- **Bloqueio de Escrita**: Prevenção de conflitos durante operações manuais

## Controles Avançados

### Modo Debug
Permite controle manual completo via Firebase:
- Controle individual de cada relé
- Ajuste de intensidade dos LEDs
- Ativação/desativação manual do umidificador

### Modo Desenvolvimento
Operações avançadas para teste:
- Leitura analógica de pinos específicos
- Controle digital de saídas
- Geração de sinais PWM
- Teste de hardware individual

## Monitoramento e Diagnóstico

- Logs detalhados via Serial (115200 baud)
- LED indicador de status com padrões visuais
- Heartbeat periódico para monitoramento remoto
- Métricas de qualidade de sinal WiFi
- Detecção e recuperação de falhas de sensores

## Performance

- **Leitura de Sensores**: 2 segundos
- **Controle de Atuadores**: 5 segundos  
- **Atualização Firebase**: 5 segundos
- **Heartbeat**: 30 segundos
- **Histórico**: 5 minutos
- **Backup Local**: 1 minuto

## Documentação

### Acesso a documentação
Para acessar a documentação deve-se instalar no windows/linux/mac **Doxygen** pelo link **https://www.doxygen.nl/download.html**, após a instalação deve-se executar o arquivo **generate_docs.bat** na raiz do projeto.

## Como rodar?

### PlatformIO
Para testar o codigo e compilar para o esp32, é utilizado a ide PlatformIO.

Passos para realizar a compilação:
1. **Instalar a extensão no VScode**: PlatformIO IDE
2. **Clonar repositorio do github**: `git clone https://github.com/viniciusmerlak/IFungi-Firmware.git`
3. **Entrar no diretorio correto**: `cd IFungi-Firmware`
4. **Na extensão platformIO**: Clique em Open Project
5. **Selecione o diretorio do repositorio**: IFungi-Firmware
6. **Precione F1 e pesquise por Platformio:build na area de pesquisa do vscode**: `F1` -> `Platformio:build`
7. **Veja o codigo sendo compilado**: Sucesso!


