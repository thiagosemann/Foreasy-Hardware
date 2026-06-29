"""
get_device_info.py — Descoberta de dispositivos MIDEA na rede local
Uso: python get_device_info.py   (ou duplo clique em rodar.bat)

Roda UMA VEZ por máquina para obter Device ID, IP, token e key necessários
para configurar o firmware midea_iot no ESP32-S3 (wizard em 192.168.4.1).

O ESP se comunica direto com a máquina na rede local — sem internet.
Os dados também ficam salvos em devices.json nesta pasta.

Requisitos:
    pip install midea-local aiohttp
"""

import asyncio
import json
import sys
import io
import getpass
import os

# Fix encoding no Windows
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")

try:
    import aiohttp
    from midealocal.discover import discover
    from midealocal.cloud import get_midea_cloud, SUPPORTED_CLOUDS
except ImportError:
    print("Erro: dependências não encontradas.")
    print("Instale com:  pip install midea-local aiohttp")
    sys.exit(1)

CLOUD_NAME = "SmartHome"   # app usado no Brasil


def print_banner():
    print("=" * 60)
    print("  Foreasy · MIDEA Device Info")
    print("  Descobre IP, token e key para configurar o ESP32-S3")
    print("=" * 60)
    print()


def discover_local():
    """Descobre dispositivos MIDEA na rede via UDP broadcast."""
    print("Procurando dispositivos MIDEA na rede local...")
    devices = discover()
    if not devices:
        print("\nNenhum dispositivo encontrado.")
        print("Verifique se o computador está na mesma rede WiFi da máquina.")
        return {}
    print(f"Encontrou {len(devices)} dispositivo(s).\n")
    return devices


def print_device_summary(devices: dict):
    for i, (device_id, dev) in enumerate(devices.items(), 1):
        prot = dev.get("protocol", "?")
        dtype = dev.get("type", 0)
        ip    = dev.get("ip_address", "N/A")
        port  = dev.get("port", 6444)
        model = dev.get("model", "N/A")
        print(f"[{i}] ID:       {device_id}")
        print(f"    IP:       {ip}:{port}")
        print(f"    Tipo:     0x{dtype:02X} ({dtype})")
        print(f"    Modelo:   {model}")
        print(f"    Protocolo: V{prot}")
        print()


async def get_tokens(devices: dict, account: str, password: str) -> dict:
    """Autentica no SmartHome e obtém token/key para cada dispositivo V3."""
    results = {}

    async with aiohttp.ClientSession() as session:
        cloud = get_midea_cloud(CLOUD_NAME, session=session,
                                account=account, password=password)
        if not cloud:
            print(f"Erro: cloud '{CLOUD_NAME}' não suportada.")
            print(f"Disponíveis: {list(SUPPORTED_CLOUDS.keys())}")
            return results

        print("Autenticando no SmartHome MIDEA...")
        try:
            logged = await cloud.login()
        except Exception as e:
            print(f"Erro de login: {e}")
            return results

        if not logged:
            print("Login falhou. Verifique e-mail e senha.")
            return results

        print("Login OK!\n")

        for device_id, dev in devices.items():
            ip    = dev.get("ip_address", "")
            port  = dev.get("port", 6444)
            dtype = dev.get("type", 0)
            prot  = dev.get("protocol", 0)

            info = {
                "device_id":  str(device_id),
                "ip":         ip,
                "port":       port,
                "type_hex":   f"0x{dtype:02X}",
                "type_dec":   dtype,
                "protocol":   f"V{prot}",
                "model":      dev.get("model", "N/A"),
                "sn":         dev.get("sn", "N/A"),
                "token":      None,
                "key":        None,
            }

            if prot == 3:
                print(f"Buscando token/key para dispositivo {device_id} ({ip})...")
                try:
                    keys = await cloud.get_cloud_keys(int(device_id))
                    if keys:
                        # get_cloud_keys retorna {protocol_version: {"token": ..., "key": ...}}
                        # Pega a chave do protocolo 3 ou a primeira disponível
                        key_data = keys.get(3) or next(iter(keys.values()), None)
                        if key_data:
                            info["token"] = key_data.get("token") or key_data.get("random_key", "")
                            info["key"]   = key_data.get("key", "")
                            print(f"  Token: {str(info['token'])[:16]}...  ({len(str(info['token']))} chars)")
                            print(f"  Key:   {str(info['key'])[:16]}...  ({len(str(info['key']))} chars)")
                            print(f"  Chaves completas salvas em devices.json")
                        else:
                            print(f"  Nenhuma chave encontrada no retorno: {keys}")
                    else:
                        print(f"  get_cloud_keys retornou vazio para device {device_id}")
                except Exception as e:
                    print(f"  Erro ao buscar chaves: {e}")
            else:
                print(f"Dispositivo V{prot} — não precisa de token/key.")

            results[str(device_id)] = info
            print()

    return results


def save_results(results: dict, filename: str = "devices.json"):
    """Salva os resultados num JSON na mesma pasta do script."""
    out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), filename)
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(results, f, indent=2, ensure_ascii=False)
    print(f"Dados salvos em: {out_path}")
    return out_path


def print_esp_config(results: dict):
    """Exibe as configurações prontas para colar no /config do ESP."""
    print()
    print("=" * 60)
    print("  Configurações para o ESP32-S3 (wizard em 192.168.4.1)")
    print("=" * 60)
    for device_id, info in results.items():
        print()
        print(f"  Dispositivo: {info['model']}  ({info['type_hex']})")
        print(f"  Device ID     : {info['device_id']}")
        print(f"  IP da máquina : {info['ip']}")
        print(f"  Protocolo     : {info['protocol']}")
        if info.get("token"):
            print(f"  Token (V3)    : {info['token']}")
            print(f"  Key (V3)      : {info['key']}")
        else:
            print(f"  Token/Key     : não necessário (protocolo {info['protocol']})")
    print()


async def main():
    print_banner()

    # 1. Descoberta local
    devices = discover_local()
    if not devices:
        return

    print_device_summary(devices)

    # 2. Verificar se algum precisa de V3
    needs_auth = any(d.get("protocol") == 3 for d in devices.values())

    results = {}
    if needs_auth:
        print("Um ou mais dispositivos usam Protocolo V3.")
        print("É necessário logar no SmartHome UMA VEZ para obter token/key.")
        print("Após isso, o ESP se comunica direto sem internet.\n")
        account  = input("E-mail do SmartHome MIDEA: ").strip()
        password = getpass.getpass("Senha: ")
        print()
        results = await get_tokens(devices, account, password)
    else:
        # V1/V2 — sem autenticação
        for device_id, dev in devices.items():
            results[str(device_id)] = {
                "device_id": str(device_id),
                "ip":        dev.get("ip_address", ""),
                "port":      dev.get("port", 6444),
                "type_hex":  f"0x{dev.get('type',0):02X}",
                "type_dec":  dev.get("type", 0),
                "protocol":  f"V{dev.get('protocol','?')}",
                "model":     dev.get("model", "N/A"),
                "sn":        dev.get("sn", "N/A"),
                "token":     None,
                "key":       None,
            }

    # 3. Exibe e salva
    print_esp_config(results)
    save_results(results)


if __name__ == "__main__":
    asyncio.run(main())
