#!/usr/bin/env python3
"""Chinese USB/IP management UI and small metadata/presence API for Linux."""
from __future__ import annotations

import argparse
import hashlib
import hmac
import ipaddress
import json
import os
import re
import secrets
import subprocess
import tempfile
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlparse

BUS_RE = re.compile(r"^\s*-\s+busid\s+(\S+)\s+\(([0-9a-fA-F]{4}:[0-9a-fA-F]{4})\)\s*$")
SAFE_BUSID_RE = re.compile(r"^[A-Za-z0-9_.:-]+$")
BASE_DIR = Path(__file__).resolve().parent
INDEX_FILE = BASE_DIR / "index.html"
MANAGED_FILE = Path(os.environ.get("USBIP_MANAGED_FILE", "/run/usbip/managed-busids"))
METADATA_FILE = Path(os.environ.get("USBIP_METADATA_FILE", "/etc/usbip/device-metadata.json"))
CLIENTS_FILE = Path(os.environ.get("USBIP_CLIENTS_FILE", "/run/usbip/clients.json"))
KICK_IDLE_SECONDS = int(os.environ.get("USBIP_KICK_IDLE_SECONDS", "60") or 60)
CLIENT_TTL_SECONDS = max(45, KICK_IDLE_SECONDS + 15)
WATCHDOG_INTERVAL_SECONDS = 10
COMMAND_LOCK = threading.Lock()
# Re-entrant: list_devices() (called while holding this lock) may itself
# migrate legacy metadata, which needs the same lock.
STATE_LOCK = threading.RLock()

AUTH_FILE = Path(os.environ.get("USBIP_AUTH_FILE", "/etc/usbip/auth.json"))
DEFAULT_PASSWORD = "123456"
SESSION_TTL_SECONDS = 7 * 24 * 3600
SESSIONS: dict[str, float] = {}
SESSION_LOCK = threading.Lock()


def hash_password(password: str, salt: str) -> str:
    return hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt.encode("utf-8"), 100_000).hex()


def auth_disabled() -> bool:
    return os.environ.get("USBIP_AUTH_DISABLED", "").lower() in ("1", "true", "yes")


def load_auth() -> dict[str, object] | None:
    if auth_disabled():
        return None
    raw = read_json_file(AUTH_FILE, None)
    if isinstance(raw, dict) and isinstance(raw.get("pwHash"), str) and isinstance(raw.get("salt"), str):
        return raw
    salt = secrets.token_hex(16)
    auth = {
        "version": 2,
        "salt": salt,
        "pwHash": hash_password(DEFAULT_PASSWORD, salt),
        "mustChange": True,
    }
    try:
        atomic_write_json(AUTH_FILE, auth)
    except OSError:
        pass
    return auth


def session_secret(auth: dict[str, object] | None) -> bytes:
    """Signing key for session tokens, derived from the stored password.

    Changing the password changes the salt and hash, which invalidates every
    token handed out before it, without the server keeping any session state.
    """
    if not auth:
        return b""
    return hashlib.sha256(f"{auth.get('salt', '')}|{auth.get('pwHash', '')}".encode("utf-8")).digest()


def issue_session_token(auth: dict[str, object] | None) -> str:
    """Self-contained signed token, so restarts do not log the admin out."""
    if auth is None:
        return ""
    expiry = int(time.time()) + SESSION_TTL_SECONDS
    signature = hmac.new(session_secret(auth), str(expiry).encode("ascii"), hashlib.sha256).hexdigest()
    return f"{expiry}.{signature}"


def verify_login(password: str) -> tuple[bool, str, bool]:
    env_token = os.environ.get("USBIP_WEB_TOKEN", "")
    if env_token and hmac.compare_digest(password, env_token):
        return True, env_token, False
    auth = load_auth()
    if auth is None:
        return True, "", False
    salt = str(auth.get("salt", ""))
    expected = str(auth.get("pwHash", ""))
    if not salt or not expected:
        return False, "", False
    if not hmac.compare_digest(hash_password(password, salt), expected):
        return False, "", False
    return True, issue_session_token(auth), bool(auth.get("mustChange", False))


def valid_token(token: str) -> bool:
    env_token = os.environ.get("USBIP_WEB_TOKEN", "")
    if env_token and hmac.compare_digest(token, env_token):
        return True
    if not token:
        return False
    # Signed token: verified from the persisted password, so a container
    # rebuild no longer throws the administrator back to the login box.
    auth = load_auth()
    expiry, _, signature = token.partition(".")
    if auth is not None and signature and expiry.isdigit():
        expected = hmac.new(session_secret(auth), expiry.encode("ascii"), hashlib.sha256).hexdigest()
        if hmac.compare_digest(signature, expected) and int(expiry) > time.time():
            return True
    with SESSION_LOCK:
        now = time.time()
        expired = [key for key, deadline in SESSIONS.items() if deadline <= now]
        for key in expired:
            SESSIONS.pop(key, None)
        return token in SESSIONS


def configured_port() -> int:
    try:
        return int(os.environ.get("USBIP_PORT", "5555"))
    except ValueError:
        return 5555


def run_usbip(*args: str) -> tuple[int, str]:
    try:
        completed = subprocess.run(
            ["usbip", *args],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=10,
            check=False,
        )
        return completed.returncode, completed.stdout
    except (OSError, subprocess.TimeoutExpired) as exc:
        return 1, str(exc)


def is_shared(busid: str) -> bool:
    return (Path("/sys/bus/usb/drivers/usbip-host") / busid).exists()


def read_sysfs_value(busid: str, filename: str) -> str:
    try:
        value = (Path("/sys/bus/usb/devices") / busid / filename).read_text(encoding="utf-8", errors="replace")
        return value.strip()
    except (FileNotFoundError, OSError):
        return ""


def enrich_identity(device: dict[str, object]) -> None:
    busid = str(device.get("busid", ""))
    vidpid = str(device.get("vidpid", ""))
    serial = read_sysfs_value(busid, "serial")
    manufacturer = read_sysfs_value(busid, "manufacturer")
    product = read_sysfs_value(busid, "product")
    bcd_device = read_sysfs_value(busid, "bcdDevice")
    device_class = read_sysfs_value(busid, "bDeviceClass")
    try:
        sysfs_path = str((Path("/sys/bus/usb/devices") / busid).resolve())
    except OSError:
        sysfs_path = ""
    device["serial"] = serial
    device["manufacturer"] = manufacturer
    device["product"] = product
    device["bcdDevice"] = bcd_device
    device["deviceClass"] = device_class
    device["sysfsPath"] = sysfs_path
    # Model fingerprint: identical for two units of the same model, so it is
    # only used as an identity when the model occurs exactly once.
    device["fingerprint"] = "|".join([vidpid, manufacturer, product, bcd_device, device_class])
    if serial:
        device["identity"] = f"serial:{vidpid}|{serial}"
    elif sysfs_path:
        device["identity"] = f"path:{sysfs_path}"
    else:
        device["identity"] = f"busid:{busid}"


def serial_key(device: dict[str, object]) -> str | None:
    serial = str(device.get("serial", ""))
    if not serial:
        return None
    return f"serial:{device.get('vidpid', '')}|{serial}"


def fingerprint_key(device: dict[str, object]) -> str:
    return f"fingerprint:{device.get('fingerprint', '')}"


def group_key(device: dict[str, object], index: int) -> str:
    return f"group:{device.get('fingerprint', '')}#{index}"


def legacy_device_key(device: dict[str, object]) -> str:
    vidpid = str(device.get("vidpid", ""))
    description = str(device.get("description", ""))
    return f"device:{vidpid}|{description}"


# 说明:曾有一个 public_device_key()/`deviceKey` 字段,原意是给 Windows 客户端做
# 本地备注的稳定键。核实后确认**两端都没有消费它**(客户端源码无 deviceKey 引用,
# 管理页也未使用),客户端实际是按 (服务器 URL, Bus ID) 匹配自己的存档记录,
# 因此这个字段已移除,避免后人误以为它在生效。


def read_json_file(path: Path, default: object) -> object:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (FileNotFoundError, OSError, json.JSONDecodeError):
        return default


def atomic_write_json(path: Path, payload: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary_name = ""
    try:
        fd, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=str(path.parent))
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            stream.write(json.dumps(payload, ensure_ascii=False, indent=2) + "\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary_name, path)
        temporary_name = ""
    finally:
        if temporary_name:
            try:
                os.unlink(temporary_name)
            except FileNotFoundError:
                pass


# Provenance fields stored alongside alias/remark so a record can be matched
# back to the hardware it belongs to before it is ever moved or trusted.
PROVENANCE_FIELDS = ("vidpid", "serial", "fingerprint")


def read_metadata() -> dict[str, dict[str, str]]:
    raw = read_json_file(METADATA_FILE, {})
    if not isinstance(raw, dict):
        return {}
    devices = raw.get("devices", raw)
    if not isinstance(devices, dict):
        return {}
    result: dict[str, dict[str, str]] = {}
    for key, value in devices.items():
        if not isinstance(key, str) or not isinstance(value, dict):
            continue
        alias = value.get("alias", "")
        remark = value.get("remark", "")
        if isinstance(alias, str) and isinstance(remark, str):
            record = {"alias": alias[:80], "remark": remark[:500]}
            for field in PROVENANCE_FIELDS:
                extra = value.get(field, "")
                if isinstance(extra, str) and extra:
                    record[field] = extra[:200]
            result[key] = record
    return result


def record_owner(record: dict[str, str]) -> dict[str, str]:
    """Provenance carried by a stored record (empty for pre-upgrade records)."""
    return {field: record.get(field, "") for field in PROVENANCE_FIELDS if record.get(field)}


def record_matches_device(record: dict[str, str], device: dict[str, object]) -> bool:
    """True when a stored record provably belongs to this device.

    A record may only be moved to another key (or trusted) when every piece of
    provenance it carries still matches. Records written by older builds carry
    no provenance at all, and those are never migrated from port-bound keys:
    a USB port tells us nothing about which hardware is plugged into it today.
    """
    owner = record_owner(record)
    if not owner:
        return False
    if owner.get("vidpid") and owner["vidpid"] != str(device.get("vidpid", "")):
        return False
    if owner.get("serial") and owner["serial"] != str(device.get("serial", "")):
        return False
    if owner.get("fingerprint") and owner["fingerprint"] != str(device.get("fingerprint", "")):
        return False
    return True


def device_provenance(device: dict[str, object]) -> dict[str, str]:
    provenance = {"vidpid": str(device.get("vidpid", ""))}
    serial = str(device.get("serial", ""))
    if serial:
        provenance["serial"] = serial
    provenance["fingerprint"] = str(device.get("fingerprint", ""))
    return provenance


def write_metadata(values: dict[str, dict[str, str]]) -> None:
    atomic_write_json(METADATA_FILE, {"version": 1, "devices": values})


def metadata_keys(device: dict[str, object], fingerprint_counts: dict[str, int] | None = None,
                  group_index: int = 0) -> list[str]:
    """Keys to try for this device's name/remark, most trustworthy first.

    Ordered by how well the key identifies the hardware itself:

    1. a real serial number;
    2. the model fingerprint, while that model is present exactly once;
    3. the per-unit ordinal inside a group of identical models, with the
       model-level record kept as a group-wide default;
    4. the model-level key written by older builds.

    Port-bound keys (``identity:path:*`` and ``busid:*``) are deliberately not
    consulted: a USB port cannot prove which hardware is plugged into it, and
    trusting them is exactly what let a replaced device inherit a name.
    """
    counts = fingerprint_counts or {}
    fingerprint = str(device.get("fingerprint", ""))
    keys: list[str] = []
    serial = serial_key(device)
    if serial:
        keys.append(serial)
    if counts.get(fingerprint, 0) == 1:
        keys.append(fingerprint_key(device))
    if counts.get(fingerprint, 0) > 1:
        keys.append(group_key(device, group_index))
        if group_index == 0:
            # A name given while this model was the only one present stays
            # useful as the group default instead of silently disappearing.
            keys.append(fingerprint_key(device))
    keys.append(legacy_device_key(device))
    return keys


def device_metadata(device: dict[str, object], fingerprint_counts: dict[str, int] | None = None,
                    group_index: int = 0,
                    metadata: dict[str, dict[str, str]] | None = None) -> dict[str, str]:
    values = read_metadata() if metadata is None else metadata
    for key in metadata_keys(device, fingerprint_counts, group_index):
        # The model-level legacy key is shared by every unit of a model, so it
        # is only honoured when the record does not contradict this device.
        record = legacy_record(values, device) if key.startswith("device:") else values.get(key)
        if record:
            return {"alias": record.get("alias", ""), "remark": record.get("remark", "")}
    return {"alias": "", "remark": ""}


def metadata_binding(device: dict[str, object], fingerprint_counts: dict[str, int] | None = None,
                     group_index: int = 0,
                     metadata: dict[str, dict[str, str]] | None = None) -> tuple[str, str]:
    """Return (code, label) explaining which identity provides this name.

    The order mirrors metadata_keys() exactly, so the label can never describe
    a different key than the one the name was actually read from.
    """
    values = read_metadata() if metadata is None else metadata
    counts = fingerprint_counts or {}
    fingerprint = str(device.get("fingerprint", ""))
    serial = serial_key(device)
    if serial and values.get(serial):
        return "serial", "按序列号识别"
    if counts.get(fingerprint, 0) == 1 and values.get(fingerprint_key(device)):
        return "model", "按设备型号识别"
    if counts.get(fingerprint, 0) > 1 and values.get(group_key(device, group_index)):
        return "group", f"同型号第 {group_index + 1} 台"
    if counts.get(fingerprint, 0) > 1 and group_index == 0 and values.get(fingerprint_key(device)):
        return "model", "按设备型号识别（同型号未分别命名）"
    if legacy_record(values, device):
        return "legacy", "旧版记录"
    return "none", "未设置"


def metadata_target_key(device: dict[str, object], fingerprint_counts: dict[str, int]) -> str:
    """Best available stable key for this device's name/remark."""
    serial = serial_key(device)
    if serial:
        return serial
    fingerprint = str(device.get("fingerprint", ""))
    if fingerprint_counts.get(fingerprint, 0) == 1:
        return fingerprint_key(device)
    return group_key(device, int(device.get("modelIndex", 0)))


def backup_metadata() -> None:
    """Keep a single rollback copy before the metadata file is rewritten."""
    try:
        raw = METADATA_FILE.read_bytes()
    except OSError:
        return
    try:
        Path(str(METADATA_FILE) + ".bak").write_bytes(raw)
    except OSError as exc:
        print(f"[usbip-share-web] metadata backup failed: {exc}", flush=True)


def legacy_record(values: dict[str, dict[str, str]],
                  device: dict[str, object]) -> dict[str, str] | None:
    """Model-level record from an older build, when it does not contradict us."""
    record = values.get(legacy_device_key(device))
    if not record:
        return None
    if record_owner(record) and not record_matches_device(record, device):
        return None
    return record


def migrate_metadata(devices: list[dict[str, object]],
                     fingerprint_counts: dict[str, int]) -> dict[str, dict[str, str]]:
    """Bring stored names onto stable keys; return the snapshot to read from.

    Two deliberately conservative steps:

    * A record left by an older build under the model-level key
      ``device:{vidpid}|{description}`` is moved onto this device's stable key,
      but only when the provenance the record carries still matches. A record
      carrying no provenance is never moved: we cannot prove who it belongs to,
      and guessing is exactly what let a replaced device inherit an old name.
    * Port-bound keys (``identity:path:*``, ``busid:*``) for ports no device
      occupies right now are dropped; nothing reads them any more, and leaving
      them in place is what made them dangerous.

    Runs once per device listing so the file heals itself, and the caller gets
    back the snapshot it should use, so the JSON file is parsed only once.
    """
    with STATE_LOCK:
        values = read_metadata()
        notes: list[str] = []

        for device in devices:
            legacy = legacy_device_key(device)
            record = values.get(legacy)
            if not record or not record_matches_device(record, device):
                continue
            target = metadata_target_key(device, fingerprint_counts)
            if target != legacy and target not in values:
                values[target] = dict(record)
                notes.append(f"moved '{record.get('alias', '')}' {legacy} -> {target}")
            values.pop(legacy, None)
            notes.append(f"removed migrated legacy key {legacy}")

        live_identities = {f"identity:{device.get('identity', '')}" for device in devices}
        live_busids = {f"busid:{device.get('busid', '')}" for device in devices}
        for key in list(values):
            if key.startswith("identity:") and key not in live_identities:
                notes.append(f"dropped dead port key {key} (was '{values[key].get('alias', '')}')")
                values.pop(key, None)
            elif key.startswith("busid:") and key not in live_busids:
                notes.append(f"dropped dead busid key {key} (was '{values[key].get('alias', '')}')")
                values.pop(key, None)

        if notes:
            try:
                backup_metadata()
                write_metadata(values)
            except OSError as exc:
                print(f"[usbip-share-web] metadata migration failed, keeping old file: {exc}", flush=True)
                return read_metadata()
            for note in notes:
                print(f"[usbip-share-web] metadata: {note}", flush=True)
        return values


def read_managed() -> set[str]:
    try:
        return {
            line.strip()
            for line in MANAGED_FILE.read_text(encoding="utf-8").splitlines()
            if line.strip() and SAFE_BUSID_RE.fullmatch(line.strip())
        }
    except (FileNotFoundError, OSError):
        return set()


def write_managed(values: set[str]) -> None:
    MANAGED_FILE.parent.mkdir(parents=True, exist_ok=True)
    temporary = MANAGED_FILE.with_suffix(".tmp")
    temporary.write_text("".join(f"{value}\n" for value in sorted(values)), encoding="utf-8")
    temporary.replace(MANAGED_FILE)


def remember_managed(busid: str) -> None:
    values = read_managed()
    values.add(busid)
    write_managed(values)


def forget_managed(busid: str) -> None:
    values = read_managed()
    values.discard(busid)
    write_managed(values)


def valid_public_ip(value: object) -> str:
    """Display-only client claim; never use this value for authentication."""
    if not isinstance(value, str) or len(value) > 64 or "%" in value:
        return ""
    try:
        ip = ipaddress.ip_address(value.strip())
    except ValueError:
        return ""
    return str(ip) if ip.is_global and not ip.is_multicast else ""


def read_clients() -> list[dict[str, object]]:
    raw = read_json_file(CLIENTS_FILE, {})
    records = raw.get("clients", {}) if isinstance(raw, dict) else {}
    if not isinstance(records, dict):
        return []
    now = time.time()
    active: list[dict[str, object]] = []
    changed = False
    for client_id, record in records.items():
        if not isinstance(client_id, str) or not isinstance(record, dict):
            changed = True
            continue
        try:
            last_seen = float(record.get("lastSeen", 0))
        except (TypeError, ValueError):
            changed = True
            continue
        if now - last_seen > CLIENT_TTL_SECONDS:
            changed = True
            continue
        busids = record.get("busids", [])
        if not isinstance(busids, list):
            busids = []
        try:
            data_port = int(record.get("dataPort", configured_port()))
        except (TypeError, ValueError):
            data_port = configured_port()
        if not 1 <= data_port <= 65535:
            data_port = configured_port()
        active.append(
            {
                "clientId": client_id,
                "name": str(record.get("name", "未命名客户端"))[:80],
                "address": str(record.get("address", ""))[:80],
                "publicIp": valid_public_ip(record.get("publicIp")),
                "dataPort": data_port,
                "busids": [str(item) for item in busids if isinstance(item, str) and SAFE_BUSID_RE.fullmatch(item)],
                "lastSeen": int(last_seen),
            }
        )
    if changed:
        try:
            atomic_write_json(CLIENTS_FILE, {"version": 1, "clients": {item["clientId"]: item for item in active}})
        except OSError:
            pass
    return active


def update_client(payload: object, address: str) -> tuple[bool, str, dict[str, object] | None]:
    if not isinstance(payload, dict):
        return False, "请求格式不正确", None
    client_id = payload.get("clientId")
    name = payload.get("clientName", "")
    busids = payload.get("busids", [])
    data_port = payload.get("dataPort", configured_port())
    if not isinstance(client_id, str) or not 1 <= len(client_id) <= 128:
        return False, "客户端 ID 不正确", None
    if not isinstance(name, str) or not name.strip():
        return False, "客户端名称不能为空", None
    if not isinstance(busids, list) or len(busids) > 64:
        return False, "客户端设备列表不正确", None
    try:
        data_port = int(data_port)
    except (TypeError, ValueError):
        return False, "客户端 USB/IP 端口不正确", None
    if not 1 <= data_port <= 65535:
        return False, "客户端 USB/IP 端口不正确", None
    valid_busids = []
    for busid in busids:
        if not isinstance(busid, str) or not SAFE_BUSID_RE.fullmatch(busid):
            return False, "客户端设备编号不正确", None
        valid_busids.append(busid)

    record: dict[str, object] = {
        "clientId": client_id[:128],
        "name": name.strip()[:80],
        "address": address[:80],  # Observed peer; do not replace with an untrusted claim.
        "publicIp": valid_public_ip(payload.get("publicIp")),
        "dataPort": data_port,
        "busids": valid_busids,
        "lastSeen": int(time.time()),
    }
    with STATE_LOCK:
        raw = read_json_file(CLIENTS_FILE, {})
        records = raw.get("clients", {}) if isinstance(raw, dict) else {}
        if not isinstance(records, dict):
            records = {}
        records[client_id[:128]] = record
        try:
            atomic_write_json(CLIENTS_FILE, {"version": 1, "clients": records})
        except OSError as exc:
            return False, f"保存客户端状态失败：{exc}", None

    # 客户端主动告别:用户从托盘菜单选「退出」时,会先发一个 shutdown=true 的
    # 心跳过来。服务端不要等 60s watchdog,直接对该 client 声称的每个设备
    # 调 kick_device() —— 等同于管理页点「强制下线」,但 0 延迟。
    # 注意:客户端进程退出后 vhci 驱动会自动清理本端的 attach 会话,服务端
    # 这边 unbind+bind 主要是为了让设备共享态立刻回到可分配池,并清掉
    # clients.json 里残留的连接方信息。
    if isinstance(payload, dict) and payload.get("shutdown") is True:
        handle_client_goodbye(client_id[:128], valid_busids)

    return True, "客户端状态已更新", record


def handle_client_goodbye(client_id: str, busids: list[str]) -> None:
    """Immediately tear down a client that explicitly announced shutdown.

    Removes the client record from clients.json and force-releases every busid
    it claimed. Safe to call even if the client record is already gone (e.g.
    a duplicate goodbye after a flaky network).
    """
    if not client_id:
        return
    log_prefix = "[usbip-share-goodbye]"
    with STATE_LOCK:
        raw = read_json_file(CLIENTS_FILE, {})
        records = raw.get("clients", {}) if isinstance(raw, dict) else {}
        if isinstance(records, dict) and client_id in records:
            del records[client_id]
            try:
                atomic_write_json(CLIENTS_FILE, {"version": 1, "clients": records})
            except OSError as exc:
                print(f"{log_prefix} write clients.json failed: {exc}", flush=True)

    for busid in busids:
        if not isinstance(busid, str) or not SAFE_BUSID_RE.fullmatch(busid):
            continue
        ok, message = kick_device(busid)
        print(
            f"{log_prefix} client={client_id} busid={busid} "
            f"{'ok - ' + message if ok else 'skipped - ' + message}",
            flush=True,
        )


def connection_info(busid: str) -> list[dict[str, object]]:
    result = []
    for client in read_clients():
        if busid in client.get("busids", []):
            result.append(
                {
                    "clientId": client.get("clientId", ""),
                    "name": client.get("name", "未命名客户端"),
                    "address": client.get("address", ""),
                    "publicIp": client.get("publicIp", ""),
                    "dataPort": client.get("dataPort", configured_port()),
                    "lastSeen": client.get("lastSeen", 0),
                }
            )
    return result


def list_devices(include_internal: bool = False) -> tuple[list[dict[str, object]], str | None]:
    rc, output = run_usbip("list", "-l")
    if rc != 0:
        return [], output.strip() or "usbip list failed"

    devices: list[dict[str, object]] = []
    current: dict[str, object] | None = None
    for line in output.splitlines():
        match = BUS_RE.match(line)
        if match:
            if current is not None:
                devices.append(current)
            current = {
                "busid": match.group(1),
                "vidpid": match.group(2).lower(),
                "description": "",
            }
            continue
        if current is not None and line.strip() and not current["description"]:
            # Keep the first descriptive line. Later lines may contain sysfs
            # paths or interface details and are not a stable device name.
            current["description"] = line.strip()

    if current is not None:
        devices.append(current)

    for device in devices:
        enrich_identity(device)

    # Count how many units of each model are present, and give each device a
    # stable per-model index (by busid order) so identical models without a
    # serial number can still be told apart predictably.
    fingerprint_counts: dict[str, int] = {}
    for device in devices:
        fingerprint = str(device.get("fingerprint", ""))
        fingerprint_counts[fingerprint] = fingerprint_counts.get(fingerprint, 0) + 1
    group_indexes: dict[str, int] = {}
    for device in sorted(devices, key=lambda item: str(item.get("busid", ""))):
        fingerprint = str(device.get("fingerprint", ""))
        ordinal = group_indexes.get(fingerprint, 0)
        device["modelIndex"] = ordinal
        group_indexes[fingerprint] = ordinal + 1

    # One snapshot for the whole listing: migrate first (it may rewrite the
    # file), then read every device from that same snapshot. This keeps the
    # JSON parsed once per poll and guarantees the alias and its "binding"
    # label are derived from identical data.
    metadata = migrate_metadata(devices, fingerprint_counts)

    for device in devices:
        group_index = int(device.get("modelIndex", 0))
        record = device_metadata(device, fingerprint_counts, group_index, metadata)
        device["alias"] = record["alias"]
        device["remark"] = record["remark"]
        device["binding"], device["bindingLabel"] = metadata_binding(
            device, fingerprint_counts, group_index, metadata
        )
        # Only warn about indistinguishable units when the model really has no
        # serial number to tell them apart with.
        device["duplicateModel"] = (
            fingerprint_counts.get(str(device.get("fingerprint", "")), 0) > 1
            and not str(device.get("serial", ""))
        )
        device["displayName"] = record["alias"] or str(device["description"]) or "未知 USB 设备"
        device["shared"] = is_shared(str(device["busid"]))
        # This is client heartbeat information, so expose it even during a
        # short bind/unbind transition; the UI labels it as a registered peer.
        device["connections"] = connection_info(str(device["busid"]))
        if not include_internal:
            device.pop("serial", None)
            device.pop("sysfsPath", None)
            device.pop("identity", None)
            device.pop("fingerprint", None)
            device.pop("manufacturer", None)
            device.pop("product", None)
            device.pop("bcdDevice", None)
            device.pop("deviceClass", None)
            device.pop("modelIndex", None)
    return devices, None


def save_device_metadata(busid: str, payload: object) -> tuple[bool, str]:
    if not SAFE_BUSID_RE.fullmatch(busid):
        return False, "设备编号格式不正确"
    if not isinstance(payload, dict):
        return False, "名称和备注格式不正确"
    alias = payload.get("alias", "")
    remark = payload.get("remark", "")
    if not isinstance(alias, str) or not isinstance(remark, str):
        return False, "名称和备注必须是文本"
    alias = alias.strip()
    remark = remark.strip()
    if len(alias) > 80:
        return False, "显示名称最多 80 个字符"
    if len(remark) > 500:
        return False, "备注最多 500 个字符"

    # Keep device discovery and the metadata read/write in one lock so two
    # simultaneous editor requests cannot overwrite each other's snapshot.
    with STATE_LOCK:
        devices, error = list_devices(include_internal=True)
        if error is not None:
            return False, error
        device = next((item for item in devices if str(item.get("busid")) == busid), None)
        if device is None:
            return False, "设备不存在，可能已经拔出"

        fingerprint_counts: dict[str, int] = {}
        for item in devices:
            fingerprint = str(item.get("fingerprint", ""))
            fingerprint_counts[fingerprint] = fingerprint_counts.get(fingerprint, 0) + 1
        counts = fingerprint_counts

        # Choose the most reliable identity available: real serial number,
        # then a unique model fingerprint, then a stable ordinal within a
        # group of identical models that carry no serial number.
        target_key = metadata_target_key(device, counts)

        # Keys that used to hold this device's name and must not survive the
        # save. The model-level record is deliberately kept when the model is
        # present more than once: it is that group's fallback name.
        model_count = fingerprint_counts.get(str(device.get("fingerprint", "")), 0)
        stale_keys = {
            f"identity:{device.get('identity', '')}",
            legacy_device_key(device),
            f"busid:{busid}",
            serial_key(device) or "",
            group_key(device, int(device.get("modelIndex", 0))) if model_count > 1 else fingerprint_key(device),
        }
        stale_keys.discard("")
        stale_keys.discard(target_key)

        values = read_metadata()
        if alias or remark:
            # Store the provenance along with the text: it is what lets a later
            # migration prove this record belongs to this hardware.
            record = {"alias": alias, "remark": remark}
            record.update(device_provenance(device))
            values[target_key] = record
            for stale in stale_keys:
                values.pop(stale, None)
        else:
            values.pop(target_key, None)
            for stale in stale_keys:
                values.pop(stale, None)
        try:
            write_metadata(values)
        except OSError as exc:
            return False, f"保存设备名称/备注失败：{exc}"
    return True, "设备名称和备注已保存"


def mutate_device(busid: str, action: str) -> tuple[bool, str]:
    if not SAFE_BUSID_RE.fullmatch(busid):
        return False, "设备编号格式不正确"

    devices, error = list_devices()
    if error is not None:
        return False, error
    if busid not in {str(item["busid"]) for item in devices}:
        return False, "设备不存在，可能已经拔出"

    with COMMAND_LOCK:
        if action == "share":
            if is_shared(busid):
                remember_managed(busid)
                return True, "设备已经处于共享状态"
            rc, output = run_usbip("bind", "-b", busid)
            if rc != 0:
                return False, output.strip() or "共享设备失败"
            remember_managed(busid)
            return True, "设备已开始共享"

        if action == "unshare":
            if not is_shared(busid):
                forget_managed(busid)
                return True, "设备已经处于未共享状态"
            rc, output = run_usbip("unbind", "-b", busid)
            if rc != 0:
                return False, output.strip() or "停止共享失败"
            forget_managed(busid)
            return True, "设备已停止共享"

    return False, "未知操作"


def read_client_records() -> dict[str, dict[str, object]]:
    """Raw client registry, including stale entries (used by the watchdog)."""
    raw = read_json_file(CLIENTS_FILE, {})
    records = raw.get("clients", {}) if isinstance(raw, dict) else {}
    if not isinstance(records, dict):
        return {}
    cleaned: dict[str, dict[str, object]] = {}
    for client_id, record in records.items():
        if isinstance(client_id, str) and isinstance(record, dict):
            cleaned[client_id] = record
    return cleaned


def remove_busid_from_clients(busid: str) -> int:
    """Drop a busid from every client record. Returns number of clients changed."""
    changed_count = 0
    with STATE_LOCK:
        raw = read_json_file(CLIENTS_FILE, {})
        records = raw.get("clients", {}) if isinstance(raw, dict) else {}
        changed = False
        if isinstance(records, dict):
            for client_id, record in list(records.items()):
                if not isinstance(client_id, str) or not isinstance(record, dict):
                    continue
                busids = record.get("busids", [])
                if not isinstance(busids, list):
                    busids = []
                filtered = [item for item in busids if str(item) != busid]
                if len(filtered) != len(busids):
                    record["busids"] = filtered
                    changed = True
                    changed_count += 1
        if changed:
            try:
                atomic_write_json(CLIENTS_FILE, {"version": 1, "clients": records})
            except OSError:
                pass
    return changed_count


def kick_device(busid: str) -> tuple[bool, str]:
    """Force-disconnect the currently attached client from the server side.

    usbipd has no per-connection kill switch; unbinding the exported device
    drops the attached client immediately, then we re-bind so the device
    remains shared for the next client. Heartbeat records for this busid are
    cleared at the same time so the UI reflects the disconnect right away.
    """
    if not SAFE_BUSID_RE.fullmatch(busid):
        return False, "设备编号格式不正确"

    devices, error = list_devices()
    if error is not None:
        return False, error
    if busid not in {str(item["busid"]) for item in devices}:
        return False, "设备不存在，可能已经拔出"
    if not is_shared(busid):
        return False, "设备当前未共享，无法断开远程连接"

    with COMMAND_LOCK:
        rc, output = run_usbip("unbind", "-b", busid)
        if rc != 0:
            return False, output.strip() or "断开远程连接失败"
        rc, output = run_usbip("bind", "-b", busid)
        if rc != 0:
            # Device is now unshared; keep managed state accurate.
            forget_managed(busid)
            return False, (output.strip() or "断开后重新共享失败，设备已停止共享")

    remember_managed(busid)
    remove_busid_from_clients(busid)
    return True, "已从服务器断开远程客户端连接，设备仍保持共享"


def json_bytes(payload: object) -> bytes:
    return json.dumps(payload, ensure_ascii=False).encode("utf-8")


class Handler(BaseHTTPRequestHandler):
    server_version = "usbip-share-ui/0.4"

    def log_message(self, fmt: str, *args: object) -> None:
        print(f"[usbip-share-web] {fmt % args}", flush=True)

    def send_json(self, payload: object, status: int = HTTPStatus.OK) -> None:
        body = json_bytes(payload)
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def authorized(self) -> bool:
        if auth_disabled():
            return True
        supplied = self.headers.get("X-Admin-Token", "")
        return bool(supplied) and valid_token(supplied)

    def read_body_json(self) -> object:
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            raise ValueError("请求长度不正确")
        if length < 0 or length > 65536:
            raise ValueError("请求内容过大")
        raw = self.rfile.read(length)
        try:
            return json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ValueError("请求必须是 UTF-8 JSON") from exc

    def do_GET(self) -> None:  # noqa: N802 (HTTP handler API)
        path = urlparse(self.path).path
        if path == "/" or path == "/index.html":
            try:
                body = INDEX_FILE.read_bytes()
            except OSError:
                self.send_json({"ok": False, "error": "管理页面文件不存在"}, HTTPStatus.INTERNAL_SERVER_ERROR)
                return
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        if path == "/api/health":
            self.send_json({"ok": True, "service": "usbip-share-web"})
            return

        if path == "/api/devices":
            # Client read path is intentionally password-free: the Windows
            # client must be able to list devices without the admin password.
            # Admin write operations behind authorized() must use login/token.
            devices, error = list_devices()
            if error:
                self.send_json({"ok": False, "error": error}, HTTPStatus.INTERNAL_SERVER_ERROR)
                return
            self.send_json({"ok": True, "devices": devices, "usbipPort": configured_port()})
            return

        if path == "/api/session":
            # Lets the page decide between the device list and the login box on
            # load. Always 200 so the browser can simply read the flags.
            auth = load_auth()
            self.send_json(
                {
                    "ok": True,
                    "authorized": self.authorized(),
                    "authDisabled": auth_disabled(),
                    "mustChange": bool(auth.get("mustChange", False)) if auth else False,
                }
            )
            return

        self.send_json({"ok": False, "error": "找不到页面"}, HTTPStatus.NOT_FOUND)

    def do_POST(self) -> None:  # noqa: N802 (HTTP handler API)
        path = urlparse(self.path).path

        if path == "/api/login":
            try:
                payload = self.read_body_json()
            except ValueError as exc:
                self.send_json({"ok": False, "error": str(exc)}, HTTPStatus.BAD_REQUEST)
                return
            password = payload.get("password", "") if isinstance(payload, dict) else ""
            if not isinstance(password, str) or not password:
                self.send_json({"ok": False, "error": "请输入密码"}, HTTPStatus.BAD_REQUEST)
                return
            success, token, must_change = verify_login(password)
            if not success:
                self.send_json({"ok": False, "error": "密码不正确"}, HTTPStatus.UNAUTHORIZED)
                return
            self.send_json({"ok": True, "token": token, "mustChange": must_change})
            return

        # Client heartbeat/registration must remain password-free so the
        # Windows client can report connection owners without the admin password.
        client_match = re.fullmatch(r"/api/clients/(register|heartbeat)", path)
        if client_match:
            try:
                payload = self.read_body_json()
            except ValueError as exc:
                self.send_json({"ok": False, "error": str(exc)}, HTTPStatus.BAD_REQUEST)
                return
            success, message, record = update_client(payload, self.client_address[0])
            self.send_json(
                {"ok": success, "message": message, "client": record} if success else {"ok": False, "error": message},
                HTTPStatus.OK if success else HTTPStatus.BAD_REQUEST,
            )
            return

        # Everything below mutates server state and requires an admin session
        # (web login) or the configured USBIP_WEB_TOKEN.
        if not self.authorized():
            self.send_json({"ok": False, "error": "未登录或登录已过期"}, HTTPStatus.UNAUTHORIZED)
            return

        if path == "/api/change-password":
            if os.environ.get("USBIP_WEB_TOKEN", ""):
                self.send_json({"ok": False, "error": "当前使用管理令牌模式，无法修改密码"}, HTTPStatus.BAD_REQUEST)
                return
            try:
                payload = self.read_body_json()
            except ValueError as exc:
                self.send_json({"ok": False, "error": str(exc)}, HTTPStatus.BAD_REQUEST)
                return
            old_password = payload.get("oldPassword", "") if isinstance(payload, dict) else ""
            new_password = payload.get("newPassword", "") if isinstance(payload, dict) else ""
            if not isinstance(old_password, str) or not isinstance(new_password, str):
                self.send_json({"ok": False, "error": "参数格式不正确"}, HTTPStatus.BAD_REQUEST)
                return
            if len(new_password) < 6:
                self.send_json({"ok": False, "error": "新密码至少 6 个字符"}, HTTPStatus.BAD_REQUEST)
                return
            auth = load_auth()
            if auth is None:
                self.send_json({"ok": True, "message": "认证未启用"}, HTTPStatus.OK)
                return
            salt = str(auth.get("salt", ""))
            expected = str(auth.get("pwHash", ""))
            if not salt or not expected or not hmac.compare_digest(hash_password(old_password, salt), expected):
                self.send_json({"ok": False, "error": "当前密码不正确"}, HTTPStatus.UNAUTHORIZED)
                return
            if old_password == new_password:
                self.send_json({"ok": False, "error": "新密码不能与当前密码相同"}, HTTPStatus.BAD_REQUEST)
                return
            with STATE_LOCK:
                auth["salt"] = secrets.token_hex(16)
                auth["pwHash"] = hash_password(new_password, str(auth["salt"]))
                auth["mustChange"] = False
                try:
                    atomic_write_json(AUTH_FILE, auth)
                except OSError as exc:
                    self.send_json({"ok": False, "error": f"保存密码失败：{exc}"}, HTTPStatus.INTERNAL_SERVER_ERROR)
                    return
            self.send_json({"ok": True, "message": "密码已修改"})
            return

        if not self.authorized():
            self.send_json({"ok": False, "error": "未登录或登录已过期"}, HTTPStatus.UNAUTHORIZED)
            return

        metadata_match = re.fullmatch(r"/api/devices/([^/]+)/metadata", path)
        if metadata_match:
            try:
                payload = self.read_body_json()
            except ValueError as exc:
                self.send_json({"ok": False, "error": str(exc)}, HTTPStatus.BAD_REQUEST)
                return
            busid = unquote(metadata_match.group(1))
            success, message = save_device_metadata(busid, payload)
            devices, error = list_devices()
            response: dict[str, object] = {"ok": success, "message": message}
            if error is None:
                response["devices"] = devices
            if not success:
                response["error"] = message
            self.send_json(response, HTTPStatus.OK if success else HTTPStatus.BAD_REQUEST)
            return

        device_match = re.fullmatch(r"/api/devices/([^/]+)/(share|unshare|kick)", path)
        if not device_match:
            self.send_json({"ok": False, "error": "不支持的操作"}, HTTPStatus.NOT_FOUND)
            return

        busid = unquote(device_match.group(1))
        action = device_match.group(2)
        if action == "kick":
            success, message = kick_device(busid)
        else:
            success, message = mutate_device(busid, action)
        devices, error = list_devices()
        payload = {"ok": success, "message": message}
        if error is None:
            payload["devices"] = devices
        if not success:
            payload["error"] = message
        self.send_json(payload, HTTPStatus.OK if success else HTTPStatus.BAD_REQUEST)


def watchdog_loop() -> None:
    """Periodically release devices whose claiming client stopped heartbeating.

    A client that powers off without detaching can leave the USB/IP import
    held server-side for a long time (TCP keepalive only notices eventually).
    Since every managed client heartbeats every 10s, a stale claim means the
    client is likely gone; we unbind+rebind to force the import down while
    keeping the device shared.
    """
    log_prefix = "[usbip-share-watchdog]"
    while True:
        time.sleep(WATCHDOG_INTERVAL_SECONDS)
        try:
            records = read_client_records()
            if not records:
                continue
            now = time.time()
            # busid -> latest lastSeen among clients claiming it
            claims: dict[str, float] = {}
            for record in records.values():
                busids = record.get("busids", [])
                if not isinstance(busids, list):
                    continue
                try:
                    last_seen = float(record.get("lastSeen", 0))
                except (TypeError, ValueError):
                    last_seen = 0
                for busid in busids:
                    if not isinstance(busid, str):
                        continue
                    if busid not in claims or last_seen > claims[busid]:
                        claims[busid] = last_seen

            if not claims:
                continue

            rc, output = run_usbip("list", "-l")
            if rc != 0:
                continue
            shared_busids = set()
            for line in output.splitlines():
                match = BUS_RE.match(line)
                if match:
                    shared_busids.add(match.group(1))

            for busid, last_seen in claims.items():
                if busid not in shared_busids:
                    continue
                if now - last_seen <= KICK_IDLE_SECONDS:
                    continue
                print(f"{log_prefix} device {busid} has no heartbeat for >{KICK_IDLE_SECONDS}s, force-releasing", flush=True)
                ok, message = kick_device(busid)
                print(f"{log_prefix} device {busid}: {'ok - ' + message if ok else 'failed - ' + message}", flush=True)
        except Exception as exc:  # noqa: BLE001 - keep the loop alive
            print(f"{log_prefix} error: {exc}", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser(description="Chinese USB/IP management UI")
    parser.add_argument("--host", default=os.environ.get("USBIP_WEB_HOST", "0.0.0.0"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("USBIP_WEB_PORT", "8080")))
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        raise SystemExit("web port must be between 1 and 65535")
    if not INDEX_FILE.exists():
        raise SystemExit(f"missing {INDEX_FILE}")

    httpd = ThreadingHTTPServer((args.host, args.port), Handler)
    watchdog = threading.Thread(target=watchdog_loop, name="usbip-kick-watchdog", daemon=True)
    watchdog.start()
    print(f"[usbip-share-web] Chinese management UI listening on {args.host}:{args.port}", flush=True)
    try:
        httpd.serve_forever(poll_interval=0.5)
    finally:
        httpd.server_close()


if __name__ == "__main__":
    main()
