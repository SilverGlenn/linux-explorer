#!/usr/bin/env python3
"""Pure configuration fixtures: never use the desktop bus or personal settings."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
import sys
from unittest.mock import patch

# Loading the helper in an out-of-source test must not litter tools/ with pyc.
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("portal_preferences",
    Path(__file__).resolve().parents[1] / "tools/portal-preferences.py")
prefs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prefs)


class PortalPreferencesTest(unittest.TestCase):
    def setUp(self):
        self.sandbox = tempfile.TemporaryDirectory(prefix="portal-preferences.")
        self.addCleanup(self.sandbox.cleanup)
        self.root = Path(self.sandbox.name)
        self.prefix = self.root / "install # % Ω"
        self.config = self.root / "config/xdg-desktop-portal"
        self.system = self.root / "system/xdg-desktop-portal"
        environment = {
            "HOME": str(self.root / "home"), "XDG_CONFIG_HOME": str(self.root / "config"),
            "XDG_CONFIG_DIRS": str(self.root / "system"), "XDG_DATA_HOME": str(self.prefix / "share"),
            "XDG_DATA_DIRS": str(self.root / "system-data"), "XDG_STATE_HOME": str(self.root / "state"),
            "XDG_CURRENT_DESKTOP": "KDE", "XDG_DESKTOP_PORTAL_DIR": "",
        }
        self.environment = patch.dict(os.environ, environment)
        self.environment.start()
        self.addCleanup(self.environment.stop)
        self.search = patch.object(prefs, "config_roots", return_value=[self.config, self.system])
        self.search.start()
        self.addCleanup(self.search.stop)
        self.system.mkdir(parents=True)
        self.system_profile = self.system / "kde-portals.conf"
        self.baseline = ("# Vendor profile\n[preferred]\ndefault=kde\n"
                         "org.freedesktop.impl.portal.Settings=kde;gtk;\n"
                         "org.freedesktop.impl.portal.Secret=kwallet\n")
        self.system_profile.write_text(self.baseline)
        metadata = self.prefix / "share/xdg-desktop-portal/portals/explorer-win7.portal"
        metadata.parent.mkdir(parents=True)
        metadata.write_text("[portal]\nDBusName=" + prefs.BUS_NAME + "\nInterfaces=" + prefs.INTERFACE + ";\n")

    def test_new_profile_clones_existing_and_changes_only_filechooser(self):
        result = prefs.enable(self.prefix)
        target = Path(result["config_file"])
        self.assertEqual(target, self.config / "kde-portals.conf")
        values = prefs.preferred(prefs.read_text(target))
        self.assertEqual(values.pop(prefs.INTERFACE), "explorer-win7;kde;")
        self.assertEqual(values, prefs.preferred(self.baseline))
        self.assertEqual(self.system_profile.read_text(), self.baseline)
        self.assertTrue(Path(result["backup"]).joinpath("manifest.json").exists())
        self.assertTrue(prefs.status()["configured"])

    def test_existing_config_comments_crlf_and_other_groups_preserved(self):
        self.config.mkdir(parents=True)
        target = self.config / "portals.conf"
        before = ("# Mine\r\n[preferred]\r\ndefault=gtk\r\n"
                  + prefs.INTERFACE + "=kde;\r\n"
                  "org.freedesktop.impl.portal.ScreenCast=kde;\r\n"
                  "\r\n[other]\r\nkeep=100% untouched\r\n").encode()
        target.write_bytes(before)
        result = prefs.enable(self.prefix)
        expected = before.replace((prefs.INTERFACE + "=kde;").encode(),
                                  (prefs.INTERFACE + "=explorer-win7;kde;").encode())
        self.assertEqual(target.read_bytes(), expected)
        prefs.restore(Path(result["backup"]))
        self.assertEqual(target.read_bytes(), before)

    def test_enable_is_idempotent_and_restore_removes_only_created_profile(self):
        first = prefs.enable(self.prefix)
        self.assertFalse(prefs.enable(self.prefix)["changed"])
        prefs.restore(Path(first["backup"]))
        self.assertFalse(Path(first["config_file"]).exists())
        self.assertEqual(self.system_profile.read_text(), self.baseline)

    def test_restore_preserves_later_unrelated_user_edits(self):
        self.config.mkdir(parents=True)
        target = self.config / "portals.conf"
        target.write_text("[preferred]\ndefault=kde\n" + prefs.INTERFACE + "=gtk;\n")
        result = prefs.enable(self.prefix)
        with target.open("a") as output:
            output.write("org.freedesktop.impl.portal.ScreenCast=another-backend;\n")
        prefs.restore(Path(result["backup"]))
        values = prefs.preferred(prefs.read_text(target))
        self.assertEqual(values[prefs.INTERFACE], "gtk;")
        self.assertEqual(values["org.freedesktop.impl.portal.ScreenCast"], "another-backend;")

    def test_restore_created_profile_keeps_later_edits(self):
        result = prefs.enable(self.prefix)
        target = Path(result["config_file"])
        with target.open("a") as output:
            output.write("org.freedesktop.impl.portal.Print=gtk;\n")
        prefs.restore(Path(result["backup"]))
        values = prefs.preferred(prefs.read_text(target))
        self.assertNotIn(prefs.INTERFACE, values)
        self.assertEqual(values["org.freedesktop.impl.portal.Print"], "gtk;")
        self.assertEqual(values["default"], "kde")

    def test_restore_does_not_overwrite_later_filechooser_selection(self):
        result = prefs.enable(self.prefix)
        target = Path(result["config_file"])
        text = prefs.patch_preference(prefs.read_text(target), "gtk;")
        target.write_text(text)
        with self.assertRaisesRegex(ValueError, "changed after"):
            prefs.restore(Path(result["backup"]))
        self.assertEqual(target.read_text(), text)

    def test_desktop_specific_profile_wins_over_generic(self):
        self.config.mkdir(parents=True)
        generic = self.config / "portals.conf"
        generic.write_text("[preferred]\ndefault=gtk\n")
        specific = self.config / "kde-portals.conf"
        specific.write_text("[preferred]\ndefault=kde\n")
        result = prefs.enable(self.prefix)
        self.assertEqual(result["config_file"], str(specific))
        self.assertEqual(generic.read_text(), "[preferred]\ndefault=gtk\n")

    def test_baseline_with_explicit_filechooser_restores_after_later_edit(self):
        self.system_profile.write_text(self.baseline + prefs.INTERFACE + "=gtk;\n")
        result = prefs.enable(self.prefix)
        target = Path(result["config_file"])
        with target.open("a") as output:
            output.write("org.freedesktop.impl.portal.Print=kde;\n")
        prefs.restore(Path(result["backup"]))
        self.assertEqual(prefs.preferred(prefs.read_text(target))[prefs.INTERFACE], "gtk;")

    def test_refuse_metadata_that_declares_other_interfaces(self):
        metadata = self.prefix / "share/xdg-desktop-portal/portals/explorer-win7.portal"
        metadata.write_text(metadata.read_text().replace(
            "Interfaces=" + prefs.INTERFACE + ";",
            "Interfaces=" + prefs.INTERFACE + ";org.freedesktop.impl.portal.ScreenCast;"))
        with self.assertRaisesRegex(ValueError, "only Explorer"):
            prefs.enable(self.prefix)
        self.assertFalse(self.config.exists())

    def test_refuse_undiscoverable_prefix_or_override(self):
        with patch.dict(os.environ, {"XDG_DATA_HOME": str(self.root / "elsewhere")}):
            with self.assertRaisesRegex(ValueError, "not in"):
                prefs.enable(self.prefix)
        with patch.dict(os.environ, {"XDG_DESKTOP_PORTAL_DIR": "/some/override"}):
            with self.assertRaisesRegex(ValueError, "overrides"):
                prefs.enable(self.prefix)
        self.assertFalse(self.config.exists())

    def test_refuse_symlink_managed_or_malformed_profile(self):
        self.config.mkdir(parents=True)
        target = self.config / "kde-portals.conf"
        target.symlink_to(self.system_profile)
        with self.assertRaisesRegex(ValueError, "symlink"):
            prefs.enable(self.prefix)
        target.unlink()
        target.write_text("[preferred]\ndefault=kde\ndefault=gtk\n")
        with self.assertRaises(prefs.configparser.Error):
            prefs.enable(self.prefix)
        self.assertEqual(target.read_text(), "[preferred]\ndefault=kde\ndefault=gtk\n")

    def test_refuse_to_invent_other_interface_defaults(self):
        self.system_profile.unlink()
        with self.assertRaisesRegex(ValueError, "No existing"):
            prefs.enable(self.prefix)
        self.assertFalse(self.config.exists())


if __name__ == "__main__":
    unittest.main()
