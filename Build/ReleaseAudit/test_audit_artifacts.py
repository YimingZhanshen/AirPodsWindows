import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
AUDITOR = ROOT / "Build" / "ReleaseAudit" / "audit_artifacts.py"
REQUIRED_RUNTIME = {
    "AirPodsWindows.exe",
    "Animations.rcc",
    "Qt6Core.dll",
    "platforms/qwindows.dll",
    "iconengines/qsvgicon.dll",
    "imageformats/qsvg.dll",
    "multimedia/ffmpegmediaplugin.dll",
}


def pe_bytes(machine):
    data = bytearray(0x86)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", data, 0x84, machine)
    return bytes(data)


class ArtifactTranslationAuditTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.artifacts = self.root / "artifacts"
        self.artifacts.mkdir()
        self.repo = self.root / "repo"
        self.translation_sources = self.repo / "Source" / "Resource" / "Translation"
        self.translation_sources.mkdir(parents=True)
        for locale in ("apd_de_DE", "apd_fr_FR"):
            (self.translation_sources / f"{locale}.ts").write_text("<TS/>", encoding="utf-8")

        installer = pe_bytes(0x14C)
        (self.artifacts / "AirPodsWindows-1.4.0-win64.exe").write_bytes(installer)
        (self.artifacts / "AirPodsWindows-1.4.0-win32-bridge.exe").write_bytes(installer)
        self.portable = self.artifacts / "AirPodsWindows-1.4.0-win64-portable.zip"
        self.entries = {name: b"runtime" for name in REQUIRED_RUNTIME}
        self.entries["AirPodsWindows.exe"] = pe_bytes(0x8664)
        for locale in ("apd_de_DE", "apd_fr_FR"):
            self.entries[f"translations/{locale}.qm"] = b"qm"
        self.entries["translations/qtbase_de.qm"] = b"qt"
        self.write_portable()

    def tearDown(self):
        self.tmp.cleanup()

    def write_portable(self):
        with zipfile.ZipFile(self.portable, "w", zipfile.ZIP_DEFLATED) as archive:
            for name, data in self.entries.items():
                archive.writestr(name, data)

    def run_audit(self):
        env = os.environ.copy()
        env["APD_REPO_ROOT"] = str(self.repo)
        return subprocess.run(
            [sys.executable, str(AUDITOR), str(self.artifacts), "1.4.0"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env,
            check=False,
        )

    def test_complete_catalogs_and_qt_translations_pass(self):
        result = self.run_audit()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        summary = json.loads(result.stdout)
        self.assertEqual(summary["problems"], [])
        self.assertEqual(summary["portable_forbidden"], [])

    def test_every_application_catalog_is_required(self):
        for locale in ("apd_de_DE", "apd_fr_FR"):
            with self.subTest(locale=locale):
                catalog = f"translations/{locale}.qm"
                data = self.entries.pop(catalog)
                self.write_portable()
                result = self.run_audit()
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                summary = json.loads(result.stdout)
                self.assertIn(catalog, summary["portable_missing_required"])
                self.entries[catalog] = data
        self.write_portable()

    def test_workflow_audits_packaged_dist_before_upload(self):
        workflow = (ROOT / ".github" / "workflows" / "windows.yml").read_text(encoding="utf-8")
        audit_step = workflow.index("- name: Audit packaged release artifacts")
        audit_call = workflow.index("audit_artifacts.py dist $env:VERSION", audit_step)
        exit_check = workflow.index('throw "Release artifact audit failed', audit_call)
        upload_step = workflow.index("- name: Upload build artifacts", exit_check)
        self.assertLess(audit_step, audit_call)
        self.assertLess(audit_call, exit_check)
        self.assertLess(exit_check, upload_step)

    def test_missing_translation_sources_fail_closed(self):
        for source in self.translation_sources.glob("*.ts"):
            source.unlink()
        result = self.run_audit()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("no application translation sources found", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
