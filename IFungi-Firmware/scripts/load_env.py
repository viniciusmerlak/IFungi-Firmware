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
────────────────────────────────────────────────────────────────────────────────
"""

import os
import sys

# Variáveis obrigatórias — o build falha se estiverem ausentes ou vazias
REQUIRED_VARS = [
    "IFUNGI_FIREBASE_API_KEY",
    "IFUNGI_FIREBASE_DB_URL",
    "IFUNGI_FIREBASE_EMAIL",
    "IFUNGI_FIREBASE_PASSWORD",
    "IFUNGI_WIFI_AP_NAME",
    "IFUNGI_WIFI_AP_PASSWORD",
    "IFUNGI_OTA_PASSWORD",
]

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
                print(f"[load_env] ⚠️  Linha {line_num} ignorada (sem '='): {line}")
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
defines = []
for key, value in variables.items():
    if not key.startswith("IFUNGI_"):
        continue  # ignora variáveis de outros projetos no mesmo .env

    if key in INTEGER_VARS:
        defines.append(f"-D{key}={value}")
    elif key in BOOL_VARS:
        defines.append(f"-D{key}={value.lower()}")
    else:
        escaped = escape_for_cpp(value)
        defines.append(f'-D{key}=\\"{escaped}\\"')

env.Append(CPPDEFINES=[], CCFLAGS=defines)

print(f"[load_env] ✅ {len(defines)} variáveis injetadas do .env")
for d in defines:
    # Oculta o valor real no log de build, mostra só o nome da chave
    key_part = d.split("=")[0].replace("-D", "")
    print(f"           → {key_part} ✓")
