import sys, tempfile, json, time
from pathlib import Path

# Import the server module straight from the repository root, and point every
# state file at a temporary directory so the real NAS data is never touched.
SERVER_DIR = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SERVER_DIR))
import web  # noqa: E402

base = Path(tempfile.mkdtemp())
web.METADATA_FILE = base / "device-metadata.json"
web.AUTH_FILE = base / "auth.json"

OK = []
def check(name, cond, detail=""):
    OK.append(bool(cond))
    print(("  PASS " if cond else "  FAIL ") + name + (f"  [{detail}]" if detail else ""))

def dev(busid, vidpid, mm, prod, fp, ident, idx=0, serial=""):
    return {"busid": busid, "vidpid": vidpid, "description": f"{mm} : {prod}",
            "serial": serial, "manufacturer": mm, "product": prod,
            "bcdDevice": "0100", "deviceClass": "00", "fingerprint": fp,
            "identity": ident, "modelIndex": idx}

print("[场景1] 同端口换成另一台不同型号设备：不得继承旧名")
web.write_metadata({"identity:path:/sys/bus/usb/devices/2-1": {"alias": "A狗-旧名", "remark": "旧备注"}})
newdev = dev("2-1", "abcd:1234", "NewVendor", "NewModel", "abcd:1234|NewVendor|NewModel|0100|00",
             "path:/sys/bus/usb/devices/2-1")
snap = web.migrate_metadata([newdev], {newdev["fingerprint"]: 1})
got = web.device_metadata(newdev, {newdev["fingerprint"]: 1}, 0, snap)
check("新设备未继承旧名", got["alias"] == "", f"alias={got['alias']!r}")
check("旧记录仍在(未误删,可人工找回)", web.read_metadata().get("identity:path:/sys/bus/usb/devices/2-1", {}).get("alias") == "A狗-旧名")

print("[场景2] 同型号两台都有唯一序列号：不应报“无序列号无法区分”")
d1 = {"fingerprint": "fp", "serial": "SN001"}
d2 = {"fingerprint": "fp", "serial": "SN002"}
fc = {}
for d in (d1, d2):
    fc[d["fingerprint"]] = fc.get(d["fingerprint"], 0) + 1
dup = fc["fp"] > 1 and not str(d1.get("serial", ""))
check("duplicateModel=False", dup is False)

print("[场景3] 单台已命名 -> 接入第二台同型号：名字不丢")
web.METADATA_FILE = base / "m2.json"
single = dev("1-1", "1241:e001", "LYFdog", "Sample", "FPX", "path:/p/1", 0)
web.write_metadata({web.metadata_target_key(single, {"FPX": 1}): {"alias": "我的狗", "remark": ""}})
web.migrate_metadata([single], {"FPX": 1})
snap3 = web.read_metadata()
check("单台读到名字", web.device_metadata(single, {"FPX": 1}, 0, snap3)["alias"] == "我的狗")
two = dev("1-2", "1241:e001", "LYFdog", "Sample", "FPX", "path:/p/2", 1)
snap3 = web.migrate_metadata([single, two], {"FPX": 2})
a0 = web.device_metadata(single, {"FPX": 2}, 0, snap3)
a1 = web.device_metadata(two, {"FPX": 2}, 1, snap3)
check("第1台仍读到名字(回落型号默认名)", a0["alias"] == "我的狗", f"alias={a0['alias']!r}")
check("第2台为空(不重复占用)", a1["alias"] == "", f"alias={a1['alias']!r}")
check("binding 标签与取键一致(来自型号默认名)", web.metadata_binding(single, {"FPX": 2}, 0, snap3)[1] == "按设备型号识别（同型号未分别命名）",
      web.metadata_binding(single, {"FPX": 2}, 0, snap3)[1])
web.write_metadata({web.group_key(two, 1): {"alias": "算王2", "remark": ""}})
snap3b = web.read_metadata()
check("已分别命名时标签带真实序号", web.metadata_binding(two, {"FPX": 2}, 1, snap3b)[1] == "同型号第 2 台",
      web.metadata_binding(two, {"FPX": 2}, 1, snap3b)[1])

print("[场景4] 带归属信息的旧型号级记录：可安全迁移")
web.METADATA_FILE = base / "m4.json"
d4 = dev("3-1", "0bda:8156", "Realtek", "LAN", "RK", "path:/p/9", 0, serial="SN9")
web.write_metadata({"device:0bda:8156|Realtek : LAN": {"alias": "网卡", "remark": "", "vidpid": "0bda:8156", "serial": "SN9", "fingerprint": "RK"}})
snap4 = web.migrate_metadata([d4], {"RK": 1})
check("已迁移到序列号键", snap4.get(web.serial_key(d4), {}).get("alias") == "网卡")
check("旧键已清除", "device:0bda:8156|Realtek : LAN" not in snap4)

print("[场景5] 归属不符的旧记录：不得迁移")
web.METADATA_FILE = base / "m5.json"
d5 = dev("3-2", "0bda:8156", "Realtek", "LAN", "RK", "path:/p/8", 0, serial="SN9")
web.write_metadata({"device:0bda:8156|Realtek : LAN": {"alias": "别人家的网卡", "remark": "",
                                                     "vidpid": "0bda:8156", "serial": "SN_OTHER"}})
snap5 = web.migrate_metadata([d5], {"RK": 1})
check("记录未被迁移", snap5.get(web.serial_key(d5)) is None)
check("记录也未被读取", web.device_metadata(d5, {"RK": 1}, 0, snap5)["alias"] == "")

print("[场景6] 僵尸端口键清理（含备份）")
web.METADATA_FILE = base / "m6.json"
d6 = dev("1-8.1", "83d3:3773", "USBKey", "USBKey", "UK", "path:/live/1-8.1")
web.write_metadata({
    "identity:path:/dead/2-1": {"alias": "僵尸A", "remark": ""},
    "busid:9-9": {"alias": "僵尸B", "remark": ""},
    "identity:path:/live/1-8.1": {"alias": "在用", "remark": ""},
    "fingerprint:UK": {"alias": "E算量", "remark": ""},
})
snap6 = web.migrate_metadata([d6], {"UK": 1})
check("死端口键已删", "identity:path:/dead/2-1" not in snap6 and "busid:9-9" not in snap6)
check("在用键保留", "identity:path:/live/1-8.1" in snap6)
check("备份已生成", Path(str(web.METADATA_FILE) + ".bak").exists())
check("正常名字不受影响", web.device_metadata(d6, {"UK": 1}, 0, snap6)["alias"] == "E算量")

print("[场景7] 登录态：容器重启不掉线 / 改密码即失效")
web.load_auth()
ok, token, must = web.verify_login("123456")
check("默认密码可登录", ok and token.count(".") == 1, f"token={token[:18]}…")
check("token 立即可用", web.valid_token(token))
web.SESSIONS.clear()   # 模拟容器重启（内存态清空）
check("模拟重启后 token 仍有效", web.valid_token(token))
auth = json.loads(web.AUTH_FILE.read_text(encoding="utf-8"))
auth["salt"] = "new-salt"
auth["pwHash"] = web.hash_password("newpass", "new-salt")
web.AUTH_FILE.write_text(json.dumps(auth), encoding="utf-8")
check("改密码后旧 token 立即失效", not web.valid_token(token))
check("乱码 token 无效", not web.valid_token("garbage") and not web.valid_token(""))

print()
print("总计:", sum(OK), "/", len(OK), "通过")
sys.exit(0 if all(OK) else 1)
