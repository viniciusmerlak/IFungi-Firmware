#!/usr/bin/env python3
"""
IFungi Greenhouse Firmware Simulator
=====================================
Simula o firmware ESP32 da estufa IFungi, publicando dados realistas no
Firebase Realtime Database para testes homologados do app web.

Replica fielmente o comportamento do firmware v1.2.2:
  - Leituras de sensores (DHT22, CCS811, MQ-7, LDR, nivel de agua)
  - Controle automatico de atuadores baseado em setpoints com histerese
  - Heartbeat periodico
  - Historico de dados (a cada 5 min)
  - Logs remotos (ring buffer)
  - Health status dos sensores
  - Resposta a comandos do app (debug mode, manual actuators, setpoints)

Uso:
    python ifungi_simulator.py --email user@example.com --password secret

Variaveis de ambiente aceitas (alternativa aos args):
    IFUNGI_EMAIL, IFUNGI_PASSWORD, IFUNGI_API_KEY, IFUNGI_DB_URL, IFUNGI_GH_ID
"""

import argparse
import json
import math
import os
import random
import signal
import sys
import threading
import time
from datetime import datetime, timezone

try:
    import requests
except ImportError:
    sys.exit(
        "Erro: modulo 'requests' nao encontrado.\n"
        "Instale com: pip install requests"
    )

# ---------------------------------------------------------------------------
# Constantes (espelham o firmware)
# ---------------------------------------------------------------------------

DEFAULT_API_KEY = "AIzaSyCqq0lkiPq8vcua1_UXPwlslR5E8yGvjOk"
DEFAULT_DB_URL = "https://pfi-ifungi-default-rtdb.firebaseio.com"

SENSOR_READ_INTERVAL = 2.0        # segundos
FIREBASE_UPDATE_INTERVAL = 5.0    # segundos
HEARTBEAT_INTERVAL = 30.0         # segundos
HISTORY_INTERVAL = 300.0          # 5 minutos
LOG_FLUSH_INTERVAL = 1.0          # segundos

RLOG_MAX_LOGS = 200
RLOG_MAX_PERSISTENT = 20

HYSTERESIS_TEMP = 0.5
HYSTERESIS_HUMIDITY = 2.0

# Limites realistas dos sensores
TEMP_RANGE = (15.0, 40.0)
HUMIDITY_RANGE = (30.0, 99.0)
CO2_RANGE = (400, 2000)
CO_RANGE = (0, 100)
TVOCS_RANGE = (0, 500)
LUX_RANGE = (0, 4095)

# ---------------------------------------------------------------------------
# Firebase REST helpers
# ---------------------------------------------------------------------------


class FirebaseClient:
    """Wrapper leve sobre a REST API do Firebase RTDB."""

    def __init__(self, api_key, db_url):
        self.api_key = api_key
        self.db_url = db_url.rstrip("/")
        self.id_token = None
        self.refresh_token = None
        self.uid = None
        self._token_expiry = 0

    def authenticate(self, email, password):
        url = (
            "https://identitytoolkit.googleapis.com/v1/"
            f"accounts:signInWithPassword?key={self.api_key}"
        )
        resp = requests.post(
            url,
            json={
                "email": email,
                "password": password,
                "returnSecureToken": True,
            },
            timeout=15,
        )
        if resp.status_code != 200:
            detail = resp.json().get("error", {}).get("message", resp.text)
            raise RuntimeError(f"Autenticacao falhou: {detail}")

        data = resp.json()
        self.id_token = data["idToken"]
        self.refresh_token = data["refreshToken"]
        self.uid = data["localId"]
        self._token_expiry = time.time() + int(data.get("expiresIn", 3600)) - 60
        return self.uid

    def _ensure_token(self):
        if time.time() < self._token_expiry:
            return
        url = (
            "https://securetoken.googleapis.com/v1/"
            f"token?key={self.api_key}"
        )
        resp = requests.post(
            url,
            json={
                "grant_type": "refresh_token",
                "refresh_token": self.refresh_token,
            },
            timeout=15,
        )
        if resp.status_code != 200:
            raise RuntimeError("Falha ao renovar token")
        data = resp.json()
        self.id_token = data["id_token"]
        self.refresh_token = data["refresh_token"]
        self._token_expiry = time.time() + int(data.get("expires_in", 3600)) - 60

    def get(self, path):
        self._ensure_token()
        url = f"{self.db_url}/{path}.json?auth={self.id_token}"
        resp = requests.get(url, timeout=10)
        if resp.status_code == 200:
            return resp.json()
        return None

    def put(self, path, data):
        self._ensure_token()
        url = f"{self.db_url}/{path}.json?auth={self.id_token}"
        resp = requests.put(url, json=data, timeout=10)
        return resp.status_code == 200

    def patch(self, path, data):
        self._ensure_token()
        url = f"{self.db_url}/{path}.json?auth={self.id_token}"
        resp = requests.patch(url, json=data, timeout=10)
        return resp.status_code == 200


# ---------------------------------------------------------------------------
# Simulador de sensores
# ---------------------------------------------------------------------------


class SensorSimulator:
    """Gera leituras de sensores com variacao realista (random walk + ruido)."""

    def __init__(self, setpoints):
        self.setpoints = setpoints

        # Valores iniciais proximos dos setpoints
        sp = setpoints
        self.temperature = (sp["tMin"] + sp["tMax"]) / 2.0
        self.humidity = (sp["uMin"] + sp["uMax"]) / 2.0
        self.co2 = min(sp["co2Sp"], 600)
        self.co = min(sp["coSp"], 10)
        self.tvocs = min(sp["tvocsSp"], 50)
        self.luminosity = sp["lux"] // 2
        self.water_level = True  # True = nivel OK (firmware: HIGH = tem agua)

        # Saude dos sensores
        self.dht_ok = True
        self.ccs_ok = True
        self.mq7_ok = True  # MQ-7 comeca falso, aquece em ~2 min no simulador
        self.ldr_ok = True
        self.water_ok = True

        self._mq7_warmup_start = time.time()
        self._mq7_warmup_secs = 120  # simulado: 2 min (real: 2 min)

        # Probabilidade de falha de sensor (para testes de error handling)
        self._fail_probability = 0.0

    def set_fail_probability(self, prob):
        self._fail_probability = max(0.0, min(1.0, prob))

    def update_setpoints(self, sp):
        self.setpoints = sp

    def update(self):
        sp = self.setpoints

        # Random walk com tendencia aos setpoints (mean-reversion)
        target_temp = (sp["tMin"] + sp["tMax"]) / 2.0
        self.temperature += (target_temp - self.temperature) * 0.02
        self.temperature += random.gauss(0, 0.15)
        self.temperature = max(TEMP_RANGE[0], min(TEMP_RANGE[1], self.temperature))

        target_hum = (sp["uMin"] + sp["uMax"]) / 2.0
        self.humidity += (target_hum - self.humidity) * 0.02
        self.humidity += random.gauss(0, 0.3)
        self.humidity = max(HUMIDITY_RANGE[0], min(HUMIDITY_RANGE[1], self.humidity))

        self.co2 += random.randint(-15, 15)
        self.co2 = max(CO2_RANGE[0], min(CO2_RANGE[1], self.co2))

        self.co += random.randint(-2, 2)
        self.co = max(CO_RANGE[0], min(CO_RANGE[1], self.co))

        self.tvocs += random.randint(-5, 5)
        self.tvocs = max(TVOCS_RANGE[0], min(TVOCS_RANGE[1], self.tvocs))

        # Luminosidade varia com "hora do dia" (ciclo sinusoidal de 24h)
        hour = datetime.now().hour + datetime.now().minute / 60.0
        day_factor = max(0, math.sin((hour - 6) * math.pi / 12))
        self.luminosity = int(sp["lux"] * day_factor * random.uniform(0.85, 1.15))
        self.luminosity = max(LUX_RANGE[0], min(LUX_RANGE[1], self.luminosity))

        # MQ-7 aquecimento
        elapsed = time.time() - self._mq7_warmup_start
        self.mq7_ok = elapsed >= self._mq7_warmup_secs

        # Simula falhas aleatorias de sensor (se habilitado)
        if self._fail_probability > 0:
            if random.random() < self._fail_probability * 0.3:
                self.dht_ok = not self.dht_ok
            if random.random() < self._fail_probability * 0.1:
                self.ccs_ok = not self.ccs_ok

        # Nivel de agua muda raramente
        if random.random() < 0.005:
            self.water_level = not self.water_level
        self.water_ok = self.water_level

    def get_readings(self):
        return {
            "temperatura": round(self.temperature, 1),
            "umidade": round(self.humidity, 1),
            "co2": self.co2,
            "co": self.co,
            "tvocs": self.tvocs,
            "luminosidade": self.luminosity,
        }


# ---------------------------------------------------------------------------
# Simulador de atuadores
# ---------------------------------------------------------------------------


class ActuatorSimulator:
    """Replica a logica de controle automatico do firmware."""

    def __init__(self):
        self.rele1 = False  # Peltier ligado
        self.rele2 = False  # Peltier polaridade (True=aquece, False=resfria)
        self.rele3 = False  # Umidificador relay
        self.rele4 = False  # Exaustor
        self.leds_on = False
        self.leds_watts = 0
        self.humidifier = False

    def control_automatically(self, sensors, setpoints, dht_healthy):
        sp = setpoints
        temp = sensors["temperatura"]
        hum = sensors["umidade"]
        co2 = sensors["co2"]
        tvocs = sensors["tvocs"]

        # -- Temperatura (Peltier) --
        if not dht_healthy:
            # Seguranca: DHT falhou -> desliga Peltier
            self.rele1 = False
            self.rele2 = False
        elif temp < sp["tMin"] - HYSTERESIS_TEMP:
            self.rele1 = True
            self.rele2 = True   # aquecimento
        elif temp > sp["tMax"] + HYSTERESIS_TEMP:
            self.rele1 = True
            self.rele2 = False  # resfriamento
        elif sp["tMin"] <= temp <= sp["tMax"]:
            self.rele1 = False
            self.rele2 = False

        # -- Umidade --
        if hum < sp["uMin"] - HYSTERESIS_HUMIDITY:
            self.rele3 = True
            self.humidifier = True
        elif hum > sp["uMax"] + HYSTERESIS_HUMIDITY:
            self.rele3 = False
            self.humidifier = False
        elif sp["uMin"] <= hum <= sp["uMax"]:
            self.rele3 = False
            self.humidifier = False

        # -- Ventilacao (CO2/TVOCs) --
        if co2 > sp["co2Sp"] or tvocs > sp["tvocsSp"]:
            self.rele4 = True
        else:
            self.rele4 = False

    def apply_manual(self, manual):
        self.rele1 = bool(manual.get("rele1", False))
        self.rele2 = bool(manual.get("rele2", False))
        self.rele3 = bool(manual.get("rele3", False))
        self.rele4 = bool(manual.get("rele4", False))
        self.humidifier = bool(manual.get("umidificador", False))
        leds = manual.get("leds", {})
        self.leds_on = bool(leds.get("ligado", False))
        self.leds_watts = int(leds.get("intensity", 0))

    def get_state(self):
        return {
            "rele1": self.rele1,
            "rele2": self.rele2,
            "rele3": self.rele3,
            "rele4": self.rele4,
            "leds": {"ligado": self.leds_on, "watts": self.leds_watts},
            "umidificador": self.humidifier,
        }


# ---------------------------------------------------------------------------
# Logger remoto (ring buffer)
# ---------------------------------------------------------------------------


class RemoteLogger:
    """Replica o ring buffer de logs do firmware."""

    def __init__(self, firebase, greenhouse_id):
        self.fb = firebase
        self.gh_id = greenhouse_id
        self._queue = []
        self._head = 0
        self._count = 0
        self._err_head = 0

    def init_from_firebase(self):
        base = f"greenhouses/{self.gh_id}/logs"
        data = self.fb.get(base)
        if data and isinstance(data, dict):
            self._head = int(data.get("head", 0))
            self._count = int(data.get("count", 0))
            self._err_head = int(data.get("err_head", 0))
        else:
            self.fb.patch(base, {
                "head": 0,
                "count": 0,
                "err_head": 0,
                "max": RLOG_MAX_LOGS,
            })

    def log(self, level, tag, msg):
        ts = int(time.time())
        dt = datetime.fromtimestamp(ts, tz=timezone.utc).strftime(
            "%Y-%m-%dT%H:%M:%SZ"
        )
        entry = {"ts": ts, "dt": dt, "lvl": level, "tag": tag, "msg": msg}
        self._queue.append(entry)
        print(f"  [{level}] {tag} {msg}")

    def flush(self):
        if not self._queue:
            return
        entry = self._queue.pop(0)
        base = f"greenhouses/{self.gh_id}/logs"
        slot = f"{base}/recent/{self._head}"

        self.fb.put(slot, entry)

        self._head = (self._head + 1) % RLOG_MAX_LOGS
        self._count += 1
        self.fb.patch(base, {"head": self._head, "count": self._count})

        if entry["lvl"] in ("ERROR", "CRITICAL"):
            err_slot = f"{base}/last_errors/{self._err_head}"
            self.fb.put(err_slot, entry)
            self._err_head = (self._err_head + 1) % RLOG_MAX_PERSISTENT
            self.fb.patch(base, {"err_head": self._err_head})


# ---------------------------------------------------------------------------
# Greenhouse Simulator (orquestrador)
# ---------------------------------------------------------------------------


class GreenhouseSimulator:
    """Orquestra todos os modulos, replicando o loop do MainController."""

    def __init__(self, firebase, greenhouse_id, scenario="normal"):
        self.fb = firebase
        self.gh_id = greenhouse_id
        self.scenario = scenario
        self._running = False

        # Setpoints default (podem ser sobrescritos pelo Firebase)
        self.setpoints = {
            "lux": 5000,
            "tMin": 20.0,
            "tMax": 30.0,
            "uMin": 60.0,
            "uMax": 80.0,
            "coSp": 50,
            "co2Sp": 400,
            "tvocsSp": 100,
        }

        self.led_schedule = {
            "scheduleEnabled": False,
            "solarSimEnabled": False,
            "onHour": 6,
            "onMinute": 0,
            "offHour": 20,
            "offMinute": 0,
            "intensity": 255,
        }

        self.operation_mode = {
            "mode": "manual",
            "lastChanged": int(time.time()),
            "changedBy": "esp32",
        }

        self.debug_mode = False

        self.sensors = SensorSimulator(self.setpoints)
        self.actuators = ActuatorSimulator()
        self.logger = RemoteLogger(firebase, greenhouse_id)

        self._apply_scenario()

    def _apply_scenario(self):
        if self.scenario == "sensor_failure":
            self.sensors.set_fail_probability(0.3)
        elif self.scenario == "hot":
            self.sensors.temperature = 38.0
            self.setpoints["tMax"] = 28.0
        elif self.scenario == "cold":
            self.sensors.temperature = 12.0
            self.setpoints["tMin"] = 22.0
        elif self.scenario == "humid":
            self.sensors.humidity = 98.0
            self.setpoints["uMax"] = 75.0
        elif self.scenario == "dry":
            self.sensors.humidity = 35.0
            self.setpoints["uMin"] = 65.0
        elif self.scenario == "co2_high":
            self.sensors.co2 = 1800
            self.setpoints["co2Sp"] = 800

    def create_initial_structure(self):
        """Cria a estrutura completa da estufa (como createInitialGreenhouse)."""
        ts = int(time.time())
        structure = {
            "atuadores": self.actuators.get_state(),
            "createdBy": self.fb.uid,
            "currentUser": self.fb.uid,
            "lastUpdate": ts,
            "sensores": self.sensors.get_readings(),
            "sensor_status": {
                "dht22_sensorError": "OK",
                "ccs811_sensorError": "OK",
                "mq07_sensorError": "SensorError03",
                "ldr_sensorError": "OK",
                "waterlevel_sensorError": "OK",
                "lastUpdate": ts,
            },
            "setpoints": self.setpoints,
            "niveis": {"agua": False},
            "debug_mode": False,
            "manual_actuators": {
                "rele1": False,
                "rele2": False,
                "rele3": False,
                "rele4": False,
                "leds": {"ligado": False, "intensity": 0},
                "umidificador": False,
            },
            "devmode": {
                "analogRead": False,
                "boolean": False,
                "pin": -1,
                "pwm": False,
                "pwmValue": 0,
            },
            "led_schedule": self.led_schedule,
            "operation_mode": self.operation_mode,
            "status": {
                "online": True,
                "lastHeartbeat": ts,
                "ip": "192.168.1.100",
            },
            "ota": {
                "available": False,
                "version": "",
                "url": "",
                "notes": "Simulador — OTA desabilitado",
            },
            "logs": {
                "head": 0,
                "count": 0,
                "err_head": 0,
                "max": RLOG_MAX_LOGS,
            },
        }

        path = f"greenhouses/{self.gh_id}"
        existing = self.fb.get(path)
        if existing:
            print(f"[sim] Estufa {self.gh_id} ja existe — atualizando status")
            self.fb.patch(path, {
                "status": structure["status"],
                "lastUpdate": ts,
            })
            # Carrega setpoints existentes
            if "setpoints" in existing:
                self.setpoints.update(existing["setpoints"])
                self.sensors.update_setpoints(self.setpoints)
            if "debug_mode" in existing:
                self.debug_mode = bool(existing["debug_mode"])
            if "led_schedule" in existing:
                self.led_schedule.update(existing["led_schedule"])
            if "operation_mode" in existing:
                self.operation_mode.update(existing["operation_mode"])
        else:
            print(f"[sim] Criando estufa {self.gh_id}")
            self.fb.put(path, structure)

        # Garante permissoes do usuario
        self._ensure_user_permission()

        # Inicializa logger
        self.logger.init_from_firebase()
        self.logger.log(
            "INFO",
            "[system]",
            f"Simulador iniciado | ID: {self.gh_id} | cenario: {self.scenario}",
        )

    def _ensure_user_permission(self):
        uid = self.fb.uid
        path = f"Usuarios/{uid}/Estufas permitidas"
        existing = self.fb.get(path)

        if existing is None:
            self.fb.put(path, [self.gh_id])
            print(f"[sim] Permissao criada para usuario {uid}")
        elif isinstance(existing, list):
            if self.gh_id not in existing:
                existing.append(self.gh_id)
                self.fb.put(path, existing)
                print(f"[sim] Estufa adicionada a lista de permissoes")
        elif isinstance(existing, dict):
            vals = list(existing.values())
            if self.gh_id not in vals:
                vals.append(self.gh_id)
                self.fb.put(path, vals)
        elif isinstance(existing, str):
            if existing != self.gh_id:
                self.fb.put(path, [existing, self.gh_id])

    def _send_sensor_data(self):
        readings = self.sensors.get_readings()
        ts = int(time.time())
        base = f"greenhouses/{self.gh_id}"
        self.fb.patch(base, {
            "sensores": readings,
            "lastUpdate": ts,
            "niveis/agua": self.sensors.water_level,
        })

    def _send_actuator_state(self):
        state = self.actuators.get_state()
        ts = int(time.time())
        base = f"greenhouses/{self.gh_id}"
        self.fb.patch(base, {
            "atuadores": state,
            "lastUpdate": ts,
        })

    def _send_sensor_health(self):
        s = self.sensors

        def err(ok, code):
            return "OK" if ok else code

        ts = int(time.time())
        base = f"greenhouses/{self.gh_id}"
        self.fb.patch(base, {
            "sensor_status": {
                "dht22_sensorError": err(s.dht_ok, "SensorError01"),
                "ccs811_sensorError": err(s.ccs_ok, "SensorError02"),
                "mq07_sensorError": err(s.mq7_ok, "SensorError03"),
                "ldr_sensorError": err(s.ldr_ok, "SensorError04"),
                "waterlevel_sensorError": err(s.water_ok, "SensorError05"),
                "lastUpdate": ts,
            },
        })

    def _send_heartbeat(self):
        ts = int(time.time())
        path = f"greenhouses/{self.gh_id}/status"
        self.fb.patch(path, {
            "online": True,
            "lastHeartbeat": ts,
            "ip": "192.168.1.100",
        })

    def _send_history(self):
        readings = self.sensors.get_readings()
        ts = int(time.time())
        ts_str = str(ts)
        dt = datetime.fromtimestamp(ts, tz=timezone.utc).strftime(
            "%Y-%m-%dT%H:%M:%SZ"
        )
        entry = {
            "timestamp": ts_str,
            "dataHora": dt,
            **readings,
        }
        path = f"historico/{self.gh_id}/{ts_str}"
        self.fb.put(path, entry)

    def _poll_commands(self):
        """Le comandos do app (setpoints, debug mode, manual actuators)."""
        base = f"greenhouses/{self.gh_id}"
        data = self.fb.get(base)
        if not data or not isinstance(data, dict):
            return

        # Setpoints
        remote_sp = data.get("setpoints")
        if remote_sp and isinstance(remote_sp, dict):
            changed = False
            for key in self.setpoints:
                if key in remote_sp:
                    new_val = remote_sp[key]
                    if isinstance(new_val, (int, float)) and new_val != self.setpoints[key]:
                        self.setpoints[key] = new_val
                        changed = True
            if changed:
                self.sensors.update_setpoints(self.setpoints)
                self.logger.log(
                    "INFO", "[setpoints]",
                    f"Setpoints atualizados: tMin={self.setpoints['tMin']}, "
                    f"tMax={self.setpoints['tMax']}, "
                    f"uMin={self.setpoints['uMin']}, uMax={self.setpoints['uMax']}"
                )

        # Debug mode
        new_debug = bool(data.get("debug_mode", False))
        if new_debug != self.debug_mode:
            self.debug_mode = new_debug
            mode_str = "MANUAL" if new_debug else "AUTOMATICO"
            self.logger.log("INFO", "[debug]", f"Modo {mode_str} ativado")

        # Manual actuators (quando debug mode ativo)
        if self.debug_mode:
            manual = data.get("manual_actuators", {})
            if manual and isinstance(manual, dict):
                self.actuators.apply_manual(manual)

        # Operation mode
        remote_mode = data.get("operation_mode")
        if remote_mode and isinstance(remote_mode, dict):
            new_mode = remote_mode.get("mode", "manual")
            if new_mode != self.operation_mode.get("mode"):
                old_mode = self.operation_mode["mode"]
                self.operation_mode.update(remote_mode)
                self.logger.log(
                    "INFO", "[mode]",
                    f"Modo alterado: {old_mode} -> {new_mode}",
                )
                self._apply_mode_setpoints(new_mode)

        # LED schedule
        remote_led = data.get("led_schedule")
        if remote_led and isinstance(remote_led, dict):
            self.led_schedule.update(remote_led)

    def _apply_mode_setpoints(self, mode):
        """Aplica setpoints pre-definidos para cada modo de operacao."""
        presets = {
            "incubacao": {
                "tMin": 24.0, "tMax": 28.0,
                "uMin": 80.0, "uMax": 90.0,
                "co2Sp": 2000, "tvocsSp": 200,
            },
            "frutificacao": {
                "tMin": 18.0, "tMax": 24.0,
                "uMin": 85.0, "uMax": 95.0,
                "co2Sp": 800, "tvocsSp": 150,
            },
            "secagem": {
                "tMin": 28.0, "tMax": 35.0,
                "uMin": 30.0, "uMax": 50.0,
                "co2Sp": 5000, "tvocsSp": 500,
            },
            "manutencao": {
                "tMin": 20.0, "tMax": 25.0,
                "uMin": 50.0, "uMax": 70.0,
                "co2Sp": 1000, "tvocsSp": 200,
            },
        }
        if mode in presets:
            self.setpoints.update(presets[mode])
            self.sensors.update_setpoints(self.setpoints)
            # Publica setpoints atualizados no Firebase
            self.fb.patch(
                f"greenhouses/{self.gh_id}",
                {"setpoints": self.setpoints},
            )

    def _apply_led_schedule(self):
        if not self.led_schedule.get("scheduleEnabled"):
            return

        now = datetime.now()
        current_minutes = now.hour * 60 + now.minute
        on_minutes = (
            self.led_schedule["onHour"] * 60 + self.led_schedule["onMinute"]
        )
        off_minutes = (
            self.led_schedule["offHour"] * 60 + self.led_schedule["offMinute"]
        )

        if on_minutes < off_minutes:
            should_be_on = on_minutes <= current_minutes < off_minutes
        else:
            should_be_on = current_minutes >= on_minutes or current_minutes < off_minutes

        if should_be_on:
            intensity = self.led_schedule.get("intensity", 255)
            if self.led_schedule.get("solarSimEnabled"):
                # Simulacao solar: seno entre on e off
                window = off_minutes - on_minutes
                if window <= 0:
                    window = 1440 + window
                progress = (current_minutes - on_minutes) % 1440
                factor = math.sin(progress * math.pi / window)
                intensity = int(intensity * max(0, factor))
            self.actuators.leds_on = True
            self.actuators.leds_watts = intensity
        else:
            self.actuators.leds_on = False
            self.actuators.leds_watts = 0

    def _generate_periodic_logs(self):
        """Gera logs periodicos para manter o painel de logs ativo."""
        readings = self.sensors.get_readings()
        temp = readings["temperatura"]
        hum = readings["umidade"]
        co2 = readings["co2"]

        # Log de status periodico
        self.logger.log(
            "INFO", "[sensor]",
            f"T={temp}C H={hum}% CO2={co2}ppm",
        )

        # Avisos baseados em setpoints
        if temp < self.setpoints["tMin"]:
            self.logger.log(
                "WARN", "[peltier]",
                f"Temperatura abaixo do setpoint: {temp}C < {self.setpoints['tMin']}C",
            )
        elif temp > self.setpoints["tMax"]:
            self.logger.log(
                "WARN", "[peltier]",
                f"Temperatura acima do setpoint: {temp}C > {self.setpoints['tMax']}C",
            )

        if not self.sensors.dht_ok:
            self.logger.log(
                "ERROR", "[sensor]",
                "DHT22 INOPERANTE — temperatura/umidade indisponivel",
            )

        if not self.sensors.water_level:
            self.logger.log(
                "WARN", "[sensor]",
                "Nivel de agua BAIXO — umidificador bloqueado",
            )

    def run(self):
        """Loop principal do simulador."""
        self._running = True

        # Timers (espelham MainController.cpp)
        last_sensor_read = 0
        last_firebase_update = 0
        last_heartbeat = 0
        last_history = 0
        last_log_flush = 0
        last_command_poll = 0
        last_periodic_log = 0

        cycle = 0

        print(f"\n{'='*60}")
        print(f" IFungi Firmware Simulator v1.2.2")
        print(f" Estufa: {self.gh_id}")
        print(f" Cenario: {self.scenario}")
        print(f" Ctrl+C para encerrar")
        print(f"{'='*60}\n")

        while self._running:
            now = time.time()

            # Leitura de sensores (2s)
            if now - last_sensor_read >= SENSOR_READ_INTERVAL:
                self.sensors.update()
                last_sensor_read = now

            # Controle de atuadores
            if not self.debug_mode:
                self.actuators.control_automatically(
                    self.sensors.get_readings(),
                    self.setpoints,
                    self.sensors.dht_ok,
                )
                self._apply_led_schedule()

            # Envio ao Firebase (5s)
            if now - last_firebase_update >= FIREBASE_UPDATE_INTERVAL:
                self._send_sensor_data()
                self._send_sensor_health()
                self._send_actuator_state()
                last_firebase_update = now
                cycle += 1

                if cycle % 6 == 0:  # a cada ~30s
                    readings = self.sensors.get_readings()
                    sys.stdout.write(
                        f"\r[sim] T={readings['temperatura']}C "
                        f"H={readings['umidade']}% "
                        f"CO2={readings['co2']}ppm "
                        f"Modo={'Manual' if self.debug_mode else 'Auto'} "
                        f"Peltier={'ON' if self.actuators.rele1 else 'OFF'} "
                        f"Hum={'ON' if self.actuators.humidifier else 'OFF'} "
                        f"LEDs={'ON' if self.actuators.leds_on else 'OFF'}   "
                    )
                    sys.stdout.flush()

            # Heartbeat (30s)
            if now - last_heartbeat >= HEARTBEAT_INTERVAL:
                self._send_heartbeat()
                last_heartbeat = now

            # Historico (5 min)
            if now - last_history >= HISTORY_INTERVAL:
                self._send_history()
                last_history = now
                self.logger.log(
                    "INFO", "[firebase]",
                    "Dados salvos no historico com sucesso",
                )

            # Poll de comandos do app (5s)
            if now - last_command_poll >= FIREBASE_UPDATE_INTERVAL:
                try:
                    self._poll_commands()
                except Exception as e:
                    print(f"\n[sim] Erro ao ler comandos: {e}")
                last_command_poll = now

            # Logs periodicos (60s)
            if now - last_periodic_log >= 60:
                self._generate_periodic_logs()
                last_periodic_log = now

            # Flush de logs (1s)
            if now - last_log_flush >= LOG_FLUSH_INTERVAL:
                try:
                    self.logger.flush()
                except Exception:
                    pass
                last_log_flush = now

            time.sleep(0.5)

    def stop(self):
        """Para o simulador e marca a estufa como offline."""
        self._running = False
        print("\n\n[sim] Encerrando simulador...")
        self.logger.log("INFO", "[system]", "Simulador encerrado")
        try:
            self.logger.flush()
        except Exception:
            pass

        ts = int(time.time())
        self.fb.patch(f"greenhouses/{self.gh_id}/status", {
            "online": False,
            "lastHeartbeat": ts,
        })
        print("[sim] Estufa marcada como offline")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def parse_args():
    parser = argparse.ArgumentParser(
        description="IFungi Greenhouse Firmware Simulator",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Cenarios disponiveis:
  normal          Operacao normal com valores proximos aos setpoints
  sensor_failure  Falhas aleatorias de sensor (DHT22, CCS811)
  hot             Temperatura alta (acima do setpoint)
  cold            Temperatura baixa (abaixo do setpoint)
  humid           Umidade alta (acima do setpoint)
  dry             Umidade baixa (abaixo do setpoint)
  co2_high        Nivel de CO2 elevado

Exemplos:
  python ifungi_simulator.py --email user@test.com --password 123456
  python ifungi_simulator.py --email user@test.com --password 123456 --scenario hot
  python ifungi_simulator.py --email user@test.com --password 123456 --greenhouse-id IFUNGI-SIM-001
        """,
    )

    parser.add_argument(
        "--email",
        default=os.environ.get("IFUNGI_EMAIL"),
        help="Email Firebase (ou env IFUNGI_EMAIL)",
    )
    parser.add_argument(
        "--password",
        default=os.environ.get("IFUNGI_PASSWORD"),
        help="Senha Firebase (ou env IFUNGI_PASSWORD)",
    )
    parser.add_argument(
        "--api-key",
        default=os.environ.get("IFUNGI_API_KEY", DEFAULT_API_KEY),
        help="Firebase API key (default: projeto pfi-ifungi)",
    )
    parser.add_argument(
        "--db-url",
        default=os.environ.get("IFUNGI_DB_URL", DEFAULT_DB_URL),
        help="Firebase RTDB URL",
    )
    parser.add_argument(
        "--greenhouse-id",
        default=os.environ.get("IFUNGI_GH_ID", "IFUNGI-SIM-001"),
        help="ID da estufa simulada (default: IFUNGI-SIM-001)",
    )
    parser.add_argument(
        "--scenario",
        default="normal",
        choices=[
            "normal",
            "sensor_failure",
            "hot",
            "cold",
            "humid",
            "dry",
            "co2_high",
        ],
        help="Cenario de simulacao (default: normal)",
    )

    return parser.parse_args()


def main():
    args = parse_args()

    if not args.email or not args.password:
        print(
            "Erro: --email e --password sao obrigatorios.\n"
            "Use --help para ver opcoes ou defina IFUNGI_EMAIL / IFUNGI_PASSWORD."
        )
        sys.exit(1)

    # Autentica
    print(f"[firebase] Autenticando com {args.email}...")
    fb = FirebaseClient(args.api_key, args.db_url)
    try:
        uid = fb.authenticate(args.email, args.password)
        print(f"[firebase] Autenticado! UID: {uid}")
    except RuntimeError as e:
        print(f"[firebase] ERRO: {e}")
        sys.exit(1)

    # Cria e executa o simulador
    sim = GreenhouseSimulator(fb, args.greenhouse_id, scenario=args.scenario)

    def signal_handler(sig, frame):
        sim.stop()
        sys.exit(0)

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    sim.create_initial_structure()

    # Envia primeiro historico imediatamente para popular o grafico
    sim._send_history()
    print("[sim] Primeiro registro de historico enviado")

    try:
        sim.run()
    except KeyboardInterrupt:
        sim.stop()


if __name__ == "__main__":
    main()
