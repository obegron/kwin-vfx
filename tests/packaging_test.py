#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline installer safety, migration and source-package checks. No live bus/GPU."""
import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT=Path(__file__).resolve().parents[1]


def module(name,file):
    spec=importlib.util.spec_from_file_location(name,ROOT/"scripts"/file)
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value


runtime=module("vfx_runtime_setup","prepare-nvidia-runtime.py")
setup=module("vfx_desktop_setup","setup-desktop.py")
package=module("vfx_arch_setup","prepare-arch-package.py")


class RuntimeTests(unittest.TestCase):
    def wheel(self,path,extra=None):
        with zipfile.ZipFile(path,"w") as archive:
            for name in runtime.REQUIRED:
                archive.writestr(name,f"Name: nvidia-vfx\nVersion: {runtime.VERSION}\n" if name==runtime.METADATA else b"fixture")
            archive.writestr(f"nvidia_vfx-{runtime.VERSION}.dist-info/licenses/terms.txt","fixture terms")
            if extra:archive.writestr(extra,"escape")
        return hashlib.sha256(path.read_bytes()).hexdigest()

    def test_corrupt_download_never_installs(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);wheel=root/"bad.whl";wheel.write_bytes(b"corrupt")
            with self.assertRaisesRegex(ValueError,"SHA256"):runtime.install_wheel(wheel,root/"runtime")
            self.assertFalse((root/"runtime").exists())

    def test_atomic_install_idempotence_and_license_retention(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);wheel=root/"good.whl";digest=self.wheel(wheel);destination=root/"runtime"
            with mock.patch.object(runtime,"SHA256",digest):
                self.assertTrue(runtime.install_wheel(wheel,destination)["ready"])
                inode=(destination/"nvvfx/libs/libVideoFX.so").stat().st_ino
                runtime.install_wheel(wheel,destination)
                self.assertEqual(inode,(destination/"nvvfx/libs/libVideoFX.so").stat().st_ino)
            self.assertTrue((destination/f"nvidia_vfx-{runtime.VERSION}.dist-info/licenses/terms.txt").is_file())
            self.assertTrue((destination/"kwin-vfx-runtime.json").is_file())

    def test_traversal_does_not_escape_or_publish(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);wheel=root/"bad.whl";digest=self.wheel(wheel,"../../escape")
            with mock.patch.object(runtime,"SHA256",digest),self.assertRaisesRegex(ValueError,"Unsafe"):
                runtime.install_wheel(wheel,root/"runtime")
            self.assertFalse((root/"escape").exists());self.assertFalse((root/"runtime").exists())

    def test_incomplete_existing_runtime_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);wheel=root/"good.whl";digest=self.wheel(wheel)
            target=root/"runtime";target.mkdir();(target/"keep").write_text("existing")
            with mock.patch.object(runtime,"SHA256",digest),self.assertRaisesRegex(ValueError,"overwrite"):
                runtime.install_wheel(wheel,target)
            self.assertEqual((target/"keep").read_text(),"existing")

    def test_matching_layout_without_provenance_is_not_silently_reused(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);wheel=root/"good.whl";digest=self.wheel(wheel)
            destination=root/"runtime"
            with zipfile.ZipFile(wheel) as archive:archive.extractall(destination)
            with mock.patch.object(runtime,"SHA256",digest),self.assertRaisesRegex(ValueError,"unverified"):
                runtime.install_wheel(wheel,destination)

    def test_verified_download_cache_needs_no_network(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);digest=self.wheel(root/runtime.WHEEL)
            with mock.patch.object(runtime,"SHA256",digest),mock.patch.object(runtime.urllib.request,"urlopen") as request:
                self.assertEqual(runtime.download_wheel(root),root/runtime.WHEEL);request.assert_not_called()


class DesktopSetupTests(unittest.TestCase):
    def test_legacy_preferences_migrate_and_existing_vfx_values_win(self):
        with tempfile.TemporaryDirectory() as directory,mock.patch.dict(os.environ,{"XDG_CONFIG_HOME":directory,"XDG_DATA_HOME":directory,"QT_PLUGIN_PATH":"/tmp/other-qt"}):
            root=Path(directory);native=root/"runtime";(native/"nvvfx/libs").mkdir(parents=True)
            metadata=native/f"nvidia_vfx-{runtime.VERSION}.dist-info/METADATA";metadata.parent.mkdir();metadata.write_text(f"Name: nvidia-vfx\nVersion: {runtime.VERSION}\n")
            (native/"nvvfx/libs/libVideoFX.so").write_text("fixture")
            (root/"kwinrc").write_text(f"[Effect-kwin_vsr]\nStrength=1\nCasStrength=0.75\nFilmGrain=1\nTrueHdrSdr=true\nNvidiaRuntime={native}\n\n[Effect-kwin_vfx]\nStrength=0.8\n")
            args=argparse.Namespace(runtime_root=None,no_autostart=False,user_plugin_path=False)
            plan=setup.enable_plan({"nvidia_backend":True,"plugin_root":"/tmp/user%name/plugins","user_plugin_path":True},args)
            self.assertNotIn("Strength",plan["settings"])
            self.assertEqual(plan["settings"]["CasStrength"],"0.75")
            self.assertEqual(plan["settings"]["FilmGrain"],"1")
            self.assertEqual(plan["settings"]["TrueHdrSdr"],"true")
            self.assertEqual(plan["settings"]["NvidiaRuntime"],str(native))
            self.assertTrue(plan["nvidia_enabled"])
            self.assertIn("/tmp/user%%name/plugins:/tmp/other-qt",plan["service_override"])
            self.assertTrue(plan["requires_login"])
            # Planning is read-only: no service override or autostart is written.
            self.assertFalse((root/"systemd").exists());self.assertFalse((root/"autostart").exists())

    def test_shader_variant_needs_no_runtime(self):
        with tempfile.TemporaryDirectory() as directory,mock.patch.dict(os.environ,{"XDG_CONFIG_HOME":directory,"XDG_DATA_HOME":directory}):
            args=argparse.Namespace(runtime_root=None,no_autostart=True,user_plugin_path=False)
            plan=setup.enable_plan({"nvidia_backend":False,"plugin_root":"/usr/lib/qt6/plugins"},args)
            self.assertFalse(plan["nvidia_enabled"])
            self.assertEqual(plan["settings"]["CasOnly"],"true")
            self.assertEqual(plan["settings"]["TrueHdrSdr"],"false")


class SourcePackageTests(unittest.TestCase):
    def test_reproducible_source_only_archives_and_real_checksums(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            first=package.prepare(ROOT,root/"first",True);second=package.prepare(ROOT,root/"second",False)
            self.assertEqual(first["source_sha256"],second["source_sha256"])
            archive=root/"first"/f"kwin-vfx-{first['version']}.tar.gz"
            with tarfile.open(archive) as tar:
                names=tar.getnames()
                self.assertTrue(any(name.endswith("vfx_worker.cpp") for name in names))
                self.assertFalse(any(any(part.startswith("build") for part in Path(name).parts[:-1])
                                     or "/.git/" in name or name.endswith((".whl",".so",".mp4")) for name in names))
            for folder in ("first","second"):
                recipe=root/folder/"PKGBUILD"
                self.assertIn(first["source_sha256"],recipe.read_text())
                self.assertNotIn("@NVIDIA",recipe.read_text())
                subprocess.run(["bash","-n",str(recipe)],check=True)


if __name__=="__main__":unittest.main()
