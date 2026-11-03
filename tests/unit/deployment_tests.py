"""Deployment policy, payload integrity, and install failure rollback; no host edits."""
import contextlib
import importlib.util
import json
import os
import runpy
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location("deploy",ROOT/"tools/deploy.py")
deploy=importlib.util.module_from_spec(spec)
spec.loader.exec_module(deploy)


class DeploymentTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)
        self.source=self.root/"source"
        self.build=self.root/"build"
        self.build.mkdir()
        for name in ("sandbox","sandbox-launcher"):
            (self.build/name).write_bytes(b"\x7fELFtest")
        for name in ("tools","sdk","tests","profiles","docs","packaging/apparmor"):
            (self.source/name).mkdir(parents=True)
        for name in ("sandbox-run","delegated_run.py","make_runtime.py","deploy.py"):
            (self.source/"tools"/name).write_text("# fixture\n")
        (self.source/"tools/sandbox-run").write_bytes(b"#!/usr/bin/python3\n# fixture\n")
        (self.source/"sdk/client.py").write_text("# sdk\n")
        (self.source/"packaging/apparmor"/deploy.PROFILE.name).write_text(
            (ROOT/"packaging/apparmor"/deploy.PROFILE.name).read_text())

    def test_plan_has_no_system_mutations(self):
        with patch.object(deploy.subprocess,"run") as run:
            result=deploy.plan(self.source,self.build)
        run.assert_not_called()
        self.assertFalse(result["global_sysctl_changes"])
        self.assertFalse(result["setuid_or_file_capabilities"])
        self.assertFalse(result["system_services_installed"])
        self.assertTrue(result["requires_administrator_once"])
        self.assertIn("/opt/linux-sandbox/bin/sandbox-launcher",result["profile_text"])
        self.assertNotIn("/home/",result["profile_text"])
        self.assertTrue(all(int(f["mode"],8) in (0o644,0o755) for f in result["files"]))

    def test_stage_integrity_and_removal(self):
        dest=self.root/"stage"
        manifest=deploy.populate(self.source,self.build,dest)
        deploy.inspect_tree(dest,root_owned=False)
        self.assertTrue((dest/"bin/sandbox-run").exists())
        deploy.remove_tree(dest,manifest)
        self.assertFalse(dest.exists())

    def test_tampered_payload_is_rejected(self):
        dest=self.root/"stage";deploy.populate(self.source,self.build,dest)
        (dest/"bin/sandbox").write_bytes(b"changed")
        with self.assertRaises(RuntimeError):deploy.inspect_tree(dest,root_owned=False)

    def test_unexpected_file_prevents_removal(self):
        dest=self.root/"stage";deploy.populate(self.source,self.build,dest)
        (dest/"unknown").write_text("retain me")
        with self.assertRaises(RuntimeError):deploy.inspect_tree(dest,root_owned=False)
        self.assertTrue((dest/"unknown").exists())

    def test_existing_stage_not_overwritten(self):
        dest=self.root/"stage";dest.mkdir();(dest/"data").write_text("retain")
        with self.assertRaises(FileExistsError):deploy.populate(self.source,self.build,dest)
        self.assertEqual((dest/"data").read_text(),"retain")

    def test_entry_rejects_crlf_shebang_before_staging(self):
        (self.source/"tools/sandbox-run").write_bytes(b"#!/usr/bin/python3\r\nprint('bad')\r\n")
        with self.assertRaisesRegex(RuntimeError, "CRLF to LF"):
            deploy.plan(self.source, self.build)
        dest = self.root/"stage"
        with self.assertRaisesRegex(RuntimeError, "CRLF to LF"):
            deploy.populate(self.source, self.build, dest)
        self.assertFalse(dest.exists())

    def test_source_entry_has_linux_shebang(self):
        with (ROOT/"tools/sandbox-run").open("rb") as entry:
            self.assertEqual(entry.readline(), b"#!/usr/bin/python3\n")

    def test_non_elf_rejected(self):
        (self.build/"sandbox").write_text("not ELF")
        with self.assertRaises(RuntimeError):deploy.plan(self.source,self.build)

    def test_protected_path_rules(self):
        self.assertTrue(deploy.protected_stat(SimpleNamespace(st_mode=stat.S_IFREG|0o755,st_uid=0)))
        for mode,uid in ((stat.S_IFREG|0o777,0),(stat.S_IFREG|0o755,1000),(stat.S_IFLNK|0o777,0)):
            self.assertFalse(deploy.protected_stat(SimpleNamespace(st_mode=mode,st_uid=uid)))

    def test_untrusted_manifest_path_rejected(self):
        dest=self.root/"stage";deploy.populate(self.source,self.build,dest)
        manifest=json.loads((dest/deploy.MANIFEST).read_text())
        manifest["files"][0]["path"]="../outside"
        (dest/deploy.MANIFEST).write_text(json.dumps(manifest))
        with self.assertRaises(RuntimeError):deploy.inspect_tree(dest,root_owned=False)

    def simulate_install(self,fail_load=False,remove=False,active=False,upgrade=False,change_policy=False):
        prefix=self.root/"opt/linux-sandbox";prefix.parent.mkdir()
        profile=self.root/"etc/profile";profile.parent.mkdir()
        calls=[]
        def parser(argv,**kwargs):
            calls.append(argv)
            if fail_load and "-a" in argv:raise subprocess.CalledProcessError(1,argv)
        with contextlib.ExitStack() as stack:
            for name,value in (("PREFIX",prefix),("PROFILE",profile)):
                stack.enter_context(patch.object(deploy,name,value))
            # The fixture policy filename follows the patched PROFILE filename.
            (self.source/"packaging/apparmor/profile").write_text("fixture userns policy")
            stack.enter_context(patch.object(deploy,"require_admin"))
            stack.enter_context(patch.object(deploy,"protected"))
            stack.enter_context(patch.object(deploy,"deployment_lock",return_value=contextlib.nullcontext()))
            stack.enter_context(patch.object(deploy,"parser_path",return_value="test-parser"))
            stack.enter_context(patch.object(deploy,"active_profile",return_value=False))
            stack.enter_context(patch.object(deploy.subprocess,"run",side_effect=parser))
            if fail_load:
                with self.assertRaises(subprocess.CalledProcessError):deploy.install(self.source,self.build)
                self.assertFalse(prefix.exists());self.assertFalse(profile.exists())
            else:
                result=deploy.install(self.source,self.build)
                self.assertFalse(result["runtime_verified"])
                deploy.inspect_tree(prefix,root_owned=False)
                if upgrade:
                    old_profile=profile.read_bytes()
                    (self.build/"sandbox").write_bytes(b"\x7fELFupdated")
                    if change_policy:(self.source/"packaging/apparmor/profile").write_text("changed policy")
                    with patch.object(deploy,"active_sandboxes",return_value=[]):
                        if change_policy:
                            with self.assertRaises(RuntimeError):deploy.upgrade(self.source,self.build)
                            self.assertEqual((prefix/"bin/sandbox").read_bytes(),b"\x7fELFtest")
                        else:
                            deploy.upgrade(self.source,self.build)
                            self.assertEqual((prefix/"bin/sandbox").read_bytes(),b"\x7fELFupdated")
                    self.assertEqual(profile.read_bytes(),old_profile)
                if remove:
                    with patch.object(deploy,"active_sandboxes",return_value=["123"] if active else []):
                        if active:
                            with self.assertRaises(RuntimeError):deploy.uninstall()
                            self.assertTrue(prefix.exists());self.assertTrue(profile.exists())
                        else:
                            deploy.uninstall()
                            self.assertFalse(prefix.exists());self.assertFalse(profile.exists())
        return calls

    def test_successful_install_requires_parser_validation_before_load(self):
        calls=self.simulate_install()
        self.assertIn("-Q",calls[0]);self.assertIn("-a",calls[1])

    def test_profile_load_failure_rolls_back(self):
        self.simulate_install(fail_load=True)

    def test_upgrade_preserves_policy_without_parser_calls(self):
        self.assertEqual(len(self.simulate_install(upgrade=True)),2)

    def test_upgrade_rejects_policy_changes(self):
        self.simulate_install(upgrade=True,change_policy=True)

    def test_managed_uninstall_removes_only_installed_payload(self):
        calls=self.simulate_install(remove=True)
        self.assertIn("-R",calls[-1])

    def test_active_sandbox_prevents_uninstall(self):
        calls=self.simulate_install(remove=True,active=True)
        self.assertFalse(any("-R" in call for call in calls))

    def test_user_entry_keeps_paths_as_single_arguments(self):
        entry=runpy.run_path(str(ROOT/"tools/sandbox-run"),run_name="test_entry")
        config=self.root/"job with spaces.json";config.write_text(json.dumps({"mode":"isolated"}))
        args=SimpleNamespace(config=str(config),test_runtime=None,state_root=str(self.root/"state with spaces"))
        argv=entry["command"](args)
        self.assertEqual(argv[0],"/usr/bin/systemd-run")
        self.assertIn("--user",argv);self.assertIn("Delegate=yes",argv)
        self.assertEqual(argv[argv.index("--config")+1],str(config.resolve()))
        self.assertNotIn("sudo",argv)

    def test_user_entry_rejects_process_mode(self):
        entry=runpy.run_path(str(ROOT/"tools/sandbox-run"),run_name="test_entry")
        config=self.root/"process.json";config.write_text(json.dumps({"mode":"process"}))
        args=SimpleNamespace(config=str(config),test_runtime=None,state_root=None)
        with self.assertRaises(ValueError):entry["command"](args)


if __name__=="__main__":unittest.main()
