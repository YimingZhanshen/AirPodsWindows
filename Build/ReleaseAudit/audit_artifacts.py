from pathlib import Path
import hashlib
import json
import os
import re
import struct
import sys
import zipfile

root = Path(sys.argv[1])
version = sys.argv[2] if len(sys.argv) > 2 else "1.4.0"
names = {
    "win64": f"AirPodsWindows-{version}-win64.exe",
    "bridge": f"AirPodsWindows-{version}-win32-bridge.exe",
    "portable": f"AirPodsWindows-{version}-win64-portable.zip",
}
files = {}
for key, name in names.items():
    matches = list(root.rglob(name))
    if len(matches) != 1:
        raise SystemExit(f"expected one {name}, found {len(matches)}")
    files[key] = matches[0]

def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()

def pe_machine(data):
    if len(data) < 0x40 or data[:2] != b"MZ":
        return None
    offset = struct.unpack_from("<I", data, 0x3C)[0]
    if offset + 6 > len(data) or data[offset:offset + 4] != b"PE\0\0":
        return None
    return struct.unpack_from("<H", data, offset + 4)[0]

hashes = {key: sha256(path) for key, path in files.items()}
required = {
    "AirPodsWindows.exe",
    "Animations.rcc",
    "Qt6Core.dll",
    "platforms/qwindows.dll",
    "iconengines/qsvgicon.dll",
    "imageformats/qsvg.dll",
    "multimedia/ffmpegmediaplugin.dll",
}
repo_root = Path(os.environ.get("APD_REPO_ROOT", Path(__file__).resolve().parents[2]))
translation_sources = sorted((repo_root / "Source" / "Resource" / "Translation").glob("*.ts"))
if not translation_sources:
    raise SystemExit("no application translation sources found")
required.update(f"translations/{source.stem}.qm" for source in translation_sources)
forbidden = re.compile(
    r"(?i)((^|[\\/])[^\\/]*(test|mock|simulat(?:or|ion)|fixture|harness|validation)[^\\/]*($|[\\/])|"
    r"(^|[\\/])[^\\/]+\.(pdb|ilk|lib|exp)$)"
)
problems = []
if hashes["win64"] != hashes["bridge"]:
    problems.append("win64 and bridge SHA-256 differ")
installer_machine = pe_machine(files["win64"].read_bytes())
if installer_machine != 0x14C:
    problems.append(f"installer PE machine is {installer_machine}, expected x86 0x14c")
with zipfile.ZipFile(files["portable"]) as archive:
    entries = [info.filename for info in archive.infolist() if not info.is_dir()]
    missing = sorted(required - set(entries))
    bad = sorted(entry for entry in entries if forbidden.search(entry))
    if missing:
        problems.append(f"portable missing runtime files: {missing}")
    if bad:
        problems.append(f"portable contains forbidden artifacts: {bad}")
    app = archive.read("AirPodsWindows.exe") if "AirPodsWindows.exe" in entries else b""
    app_machine = pe_machine(app)
    if app_machine != 0x8664:
        problems.append(f"application PE machine is {app_machine}, expected x64 0x8664")
summary = {
    "files": {key: path.name for key, path in files.items()},
    "sha256": hashes,
    "bridge_matches_win64": hashes["win64"] == hashes["bridge"],
    "installer_machine": hex(installer_machine) if installer_machine else None,
    "application_machine": hex(app_machine) if app_machine else None,
    "portable_entries": len(entries),
    "portable_missing_required": missing,
    "portable_forbidden": bad,
    "problems": problems,
}
print(json.dumps(summary, indent=2))
raise SystemExit(bool(problems))
