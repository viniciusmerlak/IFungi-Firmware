# IFungi Firmware Simulator

Simulador Python do firmware ESP32 da estufa IFungi. Publica dados realistas no Firebase Realtime Database para **testes homologados do app web** sem necessidade de hardware real.

## Funcionalidades

Replica fielmente o comportamento do firmware v1.2.2:

| Funcionalidade | Intervalo | Descrição |
|---|---|---|
| Sensores | 2s | Temperatura, umidade, CO2, CO, TVOCs, luminosidade |
| Firebase update | 5s | Envia sensores, atuadores e health status |
| Heartbeat | 30s | Marca estufa como online |
| Histórico | 5min | Salva snapshot no nó `/historico/` |
| Logs remotos | 1s | Ring buffer com 200 slots + erros persistentes |
| Comandos | 5s | Lê setpoints, debug mode, atuadores manuais |

### Controle Automático
- **Peltier**: Liga aquecimento/resfriamento com histerese de ±0.5°C
- **Umidificador**: Controle com histerese de ±2%
- **Exaustor**: Liga quando CO2 ou TVOCs excedem setpoints
- **LEDs**: Schedule com simulação solar (seno)
- **Segurança**: Bloqueia Peltier se DHT22 falhar

### Cenários de Teste

| Cenário | Descrição |
|---|---|
| `normal` | Valores próximos dos setpoints (operação nominal) |
| `sensor_failure` | Falhas aleatórias de DHT22/CCS811 |
| `hot` | Temperatura alta, dispara resfriamento |
| `cold` | Temperatura baixa, dispara aquecimento |
| `humid` | Umidade alta, desliga umidificador |
| `dry` | Umidade baixa, liga umidificador |
| `co2_high` | CO2 elevado, dispara exaustor |

## Instalação

```bash
cd simulator
pip install -r requirements.txt
```

Dependência única: `requests`.

## Uso

### Via argumentos

```bash
python ifungi_simulator.py \
  --email user@example.com \
  --password sua_senha \
  --greenhouse-id IFUNGI-SIM-001 \
  --scenario normal
```

### Via variáveis de ambiente

```bash
export IFUNGI_EMAIL="user@example.com"
export IFUNGI_PASSWORD="sua_senha"
export IFUNGI_GH_ID="IFUNGI-SIM-001"

python ifungi_simulator.py --scenario hot
```

### Opções

| Argumento | Env var | Default | Descrição |
|---|---|---|---|
| `--email` | `IFUNGI_EMAIL` | — | Email Firebase (obrigatório) |
| `--password` | `IFUNGI_PASSWORD` | — | Senha Firebase (obrigatório) |
| `--api-key` | `IFUNGI_API_KEY` | Projeto pfi-ifungi | Firebase API key |
| `--db-url` | `IFUNGI_DB_URL` | pfi-ifungi RTDB | Firebase RTDB URL |
| `--greenhouse-id` | `IFUNGI_GH_ID` | `IFUNGI-SIM-001` | ID da estufa simulada |
| `--scenario` | — | `normal` | Cenário de simulação |

## Estrutura de Dados no Firebase

O simulador cria/atualiza a mesma estrutura que o firmware real:

```
/greenhouses/IFUNGI-SIM-001/
├── atuadores/         ← estado dos relés, LEDs, umidificador
├── sensores/          ← temperatura, umidade, CO2, CO, TVOCs, luminosidade
├── sensor_status/     ← saúde de cada sensor (OK / SensorErrorXX)
├── setpoints/         ← limites configuráveis pelo app
├── led_schedule/      ← agendamento de LEDs com simulação solar
├── operation_mode/    ← manual, incubação, frutificação, secagem, manutenção
├── manual_actuators/  ← controle manual (quando debug_mode=true)
├── status/            ← online, lastHeartbeat, IP
├── logs/              ← ring buffer de logs + erros persistentes
├── niveis/agua        ← nível do reservatório
├── debug_mode         ← modo manual ativo
└── ota/               ← nó OTA (desabilitado no simulador)

/historico/IFUNGI-SIM-001/{timestamp}
├── temperatura, umidade, co2, co, tvocs, luminosidade
├── timestamp, dataHora
```

## Interação com o App

O simulador responde a comandos enviados pelo app web:

1. **Alterar setpoints** → Sensores convergem para novos valores
2. **Ativar debug mode** → Atuadores seguem `manual_actuators`
3. **Alterar modo de operação** → Aplica presets de setpoints
4. **LED schedule** → LEDs ligam/desligam conforme agendamento

## Encerrando

`Ctrl+C` para encerrar. O simulador marca a estufa como offline antes de sair.
