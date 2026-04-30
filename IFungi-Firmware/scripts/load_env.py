"""
scripts/load_env.py
────────────────────────────────────────────────────────────────────────────────
Script PlatformIO (extra_scripts) que lê o arquivo .env na raiz do projeto
e injeta cada variável como -D (preprocessor define) nas build_flags.

Como funciona:
  • Executado automaticamente pelo PlatformIO antes de cada build.
  • Lê o .env linha por linha, ignorando comentários (#) e linhas vazias.
  • Cada variável IFUNGI_CHAVE=valor vira  -DIFUNGI_CHAVE=\"valor\"  nas flags.
  • Strings são escapadas corretamente para o compilador C++.
  • Inteiros e booleans são injetados sem aspas para uso direto como literais.

No código C++, use:
    #ifdef IFUNGI_FIREBASE_API_KEY
        String apiKey = IFUNGI_FIREBASE_API_KEY;
    #endif

Segurança:
  • O .env NUNCA entra no repositório (está no .gitignore).
  • O .env.example (sem valores reais) SIM entra no repositório.
  • Se o .env não existir, o script aborta o build com mensagem clara.
  • Se uma variável obrigatória estiver vazia, o build também aborta.
  • IFUNGI_FIREBASE_EMAIL e IFUNGI_FIREBASE_PASSWORD são BLOQUEADAS — nunca
    devem ser compiladas no binário. São credenciais de runtime inseridas
    pelo usuário via portal WiFiManager e armazenadas na NVS criptografada.
    Compilar credenciais no .bin permitiria extração via binwalk/strings.
────────────────────────────────────────────────────────────────────────────────
"""

import os
import sys

# Variáveis obrigatórias em build-time — o build falha se estiverem ausentes ou vazias.
REQUIRED_VARS = [
    "IFUNGI_FIREBASE_API_KEY",   # Pública por design (como google-services.json)
    "IFUNGI_FIREBASE_DB_URL",    # Pública por design
    "IFUNGI_WIFI_AP_NAME",       # Nome do AP de configuração
    "IFUNGI_WIFI_AP_PASSWORD",   # Senha do portal de setup
    "IFUNGI_OTA_PASSWORD",       # Usada apenas pelo PlatformIO local, nunca no binário
]

# ── VARIÁVEIS BLOQUEADAS — NUNCA devem ser compiladas no binário ──────────────
#
# IFUNGI_FIREBASE_EMAIL e IFUNGI_FIREBASE_PASSWORD são credenciais de RUNTIME.
# Elas são inseridas pelo usuário via rede cativa (portal WiFiManager) e
# armazenadas na NVS criptografada do ESP32.
#
# Se estivessem aqui, seriam compiladas no binário e qualquer pessoa com
# acesso ao .bin poderia extraí-las com strings/binwalk.
#
# MOTIVO TÉCNICO: o ESP32 não tem secure enclave — qualquer dado em flash
# pode ser lido via JTAG ou dump de memória. A única proteção real é não
# compilar credenciais de usuário no firmware.
BLOCKED_VARS = {
    "IFUNGI_FIREBASE_EMAIL",
    "IFUNGI_FIREBASE_PASSWORD",
}

# Valores que devem ser tratados como inteiros (sem aspas no define)
INTEGER_VARS = set()

# Valores que devem ser tratados como booleanos (sem aspas)
BOOL_VARS = set()


def load_env(path):
    """Lê o arquivo .env e retorna um dicionário {chave: valor}."""
    env = {}
    with open(path, "r", encoding="utf-8") as f:
        for line_num, line in enumerate(f, 1):
            line = line.strip()

            # Ignora linhas vazias e comentários
            if not line or line.startswith("#"):
                continue

            if "=" not in line:
                print(f"[load_env] WARN Linha {line_num} ignorada (sem '='): {line}")
                continue

            key, _, value = line.partition("=")
            key   = key.strip()
            value = value.strip()

            # Remove aspas envolventes se existirem (ex: CHAVE="valor")
            if len(value) >= 2 and value[0] in ('"', "'") and value[-1] == value[0]:
                value = value[1:-1]

            env[key] = value
    return env


def escape_for_cpp(value):
    """Escapa uma string para uso seguro como -D no compilador C++."""
    # Escapa barras invertidas e aspas duplas
    value = value.replace("\\", "\\\\")
    value = value.replace('"', '\\"')
    return value


Import("env")  # noqa: F821  — variável injetada pelo PlatformIO

# ── Localiza o .env ──────────────────────────────────────────────────────────
project_dir = env.subst("$PROJECT_DIR")  # noqa: F821
env_file    = os.path.join(project_dir, ".env")

if not os.path.isfile(env_file):
    print("\n" + "═" * 70)
    print("  ERRO DE BUILD — arquivo .env não encontrado!")
    print(f"  Esperado em: {env_file}")
    print("  Copie o .env.example e preencha com suas credenciais:")
    print("    cp .env.example .env")
    print("═" * 70 + "\n")
    env.Exit(1)

# ── Lê e valida ──────────────────────────────────────────────────────────────
variables = load_env(env_file)

missing = []
for var in REQUIRED_VARS:
    if var not in variables or not variables[var]:
        missing.append(var)

if missing:
    print("\n" + "═" * 70)
    print("  ERRO DE BUILD — variáveis obrigatórias ausentes ou vazias no .env:")
    for m in missing:
        print(f"    • {m}")
    print("═" * 70 + "\n")
    env.Exit(1)

# ── Injeta como build_flags ──────────────────────────────────────────────────
defines  = []
blocked  = []

for key, value in variables.items():
    if not key.startswith("IFUNGI_"):
        continue  # ignora variáveis de outros projetos no mesmo .env

    # ── BLOQUEIA credenciais de runtime ──────────────────────────────────────
    if key in BLOCKED_VARS:
        blocked.append(key)
        continue  # NUNCA injeta no binário

    if key in INTEGER_VARS:
        defines.append(f"-D{key}={value}")
    elif key in BOOL_VARS:
        defines.append(f"-D{key}={value.lower()}")
    else:
        escaped = escape_for_cpp(value)
        defines.append(f'-D{key}=\\"{escaped}\\"')

env.Append(CPPDEFINES=[], CCFLAGS=defines)

print(f"[load_env] OK {len(defines)} variaveis injetadas do .env")
for d in defines:
    key_part = d.split("=")[0].replace("-D", "")
    print(f"           -> {key_part} [ok]")

if blocked:
    print(f"[load_env] BLOCKED {len(blocked)} variavel(eis) (nao compiladas no binario):")
    for b in blocked:
        print(f"           -> {b} [blocked] (runtime-only - inserida via portal WiFiManager)")
