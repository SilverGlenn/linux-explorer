#!/usr/bin/env python3
"""Credential-free helper tests: temporary homes, fake commands, no OAuth/FUSE/GUI."""

import contextlib
import csv
import importlib.util
import io
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

sys.dont_write_bytecode = True

SPEC = importlib.util.spec_from_file_location(
    "google_drive_rclone", Path(__file__).resolve().parents[1] / "scripts/google-drive-rclone.py")
HELPER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HELPER)


def mount_record(source=HELPER.REMOTE, fstype="fuse.rclone", mode="ro"):
    return [{"source": source, "fstype": fstype, "options": {mode, "nosuid", "nodev"}}]


class FakeHelper(HELPER.DriveHelper):
    def __init__(self, home):
        super().__init__(home, environment={})
        self.records = []
        self.started = 0
        self.opened = 0
        self.listed = 0
        self.commands = []
        self.cancel_start = False
        self.fail_listing = False
        self.fail_open = False

    def mount_records(self):
        return self.records

    def start_mount(self):
        self.started += 1
        self.records = mount_record()
        if self.cancel_start:
            raise KeyboardInterrupt()

    def verify_listing(self):
        self.listed += 1
        if self.started:
            assert stat.S_IMODE(self.mount.lstat().st_mode) == 0
        if self.fail_listing:
            raise HELPER.HelperError("Fake listing failed.")

    def open_explorer(self):
        self.opened += 1
        if self.fail_open:
            raise HELPER.HelperError("Fake Explorer unavailable.")

    def run_private(self, args, timeout=30):
        self.commands.append(args)
        if "fusermount3" in args[0]:
            self.records = []


class GoogleDriveMountHelperTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="explorer-drive-helper-test-")
        self.home = Path(self.temporary.name)
        self.helper = FakeHelper(self.home)
        self.output = io.StringIO()
        self.output_context = contextlib.redirect_stdout(self.output)
        self.output_context.__enter__()
        self.binary_patch = mock.patch.object(HELPER, "require_binary", side_effect=lambda name: "/usr/bin/" + name)
        self.binary_patch.start()
        original_exists = Path.exists
        self.fuse_patch = mock.patch.object(Path, "exists",
            lambda path: True if str(path) == "/dev/fuse" else original_exists(path))
        self.fuse_patch.start()

    def tearDown(self):
        self.fuse_patch.stop()
        self.binary_patch.stop()
        self.output_context.__exit__(None, None, None)
        # The backing folder deliberately ends mode000, so restore only this
        # test-owned temporary tree for cleanup (never a live mount).
        if self.helper.mount.exists() and not self.helper.mount.is_symlink():
            self.helper.mount.chmod(0o700)
        self.temporary.cleanup()

    def encrypted_config(self):
        self.helper.safe_directory(self.helper.config_dir, create=True)
        self.helper.config.write_bytes(b"# Encrypted rclone configuration File\n\nRCLONE_ENCRYPT_V0:\nopaque-test-data\n")
        self.helper.config.chmod(0o600)

    def client_json(self, **extra):
        path = self.home / "client.json"
        path.write_text(json.dumps({"installed": {"client_id": "123-test.apps.googleusercontent.com",
                                                  "client_secret": "fake-test-secret"}, **extra}))
        path.chmod(0o600)
        return path

    def test_fixed_paths_honor_absolute_xdg_and_ignore_rclone_overrides(self):
        with mock.patch.dict(os.environ, {"XDG_CONFIG_HOME": str(self.home / "custom-config"),
                                          "XDG_DATA_HOME": str(self.home / "custom-data"), "RCLONE_CONFIG": "/private/other.conf",
                                          "RCLONE_CONFIG_PASS": "never-inherit", "RCLONE_READ_ONLY": "false",
                                          "_RCLONE_CONFIG_KEY_FILE": "/private/unrelated-key"}):
            helper = HELPER.DriveHelper(self.home)
            self.assertEqual(helper.config, self.home / "custom-config/linux-explorer-win7/google-drive/rclone.conf")
            self.assertEqual(helper.mount, self.home / "custom-data/linux-explorer-win7/mounts/google-drive")
            self.assertFalse(any(key.startswith("RCLONE_") for key in HELPER.clean_environment()))
            self.assertNotIn("_RCLONE_CONFIG_KEY_FILE", HELPER.clean_environment())

    def test_relative_xdg_uses_fallback_but_outside_home_xdg_is_rejected(self):
        helper = HELPER.DriveHelper(self.home, environment={"XDG_CONFIG_HOME": "relative", "XDG_DATA_HOME": "relative"})
        self.assertEqual(helper.config, self.home / ".config/linux-explorer-win7/google-drive/rclone.conf")
        self.assertEqual(helper.mount, self.home / ".local/share/linux-explorer-win7/mounts/google-drive")
        with self.assertRaises(HELPER.HelperError):
            HELPER.DriveHelper(self.home, environment={"XDG_CONFIG_HOME": "/tmp/outside-home"})

    def test_client_reads_only_explicit_desktop_keys(self):
        path = self.client_json(unrelated={"token": "not-a-real-token"})
        self.assertEqual(HELPER.desktop_client(path), ("123-test.apps.googleusercontent.com", "fake-test-secret"))
        self.assertNotIn("secret", self.output.getvalue())
        self.assertNotIn("token", self.output.getvalue())

    def test_client_rejects_web_json_control_characters_and_oversize(self):
        path = self.home / "invalid.json"
        cases = [{"web": {"client_id": "123.apps.googleusercontent.com", "client_secret": "fake"}},
                 [], {"installed": {"client_id": "not-google", "client_secret": "fake"}},
                 {"installed": {"client_id": "123.apps.googleusercontent.com", "client_secret": "fake\ntoken = injected"}},
                 {"installed": {"client_id": "123.apps.googleusercontent.com", "client_secret": "fake\x00"}}]
        for document in cases:
            with self.subTest(document=document):
                path.write_text(json.dumps(document))
                with self.assertRaises(HELPER.HelperError):
                    HELPER.desktop_client(path)
        path.write_bytes(b"x" * (64 * 1024 + 1))
        with self.assertRaises(HELPER.HelperError):
            HELPER.desktop_client(path)

    def test_file_validation_rejects_symlink_hardlink_and_fifo(self):
        original = self.client_json()
        link = self.home / "linked.json"
        link.symlink_to(original)
        with self.assertRaises(HELPER.HelperError):
            HELPER.desktop_client(link)
        link.unlink()
        os.link(original, link)
        with self.assertRaises(HELPER.HelperError):
            HELPER.desktop_client(link)
        fifo = self.home / "fifo"
        os.mkfifo(fifo)
        with self.assertRaises(HELPER.HelperError):
            HELPER.checked_file(fifo)

    def test_config_requires_exact_encrypted_header_and_private_permissions(self):
        self.encrypted_config()
        self.assertTrue(HELPER.encrypted_config(self.helper.config))
        for data in (b"[explorer-google-drive]\ntype = drive\n", b"RCLONE_ENCRYPT_V0:extra\n",
                     b"[fake]\n# RCLONE_ENCRYPT_V0:\n"):
            self.helper.config.write_bytes(data)
            self.assertFalse(HELPER.encrypted_config(self.helper.config))
        self.encrypted_config()
        self.helper.config.chmod(0o644)
        with self.assertRaises(HELPER.HelperError):
            self.helper.require_encrypted()

    def test_directory_validation_rejects_symlink_parent_and_world_writable_parent(self):
        outside = self.home / "outside"
        outside.mkdir()
        (self.home / ".config").symlink_to(outside, target_is_directory=True)
        with self.assertRaises(HELPER.HelperError):
            self.helper.safe_directory(self.helper.config_dir, create=True)
        (self.home / ".config").unlink()
        (self.home / ".config").mkdir(mode=0o777)
        (self.home / ".config").chmod(0o777)
        with self.assertRaises(HELPER.HelperError):
            self.helper.safe_directory(self.helper.config_dir, create=True)
        with self.assertRaises(HELPER.HelperError):
            self.helper.safe_directory(self.home / "../escape", create=True)

    def test_private_primary_group_writable_ancestors_are_allowed_without_chmod(self):
        config_root = self.home / ".config"
        config_root.mkdir(mode=0o775)
        config_root.chmod(0o775)
        user = SimpleNamespace(pw_name="test-user", pw_uid=os.getuid(), pw_gid=os.getgid())
        group = SimpleNamespace(gr_gid=os.getgid(), gr_mem=["test-user"])
        with mock.patch.object(HELPER.pwd, "getpwuid", return_value=user), \
             mock.patch.object(HELPER.pwd, "getpwall", return_value=[user]), \
             mock.patch.object(HELPER.grp, "getgrgid", return_value=group):
            self.helper.safe_directory(self.helper.config_dir, create=True)
        self.assertEqual(stat.S_IMODE(config_root.stat().st_mode), 0o775)
        self.assertEqual(stat.S_IMODE(self.helper.config_dir.stat().st_mode), 0o700)

    def test_shared_group_writable_ancestors_and_group_lookup_errors_are_refused(self):
        config_root = self.home / ".config"
        config_root.mkdir(mode=0o775)
        config_root.chmod(0o775)
        user = SimpleNamespace(pw_name="test-user", pw_uid=os.getuid(), pw_gid=os.getgid())
        other_user = SimpleNamespace(pw_name="other-user", pw_uid=os.getuid() + 1, pw_gid=os.getgid())
        for explicit_members, accounts in ((["test-user", "other-user"], [user]),
                                           ([], [user, other_user])):
            with self.subTest(explicit_members=explicit_members, accounts=accounts), \
                 mock.patch.object(HELPER.pwd, "getpwuid", return_value=user), \
                 mock.patch.object(HELPER.pwd, "getpwall", return_value=accounts), \
                 mock.patch.object(HELPER.grp, "getgrgid", return_value=SimpleNamespace(
                     gr_gid=os.getgid(), gr_mem=explicit_members)):
                with self.assertRaises(HELPER.HelperError):
                    self.helper.safe_directory(self.helper.config_dir, create=True)
        with mock.patch.object(HELPER.pwd, "getpwuid", side_effect=KeyError("lookup failed")):
            with self.assertRaises(HELPER.HelperError):
                self.helper.safe_directory(self.helper.config_dir, create=True)
        self.assertEqual(stat.S_IMODE(config_root.stat().st_mode), 0o775)
        self.assertFalse(self.helper.config_dir.exists())

    def test_group_writable_ancestor_requires_users_primary_gid(self):
        user = SimpleNamespace(pw_name="test-user", pw_uid=os.getuid(), pw_gid=os.getgid())
        with mock.patch.object(HELPER.pwd, "getpwuid", return_value=user), \
             mock.patch.object(HELPER.grp, "getgrgid") as group:
            self.assertFalse(HELPER.private_primary_group(os.getgid() + 1))
        group.assert_not_called()

    def test_password_command_uses_rclone_space_delimited_csv_instead_of_shell_quoting(self):
        command = self.helper.password_command()
        self.assertEqual(next(csv.reader(io.StringIO(command), delimiter=" ")),
                         ["/usr/bin/kdialog", "--title", "Explorer Google Drive", "--password",
                          "Unlock your Explorer Google Drive configuration"])
        arguments = ["/path with spaces/program", "--title", 'Both "double" and \'single\' quotes']
        self.assertEqual(next(csv.reader(io.StringIO(HELPER.rclone_command_string(arguments)), delimiter=" ")),
                         arguments)

    def test_lock_rejects_duplicate_operations_and_symbolic_links(self):
        with self.helper.operation_lock():
            self.assertEqual(stat.S_IMODE(self.helper.config_dir.stat().st_mode), 0o700)
            with self.assertRaises(HELPER.HelperError):
                with self.helper.operation_lock():
                    self.fail("Duplicate operation entered the lock")
        lock = self.helper.config_dir / ".mount.lock"
        lock.unlink()
        lock.symlink_to(self.home / "unrelated")
        with self.assertRaises(OSError):
            with self.helper.operation_lock():
                self.fail("Symlink lock accepted")

    def test_mountinfo_parsing_requires_exact_source_type_path_readonly_and_single_mount(self):
        path = self.home / "space and\\slash"
        encoded = str(path).replace("\\", "\\134").replace(" ", "\\040")
        text = f"42 21 0:31 / {encoded} ro,nosuid,nodev shared:7 - fuse.rclone {HELPER.REMOTE} rw,user_id=1000\n"
        records = HELPER.mounts_at(text, path)
        self.assertTrue(HELPER.recognized_mount(records))
        self.assertEqual(HELPER.mounts_at(text, Path(str(path) + "-other")), [])
        self.assertFalse(HELPER.recognized_mount(records + records))
        contradictory = mount_record()
        contradictory[0]["options"].add("rw")
        self.assertFalse(HELPER.recognized_mount(contradictory))
        for records in (mount_record(source="other:"), mount_record(fstype="fuse.sshfs"), mount_record(mode="rw"), []):
            self.assertFalse(HELPER.recognized_mount(records))
        self.assertEqual(HELPER.mounts_at("broken line\n", path), [])

    def test_setup_encrypts_before_oauth_and_never_places_secrets_in_argv(self):
        client = self.client_json()
        calls = []
        def fake_run(args, **kwargs):
            calls.append(args)
            config = Path(args[args.index("--config") + 1])
            self.assertNotIn("fake-test-secret", " ".join(args))
            self.assertNotIn("123-test.apps.googleusercontent.com", " ".join(args))
            self.assertFalse(any(key.startswith("RCLONE_") for key in kwargs["env"]))
            self.assertIn("reconnect", args)
            self.assertTrue(HELPER.encrypted_config(config))
            self.assertEqual(config, self.helper.config)
            self.assertIn("--password-command", args)
            self.assertEqual(args[args.index("--drive-scope") + 1], "drive.readonly")
            return subprocess.CompletedProcess(args, 0)
        def fake_encrypt(config):
            data = config.read_text()
            self.assertIn("scope = drive.readonly", data)
            self.assertNotIn("token", data)
            self.assertEqual(stat.S_IMODE(config.stat().st_mode), 0o600)
            config.write_bytes(b"RCLONE_ENCRYPT_V0:\nopaque-test-data\n")
        with mock.patch.object(HELPER.sys.stdin, "isatty", return_value=True), \
             mock.patch.object(HELPER.sys.stdout, "isatty", return_value=True), \
             mock.patch.object(self.helper, "encrypt_new_config", side_effect=fake_encrypt) as encrypt, \
             mock.patch.object(HELPER.subprocess, "run", side_effect=fake_run):
            self.helper.setup(client_json=client)
        self.assertEqual(encrypt.call_count, 1)
        self.assertEqual(len(calls), 1)
        self.assertTrue(HELPER.encrypted_config(self.helper.config))
        self.assertEqual(list(self.helper.config_dir.glob(".setup-*")), [])

    def test_setup_refuses_unencrypted_result_without_starting_oauth(self):
        client = self.client_json()
        with mock.patch.object(HELPER.sys.stdin, "isatty", return_value=True), \
             mock.patch.object(HELPER.sys.stdout, "isatty", return_value=True), \
             mock.patch.object(self.helper, "encrypt_new_config") as encrypt, \
             mock.patch.object(HELPER.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)) as run:
            with self.assertRaises(HELPER.HelperError):
                self.helper.setup(client_json=client)
        self.assertEqual(encrypt.call_count, 1)
        self.assertEqual(run.call_count, 0)
        self.assertFalse(self.helper.config.exists())
        self.assertEqual(list(self.helper.config_dir.glob(".setup-*")), [])

    def test_setup_preserves_existing_config_and_explicit_reconnect_uses_only_it(self):
        self.encrypted_config()
        original = self.helper.config.read_bytes()
        with mock.patch.object(HELPER.sys.stdin, "isatty", return_value=True), \
             mock.patch.object(HELPER.sys.stdout, "isatty", return_value=True), \
             mock.patch.object(HELPER.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)) as run:
            with self.assertRaises(HELPER.HelperError):
                self.helper.setup(client_json=self.client_json())
            self.assertEqual(run.call_count, 0)
            self.helper.setup(reconnect=True)
            self.assertEqual(run.call_count, 1)
            self.assertIn("reconnect", run.call_args.args[0])
        self.assertEqual(self.helper.config.read_bytes(), original)

    def test_setup_requires_private_interactive_terminal(self):
        with mock.patch.object(HELPER.sys.stdin, "isatty", return_value=False):
            with self.assertRaises(HELPER.HelperError):
                self.helper.setup(client_json=self.home / "never-read.json")
        self.assertFalse(self.helper.config_dir.exists())

    def test_eof_in_masked_setup_prompt_is_reported_as_cancellation(self):
        errors = io.StringIO()
        with mock.patch.object(HELPER, "DriveHelper") as constructor, \
             mock.patch.object(HELPER.os, "getuid", return_value=1000), \
             contextlib.redirect_stderr(errors):
            constructor.return_value.setup.side_effect = EOFError()
            self.assertEqual(HELPER.main(["setup", "--client-json", str(self.home / "unused.json")]), 130)
        self.assertEqual(errors.getvalue(), "Cancelled.\n")

    def test_encryption_fixed_prompt_sequence_never_exposes_general_menu_or_logs_password(self):
        process = mock.Mock()
        process.stdin = io.BytesIO()
        process.stdout = mock.Mock()
        process.stdout.fileno.return_value = 123
        process.wait.return_value = 0
        prompts = [b"e/n/d/r/c/s/q> ", b"a/q> ", b"Enter NEW configuration password:\npassword:",
                   b"Confirm NEW configuration password:\npassword:", b"c/u/q> ", b"e/n/d/r/c/s/q> "]
        answers = []
        process.stdin = mock.Mock()
        process.stdin.closed = False
        process.stdin.write.side_effect = answers.append
        with mock.patch.object(HELPER.getpass, "getpass", return_value="synthetic-password"), \
             mock.patch.object(HELPER.subprocess, "Popen", return_value=process) as popen, \
             mock.patch.object(HELPER.select, "select", return_value=([process.stdout], [], [])), \
             mock.patch.object(HELPER.os, "read", side_effect=prompts):
            self.helper.encrypt_new_config(self.home / "synthetic.conf")
        self.assertEqual(answers, [b"s\n", b"a\n", b"synthetic-password\n", b"synthetic-password\n", b"q\n", b"q\n"])
        self.assertNotIn("synthetic-password", " ".join(popen.call_args.args[0]))
        self.assertNotIn("synthetic-password", self.output.getvalue())
        self.assertEqual(popen.call_args.kwargs["stderr"], subprocess.STDOUT)

    def test_encryption_unexpected_prompt_aborts_without_sending_any_answer(self):
        process = mock.Mock()
        process.stdout.fileno.return_value = 123
        process.stdin.closed = False
        process.poll.return_value = None
        process.wait.return_value = 0
        with mock.patch.object(HELPER.getpass, "getpass", return_value="synthetic-password"), \
             mock.patch.object(HELPER.subprocess, "Popen", return_value=process), \
             mock.patch.object(HELPER.select, "select", return_value=([process.stdout], [], [])), \
             mock.patch.object(HELPER.os, "read", side_effect=[b"Unexpected auth prompt: private-placeholder", b""]):
            with self.assertRaises(HELPER.HelperError) as raised:
                self.helper.encrypt_new_config(self.home / "synthetic.conf")
        process.stdin.write.assert_not_called()
        process.terminate.assert_called_once()
        self.assertNotIn("private-placeholder", str(raised.exception))

    def test_encryption_timeout_and_bad_password_never_proceed_to_oauth(self):
        process = mock.Mock()
        process.stdin.closed = False
        process.poll.return_value = None
        process.wait.return_value = 0
        with mock.patch.object(HELPER.getpass, "getpass", return_value="synthetic-password"), \
             mock.patch.object(HELPER.subprocess, "Popen", return_value=process), \
             mock.patch.object(HELPER.select, "select", return_value=([], [], [])):
            with self.assertRaises(HELPER.HelperError):
                self.helper.encrypt_new_config(self.home / "synthetic.conf")
        process.terminate.assert_called_once()
        for passwords in (("one", "two"), ("", ""), ("bad\nvalue", "bad\nvalue"),
                          (" padded", " padded"), ("padded ", "padded ")):
            with mock.patch.object(HELPER.getpass, "getpass", side_effect=passwords), \
                 mock.patch.object(HELPER.subprocess, "Popen") as popen:
                with self.assertRaises(HELPER.HelperError):
                    self.helper.encrypt_new_config(self.home / "synthetic.conf")
                popen.assert_not_called()

    def test_unexpected_encryption_flow_removes_temporary_config_and_never_authorizes(self):
        with mock.patch.object(HELPER.sys.stdin, "isatty", return_value=True), \
             mock.patch.object(HELPER.sys.stdout, "isatty", return_value=True), \
             mock.patch.object(self.helper, "encrypt_new_config", side_effect=HELPER.HelperError("Unexpected menu")), \
             mock.patch.object(HELPER.subprocess, "run") as run:
            with self.assertRaises(HELPER.HelperError):
                self.helper.setup(client_json=self.client_json())
        run.assert_not_called()
        self.assertFalse(self.helper.config.exists())
        self.assertEqual(list(self.helper.config_dir.glob(".setup-*")), [])

    def test_connect_protects_backing_before_listing_and_opens_only_verified_mount(self):
        self.encrypted_config()
        self.helper.connect()
        self.assertEqual((self.helper.started, self.helper.listed, self.helper.opened), (1, 1, 1))
        self.assertEqual(stat.S_IMODE(self.helper.mount.lstat().st_mode), 0)
        self.assertTrue(HELPER.recognized_mount(self.helper.records))

    def test_no_open_connect_and_duplicate_session_do_not_start_second_mount(self):
        self.encrypted_config()
        self.helper.connect(open_explorer=False)
        self.helper.connect(open_explorer=False)
        self.assertEqual((self.helper.started, self.helper.listed, self.helper.opened), (1, 2, 0))

    def test_mount_start_failure_restores_inaccessible_backing(self):
        self.encrypted_config()
        with mock.patch.object(self.helper, "start_mount", side_effect=HELPER.HelperError("Fake startup failure")):
            with self.assertRaises(HELPER.HelperError):
                self.helper.connect()
        self.assertEqual(stat.S_IMODE(self.helper.mount.lstat().st_mode), 0)
        self.assertEqual(self.helper.opened, 0)

    def test_cancel_new_mount_unmounts_only_new_recognized_mount_and_restores_backing(self):
        self.encrypted_config()
        self.helper.cancel_start = True
        with self.assertRaises(KeyboardInterrupt):
            self.helper.connect()
        self.assertEqual(stat.S_IMODE(self.helper.mount.lstat().st_mode), 0)
        self.assertEqual(self.helper.records, [])
        self.assertEqual(len(self.helper.commands), 1)
        self.assertEqual(self.helper.commands[0], ["/usr/bin/fusermount3", "-u", str(self.helper.mount)])

    def test_failed_listing_unmounts_new_mount_but_does_not_touch_existing_mount(self):
        self.encrypted_config()
        self.helper.fail_listing = True
        with self.assertRaises(HELPER.HelperError):
            self.helper.connect()
        self.assertEqual(self.helper.records, [])
        self.assertEqual(self.helper.opened, 0)
        self.helper.commands.clear()
        self.helper.records = mount_record()
        with self.assertRaises(HELPER.HelperError):
            self.helper.connect()
        self.assertTrue(HELPER.recognized_mount(self.helper.records))
        self.assertEqual(self.helper.commands, [])

    def test_explorer_failure_does_not_unmount_ready_session(self):
        self.encrypted_config()
        self.helper.fail_open = True
        with self.assertRaises(HELPER.HelperError):
            self.helper.connect()
        self.assertTrue(HELPER.recognized_mount(self.helper.records))
        self.assertEqual(self.helper.commands, [])
        self.assertEqual(stat.S_IMODE(self.helper.mount.lstat().st_mode), 0)

    def test_connect_rejects_nonempty_or_symlink_mount_and_does_not_hide_files(self):
        self.encrypted_config()
        self.helper.safe_directory(self.helper.mount, create=True)
        local = self.helper.mount / "unrelated.txt"
        local.write_text("preserve")
        with self.assertRaises(HELPER.HelperError):
            self.helper.connect()
        self.assertEqual(stat.S_IMODE(self.helper.mount.lstat().st_mode), 0o700)
        self.assertEqual(local.read_text(), "preserve")
        local.unlink()
        self.helper.mount.rmdir()
        self.helper.mount.symlink_to(self.home)
        with self.assertRaises(HELPER.HelperError):
            self.helper.connect()
        self.assertEqual(self.helper.started, 0)

    def test_foreign_or_writable_mount_is_never_unmounted(self):
        self.encrypted_config()
        for records in (mount_record(source="other:"), mount_record(mode="rw"), mount_record(fstype="ext4")):
            self.helper.records = records
            with self.assertRaises(HELPER.HelperError):
                self.helper.connect()
            with self.assertRaises(HELPER.HelperError):
                self.helper.disconnect()
        self.assertEqual(self.helper.commands, [])

    def test_disconnect_uses_nonforce_unmount_and_keeps_backing_inaccessible(self):
        self.encrypted_config()
        self.helper.connect(open_explorer=False)
        self.helper.disconnect()
        self.assertEqual(self.helper.records, [])
        self.assertEqual(stat.S_IMODE(self.helper.mount.lstat().st_mode), 0)
        self.assertEqual(self.helper.commands, [["/usr/bin/fusermount3", "-u", str(self.helper.mount)]])
        self.helper.disconnect()
        self.assertEqual(len(self.helper.commands), 1)

    def test_disconnect_preserves_nonempty_unmounted_directory_contents_and_permissions(self):
        self.helper.safe_directory(self.helper.mount, create=True, mode=0o750)
        unrelated = self.helper.mount / "unrelated.txt"
        unrelated.write_text("preserve this local file")
        with self.assertRaises(HELPER.HelperError):
            self.helper.disconnect()
        self.assertEqual(unrelated.read_text(), "preserve this local file")
        self.assertEqual(stat.S_IMODE(self.helper.mount.lstat().st_mode), 0o750)
        self.assertEqual(self.helper.commands, [])

    def test_disconnect_does_not_create_absent_mount_directory(self):
        self.helper.disconnect()
        self.assertFalse(self.helper.mount.exists())
        self.assertFalse(self.helper.mount.parent.exists())
        self.assertEqual(self.helper.commands, [])

    def test_disconnect_refuses_mount_race_before_changing_backing_permissions(self):
        self.helper.safe_directory(self.helper.mount, create=True, mode=0o750)
        # First: initial disconnect snapshot. Second: before opening the backing
        # directory. Third: another mount appears during context entry.
        with mock.patch.object(self.helper, "mount_records", side_effect=[[], [], mount_record(source="other:")]):
            with self.assertRaises(HELPER.HelperError):
                self.helper.disconnect()
        self.assertEqual(stat.S_IMODE(self.helper.mount.lstat().st_mode), 0o750)
        self.assertEqual(self.helper.commands, [])

    def test_status_is_readonly_and_does_not_unlock_or_emit_config_contents(self):
        helper = HELPER.DriveHelper(self.home, environment={})
        with mock.patch.object(helper, "mount_records", return_value=[]):
            helper.status()
        self.assertFalse(helper.config_dir.exists())
        self.assertIn('"encrypted_config": false', self.output.getvalue())
        self.encrypted_config()
        self.helper.status()
        self.assertNotIn("opaque-test-data", self.output.getvalue())
        self.assertEqual(self.helper.commands, [])

    def test_mount_command_has_readonly_private_modes_no_secret_and_no_persistent_log(self):
        self.encrypted_config()
        process = mock.Mock()
        process.wait.return_value = 0
        with mock.patch.object(HELPER.subprocess, "Popen", return_value=process) as popen:
            HELPER.DriveHelper.start_mount(self.helper)
        argv = popen.call_args.args[0]
        self.assertIn("--daemon", argv)
        self.assertIn("--read-only", argv)
        for option, expected in {"--drive-scope": "drive.readonly", "--vfs-cache-mode": "off",
                                 "--umask": "077", "--dir-perms": "0700", "--file-perms": "0600",
                                 "--devname": HELPER.REMOTE}.items():
            self.assertEqual(argv[argv.index(option) + 1], expected)
        for forbidden in ("--allow-other", "--allow-root", "--allow-non-empty", "--log-file", "--rc"):
            self.assertNotIn(forbidden, argv)
        self.assertIn("--password", argv[argv.index("--password-command") + 1])
        self.assertEqual(popen.call_args.kwargs["stdout"], subprocess.DEVNULL)

    def test_mount_launcher_timeout_interrupts_only_its_own_child(self):
        process = mock.Mock()
        process.wait.side_effect = [subprocess.TimeoutExpired("private-command", 90), 0]
        with mock.patch.object(HELPER.subprocess, "Popen", return_value=process):
            with self.assertRaises(HELPER.HelperError):
                HELPER.DriveHelper.start_mount(self.helper)
        process.send_signal.assert_called_once_with(HELPER.signal.SIGINT)
        process.kill.assert_not_called()

    def test_private_command_errors_never_include_child_output(self):
        helper = HELPER.DriveHelper(self.home, environment={})
        with mock.patch.object(HELPER.subprocess, "run", side_effect=subprocess.CalledProcessError(
                1, ["command"], output="private-token-placeholder", stderr="private-response-placeholder")):
            with self.assertRaises(HELPER.HelperError) as raised:
                helper.run_private(["command"])
        self.assertNotIn("private-token-placeholder", str(raised.exception))
        self.assertNotIn("private-response-placeholder", str(raised.exception))

    def test_explorer_launch_is_detached_and_never_waits_or_kills_gui(self):
        helper = HELPER.DriveHelper(self.home, environment={})
        with mock.patch.object(HELPER.subprocess, "Popen") as popen:
            helper.open_explorer()
        self.assertEqual(popen.call_args.args[0], ["/usr/bin/explorer-win7", "--map-google-drive-mount"])
        self.assertTrue(popen.call_args.kwargs["start_new_session"])
        popen.return_value.wait.assert_not_called()
        popen.return_value.kill.assert_not_called()

    def app_envelope(self, token=True):
        return {"version": 1, "client_id": "123-test.apps.googleusercontent.com",
                "client_secret": "synthetic-app-client-secret", "config_password": "synthetic_wallet_key_0000000000000001",
                "token": {"access_token": "synthetic-app-access", "refresh_token": "synthetic-app-refresh",
                          "token_type": "Bearer", "expiry": "2030-01-01T00:00:00Z"} if token else None}

    def app_helper(self):
        return HELPER.AppDriveHelper(self.home, environment={})

    def app_encrypted_config(self, helper):
        helper.safe_directory(helper.config_dir, create=True)
        helper.config.write_bytes(b"RCLONE_ENCRYPT_V0:\nopaque-app-test-data\n")
        helper.config.chmod(0o600)

    def test_app_envelope_strict_bounded_private_and_allows_existing_config(self):
        envelope = self.app_envelope()
        self.assertEqual(HELPER.app_credentials(io.BytesIO(json.dumps(envelope).encode())), envelope)
        self.assertIsNone(HELPER.app_credentials(io.BytesIO(json.dumps(self.app_envelope(False)).encode()))["token"])
        invalid = [b"x" * (HELPER.APP_ENVELOPE_LIMIT + 1), b'[]', b'{"version":1,"version":1}',
                   json.dumps(dict(envelope, extra="unexpected")).encode(),
                   json.dumps(dict(envelope, version=True)).encode(),
                   json.dumps(dict(envelope, config_password="short")).encode(),
                   json.dumps(dict(envelope, client_secret="bad\nsecret")).encode(),
                   json.dumps(dict(envelope, token=dict(envelope["token"], expiry="no-timezone"))).encode(),
                   json.dumps(dict(envelope, token=dict(envelope["token"], token_type="Basic"))).encode(),
                   json.dumps(dict(envelope, token=dict(envelope["token"], refresh_token=""))).encode(),
                   json.dumps(dict(envelope, token=dict(envelope["token"], access_token="bad\nvalue"))).encode(),
                   json.dumps(dict(envelope, token=dict(envelope["token"], extra="unexpected"))).encode()]
        for data in invalid:
            with self.subTest(data_length=len(data)):
                with self.assertRaises(HELPER.HelperError) as raised:
                    HELPER.app_credentials(io.BytesIO(data))
                self.assertEqual(str(raised.exception), "The application credential envelope is invalid or incomplete.")
        self.assertNotIn("synthetic-app", self.output.getvalue())

    def test_app_stdin_envelope_times_out_or_stops_at_limit(self):
        with mock.patch.object(HELPER.sys.stdin, "fileno", return_value=123), \
             mock.patch.object(HELPER.select, "select", return_value=([], [], [])):
            with self.assertRaises(HELPER.HelperError):
                HELPER.app_credentials()
        with mock.patch.object(HELPER.sys.stdin, "fileno", return_value=123), \
             mock.patch.object(HELPER.select, "select", return_value=([123], [], [])), \
             mock.patch.object(HELPER.os, "read", side_effect=[b"x" * 4096] * 17):
            with self.assertRaises(HELPER.HelperError):
                HELPER.app_credentials()

    def test_app_config_uses_sealed_memfd_and_never_plaintext_regular_file(self):
        helper = self.app_helper()
        helper.safe_directory(helper.config_dir, create=True)
        envelope = self.app_envelope()
        descriptor = []
        def encrypt(pending, password, pass_fds):
            self.assertTrue(pending.is_symlink())
            self.assertEqual(password, envelope["config_password"])
            self.assertEqual(len(pass_fds), 1)
            descriptor.extend(pass_fds)
            self.assertIn(envelope["token"]["access_token"].encode(), pending.read_bytes())
            with self.assertRaises(OSError):
                os.write(pass_fds[0], b"must be sealed")
            self.assertEqual([p for p in helper.config_dir.rglob("*") if p.is_file() and not p.is_symlink()], [])
            pending.unlink()
            pending.write_bytes(b"RCLONE_ENCRYPT_V0:\nopaque-app-test-data\n")
            pending.chmod(0o600)
        with mock.patch.object(helper, "encrypt_new_config", side_effect=encrypt):
            helper.install_app_config(envelope)
        self.assertTrue(HELPER.encrypted_config(helper.config))
        self.assertEqual(helper.config.name, "app-rclone.conf")
        self.assertEqual(list(helper.config_dir.glob(".app-setup-*")), [])
        self.assertFalse(self.helper.config.exists())
        with self.assertRaises(OSError):
            os.fstat(descriptor[0])
        self.assertNotIn(envelope["token"]["access_token"].encode(), helper.config.read_bytes())

    def test_app_encryption_failure_preserves_existing_encrypted_config(self):
        helper = self.app_helper()
        self.app_encrypted_config(helper)
        original = helper.config.read_bytes()
        with mock.patch.object(helper, "encrypt_new_config", side_effect=HELPER.HelperError("Synthetic encryption failure")):
            with self.assertRaises(HELPER.HelperError):
                helper.install_app_config(self.app_envelope())
        self.assertEqual(helper.config.read_bytes(), original)
        self.assertEqual(list(helper.config_dir.glob(".app-setup-*")), [])

    def test_app_reuses_current_config_instead_of_overwriting_rotated_token(self):
        helper = self.app_helper()
        self.app_encrypted_config(helper)
        envelope = self.app_envelope(False)
        with mock.patch.object(helper, "mount_records", return_value=[]), \
             mock.patch.object(helper, "read_app_token", return_value={"refresh_token": "new-rotated-token"}) as read, \
             mock.patch.object(helper, "install_app_config") as install, \
             mock.patch.object(helper, "connect_locked") as connect:
            helper.app_connect(envelope, open_explorer=False)
        read.assert_called_once_with(envelope)
        install.assert_not_called()
        connect.assert_called_once_with(False)
        self.assertIsNone(helper.config_password)

    def test_app_refuses_switching_credentials_beneath_active_mount(self):
        helper = self.app_helper()
        for records in (mount_record(), mount_record(mode="rw"), mount_record(source="other:")):
            with mock.patch.object(helper, "mount_records", return_value=records), \
                 mock.patch.object(helper, "install_app_config") as install, \
                 mock.patch.object(helper, "connect_locked") as connect:
                with self.assertRaises(HELPER.HelperError):
                    helper.app_connect(self.app_envelope(), open_explorer=False)
            install.assert_not_called()
            connect.assert_not_called()

    def test_app_config_dump_validates_binding_scope_type_and_private_transport(self):
        helper = self.app_helper()
        self.app_encrypted_config(helper)
        envelope = self.app_envelope(False)
        remote = {"type": "drive", "scope": "drive.readonly", "client_id": envelope["client_id"],
                  "client_secret": envelope["client_secret"], "token": json.dumps(self.app_envelope()["token"])}
        def run(args, **kwargs):
            self.assertEqual(args[-2:], ["config", "dump"])
            self.assertEqual(args[args.index("--config") + 1], str(helper.config))
            self.assertEqual(kwargs["input"], envelope["config_password"].encode() + b"\n")
            self.assertEqual(kwargs["stderr"], subprocess.DEVNULL)
            self.assertNotIn("kdialog", " ".join(args))
            self.assertNotIn(envelope["client_secret"], " ".join(args))
            self.assertNotIn(envelope["config_password"], " ".join(args))
            self.assertNotIn(envelope["config_password"], str(kwargs["env"]))
            return subprocess.CompletedProcess(args, 0, stdout=json.dumps({HELPER.REMOTE[:-1]: remote}).encode())
        with mock.patch.object(HELPER.subprocess, "run", side_effect=run):
            self.assertEqual(helper.read_app_token(envelope), self.app_envelope()["token"])
            for field, value in (("client_id", "other.apps.googleusercontent.com"), ("client_secret", "other"),
                                 ("scope", "drive"), ("type", "local"), ("token", "{}")):
                original = remote[field]
                remote[field] = value
                with self.assertRaises(HELPER.HelperError):
                    helper.read_app_token(envelope)
                remote[field] = original
            remote["token_url"] = "https://untrusted.invalid"
            with self.assertRaises(HELPER.HelperError):
                helper.read_app_token(envelope)
        self.assertNotIn("synthetic-app", self.output.getvalue())

    def test_app_forget_removes_only_valid_unmounted_app_config(self):
        helper = self.app_helper()
        self.encrypted_config()
        self.app_encrypted_config(helper)
        personal = self.helper.config.read_bytes()
        with mock.patch.object(helper, "mount_records", return_value=[]):
            helper.app_forget()
            helper.app_forget()
        self.assertFalse(helper.config.exists())
        self.assertEqual(self.helper.config.read_bytes(), personal)
        self.app_encrypted_config(helper)
        for records in (mount_record(), mount_record(mode="rw"), mount_record(source="other:")):
            with mock.patch.object(helper, "mount_records", return_value=records):
                with self.assertRaises(HELPER.HelperError):
                    helper.app_forget()
            self.assertTrue(helper.config.exists())
        helper.config.unlink()
        helper.config.symlink_to(self.helper.config)
        with mock.patch.object(helper, "mount_records", return_value=[]):
            with self.assertRaises(HELPER.HelperError):
                helper.app_forget()
        self.assertEqual(self.helper.config.read_bytes(), personal)

    def test_app_explicit_token_export_emits_only_valid_token_json(self):
        helper = self.app_helper()
        token = self.app_envelope()["token"]
        with mock.patch.object(helper, "read_app_token", return_value=token):
            helper.app_export_token(self.app_envelope(False))
        self.assertEqual(json.loads(self.output.getvalue()), token)
        self.assertNotIn("client_secret", self.output.getvalue())
        self.assertNotIn("config_password", self.output.getvalue())
        with self.assertRaises(HELPER.HelperError):
            helper.app_export_token(self.app_envelope())

    def test_app_foreground_mount_uses_stdin_key_and_never_daemon_or_dialog(self):
        helper = self.app_helper()
        envelope = self.app_envelope()
        helper.config_password = envelope["config_password"]
        process = mock.Mock()
        process.poll.return_value = None
        with mock.patch.object(helper, "mount_records", return_value=mount_record()), \
             mock.patch.object(HELPER.subprocess, "Popen", return_value=process) as popen:
            helper.start_mount()
        args, kwargs = popen.call_args.args[0], popen.call_args.kwargs
        self.assertNotIn("--daemon", args)
        self.assertNotIn("kdialog", " ".join(args))
        self.assertNotIn(envelope["config_password"], " ".join(args))
        self.assertNotIn(envelope["config_password"], str(kwargs["env"]))
        self.assertIn("--read-only", args)
        self.assertEqual(args[args.index("--password-command") + 1], "/usr/bin/cat")
        self.assertTrue(kwargs["start_new_session"])
        self.assertTrue(kwargs["close_fds"])
        self.assertEqual(kwargs["stdout"], subprocess.DEVNULL)
        self.assertEqual(kwargs["stderr"], subprocess.DEVNULL)
        process.stdin.write.assert_called_once_with(envelope["config_password"].encode() + b"\n")
        process.stdin.close.assert_called_once()
        process.terminate.assert_not_called()

    def test_app_mount_cancellation_terminates_only_its_new_process(self):
        helper = self.app_helper()
        helper.config_password = self.app_envelope()["config_password"]
        process = mock.Mock()
        process.poll.return_value = None
        process.stdin.closed = False
        with mock.patch.object(helper, "mount_records", side_effect=KeyboardInterrupt()), \
             mock.patch.object(HELPER.subprocess, "Popen", return_value=process):
            with self.assertRaises(KeyboardInterrupt):
                helper.start_mount()
        process.terminate.assert_called_once()
        process.wait.assert_called_once_with(timeout=5)
        process.kill.assert_not_called()

    def test_app_cli_routes_credentials_only_from_stdin_and_status_without_unlock(self):
        envelope = self.app_envelope()
        with mock.patch.object(HELPER, "AppDriveHelper") as constructor, \
             mock.patch.object(HELPER, "app_credentials", return_value=envelope) as read, \
             mock.patch.object(HELPER.os, "getuid", return_value=1000):
            self.assertEqual(HELPER.main(["app-connect", "--no-open"]), 0)
            constructor.return_value.app_connect.assert_called_once_with(envelope, open_explorer=False)
            read.assert_called_once_with()
            read.reset_mock()
            self.assertEqual(HELPER.main(["app-status"]), 0)
            constructor.return_value.status.assert_called_once_with()
            read.assert_not_called()


if __name__ == "__main__":
    unittest.main()
