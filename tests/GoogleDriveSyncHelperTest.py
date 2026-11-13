#!/usr/bin/env python3
"""Credential-free selected-folder sync tests; all data stays in temporary trees."""

import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import shutil
import socket
import stat
import struct
import sys
import tempfile
import threading
import time
import urllib.error
import unittest
from unittest import mock

sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location("google_drive_sync", Path(__file__).resolve().parents[1] / "scripts/google-drive-sync.py")
SYNC = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SYNC)


def file_metadata(name, content=b"data", file_id=None, parent="selected", mime="text/plain"):
    return {"id": file_id or "id_" + name.replace(".", "_"), "name": name, "mimeType": mime,
            "parents": [parent], "size": str(len(content)), "md5Checksum": hashlib.md5(content).hexdigest(),
            "capabilities": {"canDownload": True, "canEdit": True, "canListChildren": True, "canAddChildren": True}}


def folder_metadata(folder_id, name, parent):
    item = file_metadata(name, file_id=folder_id, parent=parent, mime=SYNC.FOLDER)
    if parent is None:
        item.pop("parents")
    return item


class FakeAPI(SYNC.DriveAPI):
    def __init__(self):
        super().__init__({"access_token": "synthetic", "expiry": "2099-01-01T00:00:00Z"})
        self.metadata_items = {"root": folder_metadata("root_id", "My Drive", None),
                               "root_id": folder_metadata("root_id", "My Drive", None),
                               "selected": folder_metadata("selected", "Selected", "root_id")}
        self.items = {"root_id": [self.metadata_items["selected"]], "selected": []}
        self.page_size = 1000

    def metadata(self, folder_id):
        return self.metadata_items[folder_id]

    def listing_items(self, folder_id):
        return self.items.get(folder_id, [])

    def get(self, endpoint, parameters):
        if endpoint != "files" or not parameters["q"].endswith(" and trashed=false"):
            raise AssertionError("Unexpected synthetic Drive request")
        parents = re.findall(r"'([A-Za-z0-9_-]+)' in parents", parameters["q"])
        if not parents:
            raise AssertionError("An unscoped Drive listing was requested")
        items = [item for parent in parents for item in self.listing_items(parent)]
        offset = int(parameters.get("pageToken", "0"))
        end = offset + min(int(parameters["pageSize"]), self.page_size)
        result = {"files": items[offset:end]}
        if end < len(items):
            result["nextPageToken"] = str(end)
        return result


class FakeEngine:
    def __init__(self):
        self.cancel = threading.Event()
        self.commands = []
        self.fail = False

    def run(self, arguments, **_options):
        self.commands.append(list(arguments))
        if self.fail:
            raise SYNC.Error("Synthetic network failure.")
        return b""


class SyncHelperTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="explorer-sync-test-")
        self.home = Path(self.temporary.name)
        self.paths = SYNC.Paths(self.home, {})
        self.local = self.home / "local"
        self.local.mkdir(mode=0o700)
        self.api = FakeAPI()
        self.engine = FakeEngine()
        self.worker = SYNC.Worker(self.paths, self.engine, self.api, "a" * 64, "v1.71.0")

    def tearDown(self):
        self.worker.stopping.set()
        self.worker.wakeup.set()
        self.temporary.cleanup()

    def request(self, action, **fields):
        return self.worker.request({"version": 1, "action": action, **fields})

    def add(self):
        self.request("preview", remote_folder_id="selected", local_path=str(self.local))
        self.request("add", remote_folder_id="selected", local_path=str(self.local))
        return self.worker.state["pairs"][-1]

    def test_json_rejects_duplicate_keys_extra_messages_and_oversize(self):
        for data in (b'{"version":1,"version":1}', b'{"version":NaN}', b'[]', b'{"a":1}\n{"b":2}', b'x' * (SYNC.LIMIT + 1)):
            with self.subTest(data=data[:60]), self.assertRaises(SYNC.Error):
                SYNC.decode_json(data)
        self.assertEqual(SYNC.decode_json(b'{"version":1}'), {"version": 1})

    def test_credential_shape_accepts_only_private_extended_envelope(self):
        envelope = {"version": 1, "client_id": "123-test.apps.googleusercontent.com", "client_secret": "test-secret",
                    "config_password": "s" * 43, "browse_config_password": "b" * 43, "token": None}
        self.assertEqual(SYNC.credentials(envelope)["browse_config_password"], "b" * 43)
        for change in ({"browse_config_password": "bad"}, {"version": True}, {"extra": "rejected"}):
            with self.subTest(change=change), self.assertRaises(SYNC.Error):
                SYNC.credentials(dict(envelope, **change))

    def test_stable_version_gate_rejects_dev_and_old_even_when_numeric_new(self):
        binary = self.home / ".local/libexec/explorer-win7/rclone"
        binary.parent.mkdir(parents=True)
        binary.write_text("#!/bin/sh\n")
        binary.chmod(0o700)
        for version, expected in (("v1.60.1-DEV", False), ("v1.70.9", False), ("v1.71.0", True), ("v1.99.0-beta", False), ("v2.0.0", True)):
            with self.subTest(version=version), mock.patch.object(SYNC.subprocess, "run", return_value=mock.Mock(stdout=("rclone " + version + "\n").encode())):
                selected, found, available = SYNC.binary_version(self.home)
                self.assertEqual(selected, str(binary))
                self.assertEqual(available, expected)
        binary.chmod(0o777)
        self.assertFalse(SYNC.binary_version(self.home)[2])

    def test_local_path_rejects_mount_config_cache_symlink_and_overlap(self):
        outside = self.home / "outside"
        outside.mkdir()
        linked = self.home / "linked"
        linked.symlink_to(outside)
        candidates = [self.home, self.paths.directory, self.paths.helper.mount,
                      self.home / ".cache/sync", linked, self.home / "../escape"]
        for path in candidates:
            with self.subTest(path=path), self.assertRaises(SYNC.Error):
                self.paths.local(str(path))
        pair = self.add()
        with self.assertRaisesRegex(SYNC.Error, "overlap"):
            self.paths.local(str(self.local), self.worker.state["pairs"])
        pair["local_inode"] += 1
        with self.assertRaisesRegex(SYNC.Error, "replaced"):
            self.paths.local(str(self.local), existing_pair=pair)

    def test_local_inventory_rejects_special_files_hardlinks_and_symlinks(self):
        target = self.local / "file"
        target.write_bytes(b"content")
        linked = self.local / "linked"
        linked.symlink_to(target)
        with self.assertRaises(SYNC.Error):
            SYNC.local_inventory(self.local)
        linked.unlink()
        os.link(target, linked)
        with self.assertRaises(SYNC.Error):
            SYNC.local_inventory(self.local)
        linked.unlink()
        target.unlink()
        os.mkfifo(target)
        with self.assertRaises(SYNC.Error):
            SYNC.local_inventory(self.local)

    def test_remote_inventory_rejects_duplicates_native_docs_shortcuts_and_shared_drive(self):
        cases = [[file_metadata("same"), file_metadata("same", file_id="other")],
                 [file_metadata("doc", mime="application/vnd.google-apps.document")],
                 [file_metadata("link", mime="application/vnd.google-apps.shortcut")],
                 [dict(file_metadata("file"), driveId="shared_drive")],
                 [file_metadata("../escape")], [dict(file_metadata("file"), md5Checksum=None)]]
        for items in cases:
            self.api.items["selected"] = items
            with self.subTest(items=items), self.assertRaises(SYNC.Error):
                self.api.inventory("selected")
        self.api.items["selected"] = [file_metadata("valid.txt", b"abc")]
        inventory = self.api.inventory("selected")
        self.assertEqual((inventory["file_count"], inventory["bytes"]), (1, 3))

    def test_broad_folder_preview_fits_existing_deadline_without_writes(self):
        for index in range(80):
            folder_id = "branch_" + str(index).zfill(3) + "x" * 245
            folder = folder_metadata(folder_id, f"Branch {index:02d}", "selected")
            self.api.items["selected"].append(folder)
            self.api.items[folder_id] = [file_metadata("file.txt", parent=folder_id, file_id=f"file_{index}")]
        clock = [0.0]
        known = {"selected"}
        original = self.api.get
        def slow_get(endpoint, parameters):
            parents = re.findall(r"'([A-Za-z0-9_-]+)' in parents", parameters["q"])
            self.assertTrue(set(parents) <= known, "listing escaped the discovered subtree")
            self.assertLessEqual(len(parents), 20, "folder queries must remain bounded")
            self.assertLess(len(SYNC.urllib.parse.urlencode(parameters)), 8192)
            clock[0] += 2
            result = original(endpoint, parameters)
            known.update(item["id"] for item in result["files"] if item["mimeType"] == SYNC.FOLDER)
            return result
        with mock.patch.object(SYNC.time, "monotonic", side_effect=lambda: clock[0]), mock.patch.object(self.api, "get", side_effect=slow_get):
            try:
                result = self.request("preview", remote_folder_id="selected", local_path=str(self.local))
            except SYNC.MetadataTimeout:
                self.fail("A supported broad folder timed out because its subfolders were listed one at a time")
        self.assertEqual(result["preview"]["file_count"], 80)
        self.assertEqual(result["preview"]["bytes"], 320)
        self.assertLess(clock[0], 120)
        self.assertFalse(self.engine.commands)
        self.assertFalse(list(self.local.iterdir()))
        self.assertFalse(self.worker.state["pairs"])

    def test_paginated_inventory_keeps_same_names_in_distinct_folders(self):
        left = folder_metadata("left", "Left", "selected")
        right = folder_metadata("right", "Right", "selected")
        self.api.items.update(selected=[left, right],
                              left=[file_metadata("same.txt", b"one", parent="left", file_id="left_file")],
                              right=[file_metadata("same.txt", b"two!", parent="right", file_id="right_file")])
        self.api.page_size = 1
        inventory = self.api.inventory("selected")
        self.assertEqual(set(inventory["files"]), {"Left/same.txt", "Right/same.txt"})
        self.assertEqual(inventory["files"]["Left/same.txt"]["size"], 3)
        self.assertEqual(inventory["files"]["Right/same.txt"]["size"], 4)
        self.assertEqual((inventory["file_count"], inventory["bytes"]), (2, 7))
        self.api.items["selected"].reverse()
        self.assertEqual(self.api.inventory("selected")["fingerprint"], inventory["fingerprint"])
        self.api.items["left"].append(file_metadata("same.txt", parent="left", file_id="duplicate"))
        with self.assertRaisesRegex(SYNC.Error, "Duplicate"):
            self.api.inventory("selected")

    def test_inventory_rejects_results_outside_scoped_parents(self):
        for parents in (["outside"], ["selected", "outside"], [], None, [["selected"]]):
            self.api.items["selected"] = [dict(file_metadata("file.txt"), parents=parents)]
            with self.subTest(parents=parents), self.assertRaises(SYNC.Error):
                self.api.inventory("selected")

    def test_inventory_rejects_repeated_folders_and_total_item_overflow(self):
        folder = folder_metadata("child", "Child", "selected")
        self.api.items["selected"] = [folder, dict(folder, name="Other")]
        with self.assertRaisesRegex(SYNC.Error, "repeated folder"):
            self.api.inventory("selected")
        self.api.items["selected"] = [folder, folder_metadata("sibling", "Sibling", "selected")]
        self.api.items["child"] = [file_metadata("one", parent="child")]
        self.api.items["sibling"] = [file_metadata("two", parent="sibling")]
        with mock.patch.object(SYNC, "MAX_ITEMS", 3), self.assertRaisesRegex(SYNC.Error, "item safety limit"):
            self.api.inventory("selected")

    def test_inventory_deadline_and_cancellation_apply_between_listing_pages(self):
        self.api.items["selected"] = [file_metadata("one"), file_metadata("two")]
        self.api.page_size = 1
        original = self.api.get
        for cancelled in (False, True):
            clock = [0.0]
            self.api.cancel.clear()
            def slow_get(endpoint, parameters):
                clock[0] += 61
                if cancelled and parameters.get("pageToken"):
                    self.api.cancel.set()
                return original(endpoint, parameters)
            expected = "paused or stopped" if cancelled else "timed out"
            with self.subTest(cancelled=cancelled), mock.patch.object(SYNC.time, "monotonic", side_effect=lambda: clock[0]), mock.patch.object(self.api, "get", side_effect=slow_get):
                with self.assertRaisesRegex(SYNC.Error, expected):
                    self.api.inventory("selected")
        self.api.cancel.clear()

    def test_inventory_rejects_incomplete_or_malformed_listing_pages(self):
        for document in ({"incompleteSearch": True, "files": []}, {"files": None},
                         {"files": [], "nextPageToken": 123}):
            with self.subTest(document=document), mock.patch.object(self.api, "get", return_value=document):
                with self.assertRaises(SYNC.Error):
                    self.api.inventory("selected")

    def test_folder_must_be_writable_and_beneath_my_drive_root(self):
        for folder_id in ("root", "invalid:remote", "../parent"):
            with self.subTest(folder_id=folder_id), self.assertRaises(SYNC.Error):
                self.api.folder(folder_id)
        self.api.metadata_items["selected"]["driveId"] = "shared"
        with self.assertRaises(SYNC.Error):
            self.api.folder("selected")
        self.api.metadata_items["selected"].pop("driveId")
        self.api.metadata_items["selected"]["capabilities"]["canAddChildren"] = False
        with self.assertRaises(SYNC.Error):
            self.api.folder("selected")

    def test_preview_is_read_only_and_add_requires_fresh_review_empty_target(self):
        with self.assertRaisesRegex(SYNC.Error, "Preview"):
            self.request("add", remote_folder_id="selected", local_path=str(self.local))
        preview = self.request("preview", remote_folder_id="selected", local_path=str(self.local))
        self.assertEqual(preview["preview"]["file_count"], 0)
        self.assertFalse(self.engine.commands)
        self.assertFalse(list(self.local.iterdir()))
        (self.local / "user.txt").write_text("keep")
        with self.assertRaisesRegex(SYNC.Error, "empty"):
            self.request("add", remote_folder_id="selected", local_path=str(self.local))
        (self.local / "user.txt").unlink()
        key = ("selected", str(self.local))
        old = self.worker.previews[key]
        self.worker.previews[key] = (time.monotonic() - 301, *old[1:])
        with self.assertRaisesRegex(SYNC.Error, "Preview"):
            self.request("add", remote_folder_id="selected", local_path=str(self.local))

    def test_account_binding_pause_and_needs_review_survive_worker_restart(self):
        pair = self.add()
        pair["initialized"], pair["state"] = True, "paused"
        self.paths.save(self.worker.state)
        restarted = SYNC.Worker(self.paths, self.engine, self.api, "a" * 64, "v1.71.0")
        self.assertEqual(restarted.state["pairs"][0]["state"], "paused")
        pair["state"] = "needs-review"
        self.paths.save(self.worker.state)
        restarted = SYNC.Worker(self.paths, self.engine, self.api, "a" * 64, "v1.71.0")
        self.assertEqual(restarted.state["pairs"][0]["state"], "needs-review")
        switched = SYNC.Worker(self.paths, self.engine, self.api, "b" * 64, "v1.71.0")
        self.assertIn("different", switched.state["pairs"][0]["error"])
        with self.assertRaisesRegex(SYNC.Error, "another"):
            switched.request({"version": 1, "action": "resume", "pair_id": pair["id"]})

    def test_remote_pairs_cannot_be_nested(self):
        self.add()
        child = folder_metadata("child", "Child", "selected")
        self.api.metadata_items["child"] = child
        self.api.items["selected"] = [child]
        second = self.home / "second"
        second.mkdir()
        with self.assertRaisesRegex(SYNC.Error, "overlap"):
            self.request("preview", remote_folder_id="child", local_path=str(second))

    def test_parent_preview_reports_stalled_child_overlap_before_recursive_listing(self):
        child = folder_metadata("child", "Child", "selected")
        self.api.metadata_items["child"] = child
        self.api.items["selected"] = [child]
        self.request("preview", remote_folder_id="child", local_path=str(self.local))
        self.request("add", remote_folder_id="child", local_path=str(self.local))
        pair = self.worker.state["pairs"][0]
        pair["state"] = "needs-review"
        destination = self.home / "parent-copy"
        destination.mkdir()
        with mock.patch.object(self.api, "get", side_effect=AssertionError("Recursive listing ran before checking the known overlap")):
            with self.assertRaisesRegex(SYNC.Error, "overlap"):
                self.request("preview", remote_folder_id="selected", local_path=str(destination))
        self.assertEqual(pair["state"], "needs-review")
        self.assertFalse(pair["initialized"])
        self.assertFalse(list(destination.iterdir()))
        self.assertFalse(self.engine.commands)

    def test_unsafe_state_and_lock_are_rejected(self):
        self.paths.state_file.chmod(0o644)
        with self.assertRaises(SYNC.Error):
            self.paths.read_state()
        self.paths.state_file.chmod(0o600)
        self.paths.lock_file.symlink_to(self.paths.state_file)
        with self.assertRaises(SYNC.Error):
            with self.paths.lock():
                self.fail("symlink lock accepted")

    def test_deletion_guard_stops_before_invoking_rclone(self):
        pair = self.add()
        pair["initialized"] = True
        sentinel = ".explorer-sync-access-" + pair["id"]
        (self.local / sentinel).write_text("sentinel")
        remote_sentinel = file_metadata(sentinel, b"sentinel", file_id="access")
        self.api.items["selected"] = [remote_sentinel]
        pair_dir = self.paths.directory / "pairs" / pair["id"]
        manifest = {sentinel: {}, **{f"file{index}": {} for index in range(20)}}
        self.paths.atomic_json(pair_dir / "manifest.json", {"local": manifest, "remote": manifest})
        with self.assertRaisesRegex(SYNC.Error, "ten files|ten percent"):
            self.worker.run_pair(pair)
        self.assertFalse(self.engine.commands)

    def test_missing_sentinel_stops_before_sync_or_recovery(self):
        pair = self.add()
        pair["initialized"] = True
        self.paths.atomic_json(self.paths.directory / "pairs" / pair["id"] / "manifest.json", {"local": {}, "remote": {}})
        with self.assertRaisesRegex(SYNC.Error, "sentinel"):
            self.worker.run_pair(pair)
        self.assertFalse(self.engine.commands)

    def test_low_disk_space_stops_before_any_sync_mutations(self):
        pair = self.add()
        pair["initialized"] = True
        sentinel = ".explorer-sync-access-" + pair["id"]
        (self.local / sentinel).write_bytes(b"sentinel")
        self.api.items["selected"] = [file_metadata(sentinel, b"sentinel", file_id="access")]
        self.paths.atomic_json(self.paths.directory / "pairs" / pair["id"] / "manifest.json", {"local": {sentinel: {}}, "remote": {sentinel: {}}})
        with mock.patch.object(SYNC.shutil, "disk_usage", return_value=mock.Mock(free=64 * 1024 * 1024 - 1)):
            with self.assertRaisesRegex(SYNC.Error, "free disk space"):
                self.worker.run_pair(pair)
        self.assertFalse(self.engine.commands)

    def test_regular_sync_preserves_conflicts_and_never_resyncs(self):
        pair = self.add()
        pair["initialized"] = True
        sentinel = ".explorer-sync-access-" + pair["id"]
        (self.local / sentinel).write_bytes(b"sentinel")
        remote_sentinel = file_metadata(sentinel, b"sentinel", file_id="access")
        self.api.items["selected"] = [remote_sentinel]
        self.paths.atomic_json(self.paths.directory / "pairs" / pair["id"] / "manifest.json", {"local": {sentinel: {}}, "remote": {sentinel: {}}})
        self.worker.run_pair(pair)
        command = self.engine.commands[-1]
        self.assertEqual(command[0], "bisync")
        self.assertNotIn("--resync", command)
        self.assertNotIn("--resync-mode", command)
        self.assertEqual(command[command.index("--conflict-resolve") + 1], "none")
        for flag in ("--backup-dir1", "--backup-dir2", "--check-access", "--recover", "--resilient"):
            self.assertIn(flag, command)
        self.assertNotIn("--force", command)
        self.assertNotIn(str(self.paths.helper.mount), command)

    def test_failed_sync_retains_manifest_and_worker_requires_review(self):
        pair = self.add()
        pair["initialized"], pair["state"] = True, "idle"
        manifest_path = self.paths.directory / "pairs" / pair["id"] / "manifest.json"
        self.paths.atomic_json(manifest_path, {"local": {}, "remote": {}})
        before = manifest_path.read_bytes()
        self.worker.run_pair = mock.Mock(side_effect=SYNC.Error("Synthetic failure; retained baseline."))
        thread = threading.Thread(target=self.worker.schedule)
        thread.start()
        deadline = time.monotonic() + 2
        while pair["state"] != "needs-review" and time.monotonic() < deadline:
            time.sleep(0.01)
        self.worker.stopping.set()
        self.worker.wakeup.set()
        thread.join(timeout=2)
        self.assertEqual(pair["state"], "needs-review")
        self.assertEqual(manifest_path.read_bytes(), before)
        self.worker.run_pair.assert_called_once()

    def test_interrupted_initial_copy_can_resume_without_resetting_review(self):
        pair = self.add()
        reviewed = pair["reviewed_hash"]
        for state in ("needs-review", "paused"):
            pair["state"], pair["error"] = state, "Interrupted initial copy"
            public = self.worker.response()["pairs"][0]
            self.assertTrue(public.get("can_retry_initial"))
            self.assertFalse(public["can_resume"], "partial copies must not enable ordinary Sync now or deletion review")
            self.assertFalse(SYNC.public_pair(pair, "b" * 64).get("can_retry_initial"))
            response = self.request("resume", pair_id=pair["id"])
            self.assertEqual(response["pairs"][0]["state"], "initializing")
            self.assertEqual(pair["reviewed_hash"], reviewed)
            self.assertFalse(pair["initialized"])
            self.assertFalse(self.engine.commands)
        pair["state"] = "needs-review"
        with self.assertRaises(SYNC.Error):
            self.request("sync-now", pair_id=pair["id"])
        with self.assertRaises(SYNC.Error):
            self.request("review", pair_id=pair["id"])

    def test_initial_copy_rejects_edited_or_unexpected_partial_files_before_writes(self):
        self.api.items["selected"] = [file_metadata("downloaded.txt", b"original")]
        pair = self.add()
        for name, content in (("downloaded.txt", b"user edit"), ("new-local.txt", b"keep")):
            local_file = self.local / name
            local_file.write_bytes(content)
            with self.subTest(name=name), self.assertRaisesRegex(SYNC.Error, "partial download"):
                self.worker.run_pair(pair)
            self.assertEqual(local_file.read_bytes(), content)
            self.assertFalse(self.engine.commands)
            self.assertFalse(pair["initialized"])
            local_file.unlink()

    def test_initial_copy_rejects_unexpected_partial_directories_before_writes(self):
        pair = self.add()
        unexpected = self.local / "new-local-folder"
        unexpected.mkdir()
        with self.assertRaisesRegex(SYNC.Error, "partial download"):
            self.worker.run_pair(pair)
        self.assertTrue(unexpected.is_dir())
        self.assertFalse(self.engine.commands)
        self.assertFalse(pair["initialized"])

    def test_partial_initial_copy_still_rejects_changed_cloud_snapshot(self):
        self.api.items["selected"] = [file_metadata("downloaded.txt", b"original")]
        pair = self.add()
        (self.local / "downloaded.txt").write_bytes(b"original")
        self.api.items["selected"].append(file_metadata("cloud-change.txt"))
        with self.assertRaisesRegex(SYNC.Error, "changed after its preview"):
            self.worker.run_pair(pair)
        self.assertFalse(self.engine.commands)
        self.assertEqual((self.local / "downloaded.txt").read_bytes(), b"original")

    def test_partial_initial_copy_cannot_restart_already_started_baseline(self):
        pair = self.add()
        sentinel = self.local / (".explorer-sync-access-" + pair["id"])
        sentinel.write_bytes(b"keep existing setup marker")
        with self.assertRaisesRegex(SYNC.Error, "reserved"):
            self.worker.run_pair(pair)
        self.assertEqual(sentinel.read_bytes(), b"keep existing setup marker")
        self.assertFalse(self.engine.commands)
        self.assertFalse(pair["initialized"])

    def test_partial_initial_copy_cannot_reset_existing_bisync_state(self):
        pair = self.add()
        workdir = self.paths.directory / "pairs" / pair["id"] / "bisync"
        self.paths.helper.safe_directory(workdir, create=True)
        baseline = workdir / "retained.lst"
        baseline.write_bytes(b"keep the existing baseline")
        with self.assertRaisesRegex(SYNC.Error, "initial baseline"):
            self.worker.run_pair(pair)
        self.assertEqual(baseline.read_bytes(), b"keep the existing baseline")
        self.assertFalse(self.engine.commands)
        self.assertFalse(pair["initialized"])

    def test_cloud_change_after_preview_blocks_initial_download(self):
        pair = self.add()
        self.api.items["selected"] = [file_metadata("unreviewed.txt")]
        with self.assertRaisesRegex(SYNC.Error, "changed after its preview"):
            self.worker.run_pair(pair)
        self.assertFalse(self.engine.commands)
        self.assertFalse(list(self.local.iterdir()))

    def test_transient_metadata_failure_retries_initialized_pair_without_resync(self):
        pair = self.add()
        pair["initialized"], pair["state"] = True, "idle"
        self.worker.run_pair = mock.Mock(side_effect=SYNC.TransientError("Offline; retry automatically."))
        thread = threading.Thread(target=self.worker.schedule)
        thread.start()
        deadline = time.monotonic() + 2
        while pair["state"] != "error" and time.monotonic() < deadline:
            time.sleep(0.01)
        self.worker.stopping.set()
        self.worker.wakeup.set()
        thread.join(timeout=2)
        self.assertEqual(pair["state"], "error")
        self.assertGreater(self.worker.next_run[pair["id"]], time.monotonic())
        self.worker.run_pair.assert_called_once()

    def test_http_transient_errors_are_distinct_from_permission_errors(self):
        api = SYNC.DriveAPI({"access_token": "synthetic", "expiry": "2099-01-01T00:00:00Z"})
        opener = mock.Mock()
        for code, expected in ((429, SYNC.TransientError), (503, SYNC.TransientError), (403, SYNC.Error)):
            opener.open.side_effect = urllib.error.HTTPError("https://example.invalid", code, "synthetic", {}, None)
            with self.subTest(code=code), mock.patch.object(SYNC.urllib.request, "build_opener", return_value=opener), mock.patch.object(api.cancel, "wait", return_value=False):
                with self.assertRaises(expected):
                    api.get("about", {"fields": "user(permissionId)"})
        api.cancel.set()
        with self.assertRaisesRegex(SYNC.Error, "paused or stopped"):
            api.get("about", {})

    def test_inventory_deadline_includes_ancestry_and_does_not_restart(self):
        clock = [0.0]
        original = self.api.metadata
        def slow_metadata(folder_id):
            clock[0] += 61
            return original(folder_id)
        with mock.patch.object(SYNC.time, "monotonic", side_effect=lambda: clock[0]), mock.patch.object(self.api, "metadata", side_effect=slow_metadata):
            with self.assertRaisesRegex(SYNC.MetadataTimeout, "timed out.*smaller folder"):
                self.api.inventory("selected")
        self.assertFalse(self.engine.commands)

    def test_request_deadline_reaches_token_refresh_and_prevents_http_after_expiry(self):
        clock = [0.0]
        api = SYNC.DriveAPI({"access_token": "synthetic", "expiry": "2000-01-01T00:00:00Z"})
        received = []
        def refresh(deadline=None):
            received.append(deadline)
            clock[0] = 121
            return {"access_token": "synthetic-new", "expiry": "2099-01-01T00:00:00Z"}
        api.refresh = refresh
        with mock.patch.object(SYNC.time, "monotonic", side_effect=lambda: clock[0]), mock.patch.object(SYNC.urllib.request, "build_opener") as opener:
            with self.assertRaises(SYNC.MetadataTimeout):
                with api.budget(120):
                    api.get("about", {})
        self.assertEqual(received, [120.0])
        opener.assert_not_called()

    def test_retry_backoff_and_http_timeout_share_one_budget(self):
        clock = [0.0]
        api = SYNC.DriveAPI({"access_token": "synthetic", "expiry": "2099-01-01T00:00:00Z"})
        opener = mock.Mock()
        opener.open.side_effect = urllib.error.URLError("synthetic unavailable")
        def advance(seconds):
            clock[0] += seconds
            return False
        with mock.patch.object(SYNC.time, "monotonic", side_effect=lambda: clock[0]), mock.patch.object(SYNC.urllib.request, "build_opener", return_value=opener), mock.patch.object(api.cancel, "wait", side_effect=advance) as wait:
            with self.assertRaises(SYNC.MetadataTimeout):
                with api.budget(0.25):
                    api.get("files", {})
        self.assertEqual(opener.open.call_count, 1)
        self.assertLessEqual(opener.open.call_args.kwargs["timeout"], 0.25)
        self.assertLessEqual(wait.call_args.args[0], 0.25)

    def test_review_hash_respects_metadata_deadline_between_chunks(self):
        (self.local / "large.bin").write_bytes(b"x" * (3 * 1024 * 1024))
        clock = iter((0.0, 0.5, 1.0, 1.5))
        with mock.patch.object(SYNC.time, "monotonic", side_effect=lambda: next(clock)):
            with self.assertRaises(SYNC.MetadataTimeout):
                SYNC.local_inventory(self.local, hash_files=True, deadline=0.75)

    def test_socket_read_timeout_is_total_instead_of_per_chunk(self):
        clock = [0.0]
        connection = mock.Mock()
        chunks = iter((b'{"version":', b'1}', b''))
        def receive(_limit):
            clock[0] += 0.6
            return next(chunks)
        connection.recv.side_effect = receive
        with mock.patch.object(SYNC.time, "monotonic", side_effect=lambda: clock[0]):
            with self.assertRaisesRegex(SYNC.Error, "timed out"):
                SYNC.receive_socket(connection, timeout=1)
        self.assertEqual(connection.settimeout.call_count, 2)
        self.assertAlmostEqual(connection.settimeout.call_args_list[1].args[0], 0.4)

    def test_metadata_peer_cancel_does_not_set_global_sync_cancel(self):
        with self.assertRaisesRegex(SYNC.Error, "client disconnected"):
            with self.api.budget(120, peer=lambda: True):
                self.api.check_cancel()
        self.assertFalse(self.api.cancel.is_set())
        self.assertFalse(self.engine.cancel.is_set())
        self.api.check_cancel()

    def test_expired_subprocess_metadata_deadline_never_launches_rclone(self):
        runner = SYNC.Rclone(self.paths, "synthetic-key", "unused")
        with mock.patch.object(SYNC.subprocess, "Popen") as launch:
            with self.assertRaises(SYNC.MetadataTimeout):
                runner.run(["config", "dump"], deadline=time.monotonic() - 1)
        launch.assert_not_called()

    def test_new_preserved_conflict_names_are_reported(self):
        pair = self.add()
        pair["initialized"] = True
        sentinel = ".explorer-sync-access-" + pair["id"]
        (self.local / sentinel).write_bytes(b"sentinel")
        self.api.items["selected"] = [file_metadata(sentinel, b"sentinel", file_id="access")]
        self.paths.atomic_json(self.paths.directory / "pairs" / pair["id"] / "manifest.json", {"local": {sentinel: {}}, "remote": {sentinel: {}}})
        original = self.engine.run
        def create_conflicts(arguments, **options):
            result = original(arguments, **options)
            for index, content in ((1, b"cloud"), (2, b"local")):
                name = f"file.explorer-conflict{index}"
                (self.local / name).write_bytes(content)
                self.api.items["selected"].append(file_metadata(name, content))
            return result
        self.engine.run = create_conflicts
        self.assertTrue(self.worker.run_pair(pair))
        self.assertIn("--conflict-suffix", self.engine.commands[-1])

    def test_status_preserves_failed_operation_even_when_worker_is_running(self):
        response = self.worker.response()
        with mock.patch.object(SYNC, "rpc", return_value=response), mock.patch.object(SYNC, "binary_version", return_value=("rclone", "v1.71.0", True)):
            status = SYNC.public_status(self.paths, "Cannot forget an active worker.", False)
        self.assertFalse(status["ok"])
        self.assertTrue(status["worker_running"])
        self.assertIn("Cannot forget", status["message"])

    def reviewable_pair(self):
        pair = self.add()
        pair["initialized"], pair["state"] = True, "needs-review"
        sentinel = ".explorer-sync-access-" + pair["id"]
        (self.local / sentinel).write_bytes(b"sentinel")
        self.api.items["selected"] = [file_metadata(sentinel, b"sentinel", file_id="access"), file_metadata("removed.txt", b"old")]
        manifest = {sentinel: {}, "removed.txt": {}}
        self.paths.atomic_json(self.paths.directory / "pairs" / pair["id"] / "manifest.json", {"local": manifest, "remote": manifest})
        return pair

    def test_review_tokens_are_readonly_and_normal_resume_cannot_ack_review(self):
        pair = self.reviewable_pair()
        for action in ("resume", "sync-now"):
            with self.subTest(action=action), self.assertRaisesRegex(SYNC.Error, "Review this pair"):
                self.request(action, pair_id=pair["id"])
        response = self.request("review", pair_id=pair["id"])
        review = response["review"]
        self.assertEqual(review["local_deleted"], ["removed.txt"])
        self.assertEqual(review["remote_deleted"], [])
        self.assertFalse(self.engine.commands)
        self.assertTrue(response["pairs"][0]["initialized"])
        self.assertTrue(response["pairs"][0]["can_resume"])
        self.request("resume-reviewed", pair_id=pair["id"], review_token=review["token"])
        self.assertIsNotNone(pair["deletion_review"])
        self.paths.read_state()
        with self.assertRaisesRegex(SYNC.Error, "review expired"):
            self.request("resume-reviewed", pair_id=pair["id"], review_token=review["token"])

    def test_review_rejects_changed_snapshot_or_expired_token(self):
        pair = self.reviewable_pair()
        review = self.request("review", pair_id=pair["id"])["review"]
        self.api.items["selected"].append(file_metadata("changed.txt"))
        with self.assertRaisesRegex(SYNC.Error, "changed or its review expired"):
            self.request("resume-reviewed", pair_id=pair["id"], review_token=review["token"])
        self.assertIsNone(pair["deletion_review"])
        review = self.request("review", pair_id=pair["id"])["review"]
        self.worker.reviews[review["token"]]["expires"] = int(time.time()) - 1
        with self.assertRaisesRegex(SYNC.Error, "review expired"):
            self.request("resume-reviewed", pair_id=pair["id"], review_token=review["token"])
        self.assertFalse(self.engine.commands)

    def test_approved_deletion_snapshot_is_rechecked_and_one_use(self):
        pair = self.reviewable_pair()
        review = self.request("review", pair_id=pair["id"])["review"]
        self.request("resume-reviewed", pair_id=pair["id"], review_token=review["token"])
        self.worker.run_pair(pair)
        command = self.engine.commands[-1]
        self.assertEqual(command[command.index("--max-delete") + 1], "50")
        self.assertNotIn("--force", command)
        self.assertNotIn("--resync", command)
        self.assertNotIn("--resync-mode", command)
        self.assertIsNone(pair["deletion_review"])
        self.assertIsNone(self.paths.read_state()["pairs"][0]["deletion_review"])

    def test_changes_after_approval_cannot_use_raised_deletion_limit(self):
        pair = self.reviewable_pair()
        review = self.request("review", pair_id=pair["id"])["review"]
        self.request("resume-reviewed", pair_id=pair["id"], review_token=review["token"])
        (self.local / "new-edit.txt").write_text("changed after approval")
        with self.assertRaisesRegex(SYNC.Error, "approved folder state changed"):
            self.worker.run_pair(pair)
        self.assertFalse(self.engine.commands)
        self.assertIsNone(pair["deletion_review"])

    def test_review_cannot_override_hard_delete_cap_or_missing_sentinel(self):
        pair = self.reviewable_pair()
        sentinel = ".explorer-sync-access-" + pair["id"]
        many = {sentinel: {}, **{f"removed{index}.txt": {} for index in range(11)}}
        self.paths.atomic_json(self.paths.directory / "pairs" / pair["id"] / "manifest.json", {"local": many, "remote": many})
        with self.assertRaisesRegex(SYNC.Error, "cannot approve a larger deletion"):
            self.request("review", pair_id=pair["id"])
        (self.local / sentinel).unlink()
        with self.assertRaisesRegex(SYNC.Error, "sentinel"):
            self.request("review", pair_id=pair["id"])
        self.assertFalse(self.engine.commands)

    def test_config_identity_is_validated_before_any_network_refresh(self):
        self.paths.config.write_bytes(b"RCLONE_ENCRYPT_V0:\nsynthetic-ciphertext\n")
        self.paths.config.chmod(0o600)
        runner = SYNC.Rclone(self.paths, "s" * 43, "unused")
        token = {"access_token": "synthetic-access", "refresh_token": "synthetic-refresh",
                 "token_type": "Bearer", "expiry": "2099-01-01T00:00:00Z"}
        base = {"type": "drive", "scope": "drive", "client_id": "123.apps.googleusercontent.com",
                "client_secret": "synthetic-secret", "token": json.dumps(token)}
        for replacement in ({"scope": "drive.readonly"}, {"client_id": "456.apps.googleusercontent.com"}, {"type": "local"}, {"extra": "unexpected"}):
            document = {SYNC.REMOTE[:-1]: dict(base, **replacement)}
            with self.subTest(replacement=replacement), mock.patch.object(runner, "run", return_value=json.dumps(document).encode()) as run:
                with self.assertRaises(SYNC.Error):
                    runner.config_document(self.paths.config, "s" * 43, "drive", "123.apps.googleusercontent.com", "synthetic-secret")
                self.assertEqual(run.call_count, 1)
                self.assertEqual(run.call_args.args[0], ["config", "dump"])

    def test_start_forks_once_and_holds_worker_lock_until_stopped(self):
        with mock.patch.object(SYNC, "binary_version", return_value=("unused", "v1.71.0", True)), mock.patch.object(SYNC, "make_worker", return_value=self.worker):
            response = SYNC.start(self.paths, {})
            self.assertTrue(response["worker_running"])
            with self.assertRaisesRegex(SYNC.Error, "already running"):
                SYNC.start(self.paths, {})
        child_pid = None
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
                connection.connect(str(self.paths.socket))
                child_pid, uid, _gid = struct.unpack("3i", connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize("3i")))
                self.assertEqual(uid, os.getuid())
                self.assertNotEqual(child_pid, os.getpid())
                connection.sendall(SYNC.encoded({"version": 1, "action": "status"}))
                self.assertTrue(SYNC.receive_socket(connection)["worker_running"])
            with self.assertRaisesRegex(SYNC.Error, "already running"):
                with self.paths.lock():
                    self.fail("live worker lock was not retained after start returned")
            response = SYNC.rpc(self.paths, {"version": 1, "action": "stop"})
            self.assertFalse(response["worker_running"])
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                waited, _status = os.waitpid(child_pid, os.WNOHANG)
                if waited:
                    child_pid = None
                    break
                time.sleep(0.01)
            self.assertIsNone(child_pid, "detached worker did not stop after RPC")
            with self.paths.lock():
                pass
        finally:
            if child_pid:
                with contextlib.suppress(ProcessLookupError):
                    os.kill(child_pid, 15)
                os.waitpid(child_pid, 0)

    def test_private_socket_roundtrip_and_permissions(self):
        thread = threading.Thread(target=self.worker.serve)
        thread.start()
        deadline = time.monotonic() + 2
        while not self.paths.socket.exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        try:
            self.assertEqual(stat.S_IMODE(self.paths.socket.stat().st_mode), 0o600)
            response = SYNC.rpc(self.paths, {"version": 1, "action": "status"})
            self.assertTrue(response["worker_running"])
            response = SYNC.rpc(self.paths, {"version": 1, "action": "stop"})
            self.assertFalse(response["worker_running"])
        finally:
            self.worker.stopping.set()
            self.worker.wakeup.set()
            thread.join(timeout=3)
        self.assertFalse(self.paths.socket.exists())

    def test_stop_response_waits_for_active_job_to_finish(self):
        self.add()
        started, finished = threading.Event(), threading.Event()
        def active_job(_pair):
            started.set()
            self.engine.cancel.wait(3)
            time.sleep(0.03)
            finished.set()
            return False
        self.worker.run_pair = active_job
        thread = threading.Thread(target=self.worker.serve)
        thread.start()
        self.assertTrue(started.wait(2))
        try:
            response = SYNC.rpc(self.paths, {"version": 1, "action": "stop"})
            self.assertTrue(finished.is_set())
            self.assertFalse(response["worker_running"])
            self.assertIsNone(self.worker.active)
        finally:
            self.worker.stopping.set()
            self.engine.cancel.set()
            self.worker.wakeup.set()
            thread.join(timeout=3)

    def test_busy_preview_status_falls_back_and_abandoned_queue_is_skipped(self):
        entered, release = threading.Event(), threading.Event()
        original_inventory = self.api.inventory
        calls, replies, errors = [], [], []
        self.api.cancel = self.engine.cancel
        def slow_inventory(*args, **options):
            calls.append(True)
            entered.set()
            while not release.wait(0.01):
                self.api.check_cancel()
            return original_inventory(*args, **options)
        self.api.inventory = slow_inventory
        original_rpc = SYNC.rpc
        preview = {"version": 1, "action": "preview", "remote_folder_id": "selected", "local_path": str(self.local)}
        def check_preview():
            try:
                replies.append(original_rpc(self.paths, preview, timeout=3))
            except Exception as error:
                errors.append(type(error).__name__)
        serve = threading.Thread(target=self.worker.serve)
        with self.paths.lock(), mock.patch.object(SYNC, "binary_version", return_value=("unused", "v1.74.2", True)):
            serve.start()
            deadline = time.monotonic() + 2
            while not self.paths.socket.exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            checking = threading.Thread(target=check_preview)
            checking.start()
            self.assertTrue(entered.wait(2))
            try:
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as abandoned:
                    abandoned.connect(str(self.paths.socket))
                    abandoned.sendall(SYNC.encoded(preview))
                def short_status(paths, request, timeout=None):
                    self.assertEqual(timeout, 2)
                    return original_rpc(paths, request, timeout=0.05)
                with mock.patch.object(SYNC, "rpc", side_effect=short_status):
                    status = SYNC.public_status(self.paths)
                self.assertTrue(status["available"])
                self.assertTrue(status["worker_running"])
                self.assertEqual(len(status["pairs"]), 0)
                self.assertFalse(self.engine.commands)
                self.assertFalse(self.engine.cancel.is_set())
                release.set()
                checking.join(timeout=2)
                self.assertFalse(errors)
                self.assertTrue(replies[0]["ok"])
                self.assertTrue(original_rpc(self.paths, {"version": 1, "action": "status"}, timeout=1)["worker_running"])
                self.assertEqual(len(calls), 1, "the disconnected queued preview was executed")
                original_rpc(self.paths, {"version": 1, "action": "stop"}, timeout=2)
            finally:
                release.set()
                self.worker.stopping.set()
                self.engine.cancel.set()
                self.worker.wakeup.set()
                checking.join(timeout=2)
                serve.join(timeout=3)

    def test_disconnected_preview_client_cancels_only_its_metadata(self):
        entered, cancelled = threading.Event(), threading.Event()
        self.api.cancel = self.engine.cancel
        def waiting_inventory(*_args, **_options):
            entered.set()
            try:
                while True:
                    self.api.check_cancel()
                    time.sleep(0.01)
            finally:
                cancelled.set()
        self.api.inventory = waiting_inventory
        serve = threading.Thread(target=self.worker.serve)
        serve.start()
        deadline = time.monotonic() + 2
        while not self.paths.socket.exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
                connection.connect(str(self.paths.socket))
                connection.sendall(SYNC.encoded({"version": 1, "action": "preview", "remote_folder_id": "selected", "local_path": str(self.local)}))
                self.assertTrue(entered.wait(1))
            self.assertTrue(cancelled.wait(1))
            self.assertFalse(self.engine.cancel.is_set())
            self.assertFalse(self.worker.stopping.is_set())
            self.assertTrue(SYNC.rpc(self.paths, {"version": 1, "action": "status"}, timeout=1)["worker_running"])
            SYNC.rpc(self.paths, {"version": 1, "action": "stop"}, timeout=2)
        finally:
            self.worker.stopping.set()
            self.engine.cancel.set()
            self.worker.wakeup.set()
            serve.join(timeout=3)

    def test_runner_limits_output_and_keeps_password_out_of_argv_and_environment(self):
        binary = self.home / "fake-rclone"
        binary.write_text("#!/usr/bin/python3\nimport sys\nsys.stdin.readline()\nsys.stdout.write('x'*1024)\n")
        binary.chmod(0o700)
        runner = SYNC.Rclone(self.paths, "synthetic-secret", str(binary))
        original = SYNC.subprocess.Popen
        calls = []
        def launch(command, **options):
            calls.append((command, options))
            return original(command, **options)
        with mock.patch.object(SYNC.subprocess, "Popen", side_effect=launch), mock.patch.dict(os.environ, {"RCLONE_CONFIG_PASS": "ambient-secret", "RCLONE_CONFIG": "/outside/config"}):
            with self.assertRaisesRegex(SYNC.Error, "output"):
                runner.run(["version"], limit=100)
        command, options = calls[0]
        self.assertNotIn("synthetic-secret", repr(command))
        self.assertFalse(any(key.startswith("RCLONE_") for key in options["env"]))


class ModernRcloneLocalIntegrationTest(unittest.TestCase):
    """Real rclone, local endpoints only; never touches a Drive configuration."""

    def modern_binary(self):
        injected_binary = os.environ.get("EXPLORER_SYNC_TEST_RCLONE")
        if injected_binary:
            with tempfile.TemporaryDirectory(prefix="explorer-version-test-") as version_home, mock.patch.object(SYNC.shutil, "which", return_value=injected_binary):
                binary, version, available = SYNC.binary_version(Path(version_home))
        else:
            binary, version, available = SYNC.binary_version(Path.home())
        if not available:
            self.skipTest("A stable rclone >= 1.71 is not installed; found " + version)
        return binary

    def test_encrypted_synthetic_configuration_roundtrip(self):
        binary = self.modern_binary()
        with tempfile.TemporaryDirectory(prefix="explorer-sync-encryption-test-") as temporary:
            paths = SYNC.Paths(Path(temporary), {})
            paths.prepare()
            token = {"access_token": "synthetic-access-token-no-account", "refresh_token": "synthetic-refresh-token-no-account",
                     "token_type": "Bearer", "expiry": "2099-01-01T00:00:00Z"}
            credential = {"version": 1, "client_id": "123-synthetic.apps.googleusercontent.com", "client_secret": "synthetic-client-secret",
                          "config_password": "s" * 43, "browse_config_password": "b" * 43, "token": token}
            runner = SYNC.Rclone(paths, credential["config_password"], binary)
            runner.install(credential)
            self.assertTrue(SYNC.MOUNT.encrypted_config(paths.config))
            ciphertext = paths.config.read_bytes()
            self.assertNotIn(token["access_token"].encode(), ciphertext)
            self.assertNotIn(token["refresh_token"].encode(), ciphertext)
            self.assertNotIn(credential["config_password"].encode(), ciphertext)
            self.assertEqual(stat.S_IMODE(paths.config.stat().st_mode), 0o600)
            actual = runner.config_document(paths.config, credential["config_password"], "drive", credential["client_id"], credential["client_secret"], refresh=False)
            self.assertEqual(actual, token)
            with self.assertRaises(SYNC.Error):
                runner.config_document(paths.config, credential["config_password"], "drive.readonly", credential["client_id"], credential["client_secret"], refresh=False)
            with self.assertRaises(SYNC.Error):
                runner.config_document(paths.config, credential["config_password"], "drive", "other.apps.googleusercontent.com", credential["client_secret"], refresh=False)

    def test_refreshed_rclone_token_metadata_keeps_strict_credential_validation(self):
        binary = self.modern_binary()
        with tempfile.TemporaryDirectory(prefix="drive-token-") as temporary:
            paths = SYNC.Paths(Path(temporary), {})
            paths.prepare()
            token = {"access_token": "synthetic-access-no-account", "refresh_token": "synthetic-refresh-no-account",
                     "token_type": "Bearer", "expiry": "2099-01-01T00:00:00Z"}
            credential = {"version": 1, "client_id": "123-synthetic.apps.googleusercontent.com",
                          "client_secret": "synthetic-client-secret", "config_password": "s" * 43,
                          "browse_config_password": "b" * 43, "token": token}
            runner = SYNC.Rclone(paths, credential["config_password"], binary)
            cases = [({"expires_in": 0}, True), ({"expires_in": 3600}, True), ({"expires_in": 86400}, True),
                     ({"expires_in": -1}, False), ({"expires_in": 86401}, False),
                     ({"expires_in": True}, False), ({"expires_in": "3600"}, False),
                     ({"expires_in": None}, False), ({"expires_in": 3600.0}, False),
                     ({"expires_in": 3600, "token_uri": "https://example.invalid"}, False),
                     ({"expires_in": 3600, "refresh_token": ""}, False),
                     ({"expires_in": 3600, "expiry": "invalid"}, False)]
            for extra, valid in cases:
                with self.subTest(extra=extra):
                    runner.install(dict(credential, token=dict(token, **extra)))
                    def load():
                        return runner.config_document(paths.config, credential["config_password"], "drive",
                                                      credential["client_id"], credential["client_secret"], refresh=False)
                    if valid:
                        self.assertEqual(load(), token)
                    else:
                        with self.assertRaises(SYNC.Error):
                            load()

    def local_worker(self, home, file_count=30, empty_directory=False):
        binary = self.modern_binary()
        paths = SYNC.Paths(home, {})
        local, remote = home / "mirror", home / "cloud-synthetic"
        local.mkdir(mode=0o700)
        remote.mkdir(mode=0o700)
        if empty_directory:
            (remote / "Empty").mkdir()
        for index in range(file_count):
            (remote / f"file{index}.txt").write_text(f"original {index}")
        class LocalAPI(FakeAPI):
            def listing_items(self, folder_id):
                if folder_id != "selected":
                    return super().listing_items(folder_id)
                items = []
                for path in remote.iterdir():
                    if path.is_dir():
                        items.append(folder_metadata("directory_" + hashlib.sha256(path.name.encode()).hexdigest()[:20], path.name, "selected"))
                    else:
                        items.append(file_metadata(path.name, path.read_bytes(), file_id="file_" + hashlib.sha256(path.name.encode()).hexdigest()[:20]))
                return items
        class LocalEngine(SYNC.Rclone):
            def __init__(self, *args):
                super().__init__(*args)
                self.commands = []

            def run(self, arguments, **options):
                self.commands.append(list(arguments))
                translated = []
                iterator = iter(arguments)
                for argument in iterator:
                    if argument == "--drive-root-folder-id":
                        next(iterator)
                    elif argument in ("--drive-skip-gdocs", "--drive-skip-shortcuts"):
                        continue
                    elif argument.startswith(SYNC.REMOTE):
                        translated.append(str(remote / argument[len(SYNC.REMOTE):]))
                    else:
                        translated.append(argument)
                return super().run(translated, **options)
        engine = LocalEngine(paths, "synthetic-key", binary)
        worker = SYNC.Worker(paths, engine, LocalAPI(), "a" * 64, "v1.74.2")
        selection = {"version": 1, "remote_folder_id": "selected", "local_path": str(local)}
        worker.request(dict(selection, action="preview"))
        worker.request(dict(selection, action="add"))
        return worker, worker.state["pairs"][0], local, remote, engine

    def test_partial_initial_copy_resumes_without_replacing_downloaded_files(self):
        with tempfile.TemporaryDirectory(prefix="explorer-sync-resume-test-") as temporary:
            worker, pair, local, remote, engine = self.local_worker(Path(temporary), file_count=2, empty_directory=True)
            shutil.copy2(remote / "file0.txt", local / "file0.txt")
            (local / "Empty").mkdir()
            before = (local / "file0.txt").stat()
            pair["state"] = "needs-review"
            worker.request({"version": 1, "action": "resume", "pair_id": pair["id"]})
            self.assertEqual(pair["state"], "initializing")
            self.assertFalse(worker.run_pair(pair))
            self.assertTrue(pair["initialized"])
            self.assertEqual((local / "file0.txt").stat().st_ino, before.st_ino)
            self.assertEqual((local / "file0.txt").read_text(), "original 0")
            self.assertEqual((local / "file1.txt").read_text(), "original 1")
            copy = next(command for command in engine.commands if command[0] == "copy")
            self.assertIn("--ignore-existing", copy)

    def test_partial_initial_copy_reserves_space_only_for_missing_files(self):
        with tempfile.TemporaryDirectory(prefix="explorer-sync-space-test-") as temporary:
            worker, pair, local, remote, _engine = self.local_worker(Path(temporary), file_count=2)
            shutil.copy2(remote / "file0.txt", local / "file0.txt")
            free = 64 * 1024 * 1024 + (remote / "file1.txt").stat().st_size
            with mock.patch.object(SYNC.shutil, "disk_usage", return_value=mock.Mock(free=free)):
                self.assertFalse(worker.run_pair(pair))
            self.assertTrue(pair["initialized"])
            self.assertEqual((local / "file1.txt").read_text(), "original 1")

    def test_data_transfers_have_longer_bounded_budget_than_thirty_minutes(self):
        with tempfile.TemporaryDirectory(prefix="explorer-sync-budget-test-") as temporary:
            worker, pair, _local, _remote, engine = self.local_worker(Path(temporary), file_count=2)
            with mock.patch.object(engine, "run", wraps=engine.run) as run:
                self.assertFalse(worker.run_pair(pair))
            budgets = {call.args[0][0]: call.kwargs.get("timeout", 1800) for call in run.call_args_list}
            for command in ("copy", "bisync"):
                with self.subTest(command=command):
                    self.assertGreater(budgets[command], 1800)
                    self.assertLessEqual(budgets[command], 24 * 60 * 60)
            self.assertTrue(pair["initialized"])

    def test_worker_initialization_and_regular_jobs_with_local_remote_adapter(self):
        with tempfile.TemporaryDirectory(prefix="explorer-sync-job-test-") as temporary:
            worker, pair, local, remote, _engine = self.local_worker(Path(temporary))
            self.assertFalse(worker.run_pair(pair))
            self.assertTrue(pair["initialized"])
            self.assertEqual((local / "file0.txt").read_text(), "original 0")
            (local / "file0.txt").write_text("local edit to upload")
            (remote / "file1.txt").write_text("cloud edit to download")
            self.assertFalse(worker.run_pair(pair))
            self.assertEqual((remote / "file0.txt").read_text(), "local edit to upload")
            self.assertEqual((local / "file1.txt").read_text(), "cloud edit to download")
            (local / "file2.txt").write_text("local conflicting version")
            (remote / "file2.txt").write_text("cloud conflicting version longer")
            self.assertTrue(worker.run_pair(pair))
            self.assertTrue(list(local.glob("file2.txt.explorer-conflict*")))
            self.assertTrue(list((worker.paths.directory / "pairs" / pair["id"] / "backups").rglob("file1.txt")))

    def test_small_folder_deletion_requires_exact_review_then_propagates(self):
        with tempfile.TemporaryDirectory(prefix="drv-delete-") as temporary:
            worker, pair, local, remote, engine = self.local_worker(Path(temporary), file_count=1)
            worker.run_pair(pair)
            (local / "file0.txt").unlink()
            with self.assertRaisesRegex(SYNC.Error, "ten percent"):
                worker.run_pair(pair)
            pair["state"] = "needs-review"
            review = worker.request({"version": 1, "action": "review", "pair_id": pair["id"]})["review"]
            self.assertEqual(review["local_deleted"], ["file0.txt"])
            worker.request({"version": 1, "action": "resume-reviewed", "pair_id": pair["id"], "review_token": review["token"]})
            worker.run_pair(pair)
            self.assertFalse((remote / "file0.txt").exists())
            command = engine.commands[-1]
            self.assertEqual(command[command.index("--max-delete") + 1], "50")
            self.assertNotIn("--force", command)
            self.assertNotIn("--resync-mode", command)
            self.assertIsNone(pair["deletion_review"])
            self.assertTrue(list(remote.glob(".explorer-sync-backups-*/**/file0.txt")))
            # Deletion initiated on the remote side receives the same review
            # path and the removed local version is retained in local backups.
            (remote / "new.txt").write_text("new remote file")
            worker.run_pair(pair)
            self.assertTrue((local / "new.txt").exists())
            (remote / "new.txt").unlink()
            with self.assertRaisesRegex(SYNC.Error, "ten percent"):
                worker.run_pair(pair)
            pair["state"] = "needs-review"
            review = worker.request({"version": 1, "action": "review", "pair_id": pair["id"]})["review"]
            self.assertEqual(review["remote_deleted"], ["new.txt"])
            worker.request({"version": 1, "action": "resume-reviewed", "pair_id": pair["id"], "review_token": review["token"]})
            worker.run_pair(pair)
            self.assertFalse((local / "new.txt").exists())
            self.assertTrue(list((worker.paths.directory / "pairs" / pair["id"] / "backups").rglob("new.txt")))

    def test_real_worker_rejects_stale_review_and_supports_conflict_cleanup(self):
        with tempfile.TemporaryDirectory(prefix="drv-conflict-") as temporary:
            worker, pair, local, remote, engine = self.local_worker(Path(temporary), file_count=1)
            worker.run_pair(pair)
            (local / "file0.txt").write_text("local conflict longer")
            (remote / "file0.txt").write_text("remote conflict even longer")
            self.assertTrue(worker.run_pair(pair))
            pair["state"] = "needs-review"
            conflicts = sorted(local.glob("file0.txt.explorer-conflict*"))
            self.assertEqual(len(conflicts), 2)
            conflicts[0].unlink()
            review = worker.request({"version": 1, "action": "review", "pair_id": pair["id"]})["review"]
            (remote / "unreviewed.txt").write_text("changed after review")
            count = len(engine.commands)
            with self.assertRaisesRegex(SYNC.Error, "changed or its review expired"):
                worker.request({"version": 1, "action": "resume-reviewed", "pair_id": pair["id"], "review_token": review["token"]})
            self.assertEqual(len(engine.commands), count)
            review = worker.request({"version": 1, "action": "review", "pair_id": pair["id"]})["review"]
            worker.request({"version": 1, "action": "resume-reviewed", "pair_id": pair["id"], "review_token": review["token"]})
            self.assertFalse(worker.run_pair(pair))
            self.assertFalse((remote / conflicts[0].name).exists())
            self.assertTrue(conflicts[1].exists())
            self.assertTrue((local / "unreviewed.txt").exists())

    def test_two_way_edits_conflicts_deletes_and_backups(self):
        binary = self.modern_binary()
        with tempfile.TemporaryDirectory(prefix="explorer-bisync-local-test-") as temporary:
            home = Path(temporary)
            paths = SYNC.Paths(home, {})
            left, right, work = home / "left", home / "right", home / "work"
            for directory in (left, right, work):
                directory.mkdir(mode=0o700)
            for index in range(30):
                (right / f"file{index}.txt").write_text(f"original {index}")
            (left / "ACCESS").write_text("sentinel")
            (right / "ACCESS").write_text("sentinel")
            runner = SYNC.Rclone(paths, "synthetic-key", binary)
            options = ["--workdir", str(work), "--check-access", "--check-filename", "ACCESS", "--max-delete", "10",
                       "--conflict-resolve", "none", "--conflict-loser", "num", "--resilient", "--recover",
                       "--backup-dir1", str(home / "backup-left"), "--backup-dir2", str(right / ".explorer-sync-backups-test/run1"),
                       "--exclude", "/.explorer-sync-backups-test/**", "--exclude", "/.explorer-sync-backups-test/"]
            runner.run(["bisync", str(left), str(right), "--resync-mode", "path2"] + options, timeout=30)
            self.assertEqual((left / "file0.txt").read_text(), "original 0")
            (left / "file0.txt").write_text("local edit longer")
            (right / "file1.txt").write_text("remote edit longer")
            runner.run(["bisync", str(left), str(right)] + options, timeout=30)
            self.assertEqual((right / "file0.txt").read_text(), "local edit longer")
            self.assertEqual((left / "file1.txt").read_text(), "remote edit longer")
            (left / "file2.txt").write_text("left conflicting edit")
            (right / "file2.txt").write_text("right conflicting version")
            runner.run(["bisync", str(left), str(right)] + options, timeout=30)
            for directory in (left, right):
                copies = [item.read_text() for item in directory.glob("file2.txt*")]
                self.assertIn("left conflicting edit", copies)
                self.assertIn("right conflicting version", copies)
            (left / "file3.txt").unlink()
            runner.run(["bisync", str(left), str(right)] + options, timeout=30)
            self.assertFalse((right / "file3.txt").exists())
            self.assertTrue(any((right / ".explorer-sync-backups-test/run1").rglob("file3.txt")))


if __name__ == "__main__":
    unittest.main()
