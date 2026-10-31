#!/usr/bin/python3
"""Plan, stage, install, inspect, or remove the fixed-path rootless distribution.

Only install/uninstall --apply change the Linux system. They require an already
privileged administrator process; this tool never invokes sudo or alters sysctl.
"""
import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile

SOURCE = Path(__file__).resolve().parents[1]
PREFIX = Path("/opt/linux-sandbox")
PROFILE = Path("/etc/apparmor.d/linux-sandbox-installed-launcher")
PROFILE_NAME = "linux-sandbox-installed-launcher"
MANIFEST = "INSTALL_MANIFEST.json"


def sha(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def inputs(source, build):
    items = [(build/"sandbox", "bin/sandbox", 0o755),
             (build/"sandbox-launcher", "bin/sandbox-launcher", 0o755),
             (source/"tools/sandbox-run", "bin/sandbox-run", 0o755)]
    for name in ("sdk", "tests", "profiles", "docs", "tools"):
        for path in sorted((source/name).rglob("*")):
            if not path.is_file() or "__pycache__" in path.parts or path.suffix == ".pyc":
                continue
            if name == "tools" and path.name not in ("delegated_run.py", "make_runtime.py", "deploy.py"):
                continue
            items.append((path, "share/linux-sandbox/"+path.relative_to(source).as_posix(), 0o644))
    items.append((source/"packaging/apparmor"/PROFILE.name,
                  "share/linux-sandbox/packaging/apparmor/"+PROFILE.name,0o644))
    for path, _, _ in items:
        if path.is_symlink() or not path.is_file():
            raise RuntimeError("missing or symlink package input: " + str(path))
    # Linux execve includes CR in the interpreter path for a CRLF shebang.
    with (source/"tools/sandbox-run").open("rb") as stream:
        if stream.readline(256) != b"#!/usr/bin/python3\n":
            raise RuntimeError("tools/sandbox-run must start with #!/usr/bin/python3 and an LF newline; convert CRLF to LF")
    for name in ("sandbox", "sandbox-launcher"):
        with (build/name).open("rb") as stream:
            if stream.read(4) != b"\x7fELF":
                raise RuntimeError(name + " is not a Linux ELF executable")
    return items


def plan(source, build):
    items = inputs(source, build)
    profile = (source/"packaging/apparmor"/PROFILE.name).read_text()
    return {"prefix": str(PREFIX), "profile": str(PROFILE), "profile_text": profile,
            "profile_kind": "Ubuntu compatibility authorization, not complete confinement",
            "requires_administrator_once": True, "runtime_uid": "calling ordinary user",
            "global_sysctl_changes": False, "setuid_or_file_capabilities": False,
            "system_services_installed": False,
            "files": [{"source": str(path), "destination": str(PREFIX/rel),
                       "mode": oct(mode), "sha256": sha(path)} for path, rel, mode in items]}


def populate(source, build, destination):
    items = inputs(source, build)
    destination.mkdir(mode=0o755)
    files = []
    for path, rel, mode in items:
        target = destination/rel
        target.parent.mkdir(parents=True, exist_ok=True, mode=0o755)
        # Read sources through an explicit no-follow descriptor; never copy mode bits.
        fd = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
        with os.fdopen(fd, "rb") as src, target.open("xb") as dst:
            if not stat.S_ISREG(os.fstat(src.fileno()).st_mode):
                raise RuntimeError("nonregular package input")
            shutil.copyfileobj(src, dst)
            dst.flush()
            os.fsync(dst.fileno())
        target.chmod(mode)
        files.append({"path": rel, "sha256": sha(target), "mode": mode})
    for path in destination.rglob("*"):
        if path.is_dir():
            path.chmod(0o755)
    manifest = {"schema_version": 1, "package": "linux-sandbox", "prefix": str(PREFIX),
                "profile": str(PROFILE), "files": files}
    (destination/MANIFEST).write_text(json.dumps(manifest, indent=2))
    (destination/MANIFEST).chmod(0o644)
    return manifest


def protected_stat(info):
    return not stat.S_ISLNK(info.st_mode) and info.st_uid == 0 and not info.st_mode & 0o022


def protected(path):
    for item in (path, *path.parents):
        info = item.lstat()
        if not protected_stat(info):
            raise RuntimeError("unprotected system path: " + str(item))


def inspect_tree(prefix, root_owned=True):
    if prefix.is_symlink():
        raise RuntimeError("installation is a symlink")
    manifest = json.loads((prefix/MANIFEST).read_text())
    if manifest.get("package") != "linux-sandbox" or manifest.get("prefix") != str(PREFIX):
        raise RuntimeError("unrecognized installation manifest")
    expected = {MANIFEST}
    for entry in manifest["files"]:
        rel = Path(entry["path"])
        if rel.is_absolute() or ".." in rel.parts:
            raise RuntimeError("invalid manifest path")
        path = prefix/rel
        if path.is_symlink() or not path.is_file() or sha(path) != entry["sha256"]:
            raise RuntimeError("installed file missing, changed or symlink: " + str(path))
        if os.name != "nt" and stat.S_IMODE(path.stat().st_mode) != entry["mode"]:
            raise RuntimeError("installed mode changed: " + str(path))
        expected.add(rel.as_posix())
    for path in prefix.rglob("*"):
        if path.is_symlink():
            raise RuntimeError("unexpected installation symlink")
        if root_owned:
            protected(path)
        if path.is_file() and path.relative_to(prefix).as_posix() not in expected:
            raise RuntimeError("unexpected file; refusing removal: " + str(path))
    return manifest


def remove_tree(prefix, manifest):
    # Only individual manifest files, followed by empty directories; no recursive rm.
    for entry in manifest["files"]:
        (prefix/entry["path"]).unlink()
    (prefix/MANIFEST).unlink()
    directories = sorted((p for p in prefix.rglob("*") if p.is_dir()),
                         key=lambda p: len(p.parts), reverse=True)
    for path in directories:
        path.rmdir()
    prefix.rmdir()


def require_admin():
    if sys.platform != "linux" or os.geteuid() != 0:
        raise RuntimeError("system changes require explicit administrator execution on Linux")


@contextlib.contextmanager
def deployment_lock():
    import fcntl
    lock_dir = Path("/run/linux-sandbox-deployment")
    protected(lock_dir.parent)
    lock_dir.mkdir(mode=0o700, exist_ok=True)
    protected(lock_dir)
    lock = lock_dir/"lock"
    fd = os.open(lock, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_nlink != 1:
            raise RuntimeError("unsafe deployment lock")
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield
    finally:
        os.close(fd)


def parser_path():
    for path in (Path("/usr/sbin/apparmor_parser"), Path("/sbin/apparmor_parser")):
        if path.is_file():
            protected(path.resolve())
            return str(path)
    raise RuntimeError("AppArmor parser is required; automatic installation is not performed")


def active_profile():
    path = Path("/sys/kernel/security/apparmor/profiles")
    return path.exists() and any(line.startswith(PROFILE_NAME+" ") for line in path.read_text().splitlines())


def install(source, build):
    require_admin()
    with deployment_lock():
        protected(PREFIX.parent)
        protected(PROFILE.parent)
        if PREFIX.exists() or PREFIX.is_symlink() or PROFILE.exists() or PROFILE.is_symlink() or active_profile():
            raise RuntimeError("installation/profile already exists; use status then explicit uninstall")
        parser = parser_path()
        profile_text = (source/"packaging/apparmor"/PROFILE.name).read_text()
        # A protected stage is on the same filesystem as the final prefix.
        container = Path(tempfile.mkdtemp(prefix=".linux-sandbox-install-", dir=PREFIX.parent))
        staged = container/"payload"
        published = False
        profile_created = False
        loaded = False
        manifest = None
        try:
            manifest = populate(source, build, staged)
            candidate = container/"profile"
            candidate.write_text(profile_text)
            candidate.chmod(0o644)
            subprocess.run([parser, "-Q", "-K", str(candidate)], check=True, timeout=15)
            policy_copy = staged/"INSTALLED_APPARMOR"
            policy_copy.write_text(profile_text)
            policy_copy.chmod(0o644)
            manifest["files"].append({"path": "INSTALLED_APPARMOR", "sha256": sha(policy_copy), "mode": 0o644})
            (staged/MANIFEST).write_text(json.dumps(manifest, indent=2))
            inspect_tree(staged)
            staged.rename(PREFIX)
            published = True
            with PROFILE.open("x") as stream:
                profile_created = True
                stream.write(profile_text)
                stream.flush()
                os.fsync(stream.fileno())
            PROFILE.chmod(0o644)
            subprocess.run([parser, "-a", "-K", str(PROFILE)], check=True, timeout=15)
            loaded = True
            inspect_tree(PREFIX)
            return {"installed": str(PREFIX), "policy_loaded": True,
                    "runtime_verified": False, "next": "run installed doctor as ordinary user"}
        except Exception:
            # If loading succeeded, never remove its executable before successful unload.
            if loaded:
                subprocess.run([parser, "-R", str(PROFILE)], check=True, timeout=15)
            elif profile_created and active_profile():
                subprocess.run([parser, "-R", str(PROFILE)], check=True, timeout=15)
            if profile_created:
                PROFILE.unlink()
            if manifest is not None:
                location = PREFIX if published else staged
                if location.exists():
                    remove_tree(location, manifest)
            raise
        finally:
            candidate = container/"profile"
            if candidate.exists():
                candidate.unlink()
            # Nonempty staging after a partial copy is retained for diagnosis.
            try:
                container.rmdir()
            except OSError:
                pass


def active_sandboxes():
    active = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            executable = (entry/"exe").readlink()
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            continue
        if executable in (PREFIX/"bin/sandbox", PREFIX/"bin/sandbox-launcher"):
            active.append(entry.name)
    return active


def upgrade(source, build):
    """Update an idle verified payload, leaving the exact installed policy untouched."""
    require_admin()
    with deployment_lock():
        protected(PREFIX)
        old_manifest=inspect_tree(PREFIX)
        protected(PROFILE)
        policy=(PREFIX/"INSTALLED_APPARMOR").read_bytes()
        if PROFILE.read_bytes()!=policy or (source/"packaging/apparmor"/PROFILE.name).read_text().encode("utf-8")!=policy:
            raise RuntimeError("policy differs; payload-only upgrade will not alter authorization")
        if active_sandboxes():
            raise RuntimeError("sandbox is active; stop submissions and close sessions before upgrade")
        container=Path(tempfile.mkdtemp(prefix=".linux-sandbox-upgrade-",dir=PREFIX.parent))
        staged=container/"payload";previous=container/"previous"
        renamed=False;published=False
        try:
            manifest=populate(source,build,staged)
            copy=staged/"INSTALLED_APPARMOR";copy.write_bytes(policy);copy.chmod(0o644)
            manifest["files"].append({"path":"INSTALLED_APPARMOR","sha256":sha(copy),"mode":0o644})
            (staged/MANIFEST).write_text(json.dumps(manifest,indent=2))
            inspect_tree(staged)
            PREFIX.rename(previous);renamed=True
            staged.rename(PREFIX);published=True
            inspect_tree(PREFIX)
        except Exception:
            if published:PREFIX.rename(container/"failed")
            if renamed:previous.rename(PREFIX)
            raise
        remove_tree(previous,old_manifest)
        container.rmdir()
        return {"upgraded":str(PREFIX),"policy_changed":False,"policy_reloaded":False,
                "user_data_removed":False,"runtime_verified":False}


def uninstall():
    require_admin()
    with deployment_lock():
        protected(PREFIX)
        manifest = inspect_tree(PREFIX)
        protected(PROFILE)
        if PROFILE.read_bytes() != (PREFIX/"INSTALLED_APPARMOR").read_bytes():
            raise RuntimeError("AppArmor policy changed; refusing to remove it")
        if active_sandboxes():
            raise RuntimeError("sandbox is active; close sessions before uninstall")
        subprocess.run([parser_path(), "-R", str(PROFILE)], check=True, timeout=15)
        PROFILE.unlink()
        remove_tree(PREFIX, manifest)
        return {"uninstalled": True, "user_data_removed": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("plan", "stage", "install", "upgrade", "status", "uninstall"))
    parser.add_argument("--build-dir", type=Path, default=SOURCE/"build")
    parser.add_argument("--destination", type=Path)
    parser.add_argument("--apply", action="store_true")
    args = parser.parse_args()
    if args.action == "plan":
        result = plan(SOURCE, args.build_dir.resolve())
    elif args.action == "stage":
        if args.destination is None:
            parser.error("stage requires a new --destination directory")
        result = populate(SOURCE, args.build_dir.resolve(), args.destination.absolute())
    elif args.action == "status":
        result = {"installed": PREFIX.is_dir(), "profile_present": PROFILE.exists()}
        if PREFIX.is_dir():
            inspect_tree(PREFIX)
            result["integrity"] = "passed"
            protected(PROFILE)
            if PROFILE.read_bytes() != (PREFIX/"INSTALLED_APPARMOR").read_bytes():
                raise RuntimeError("installed AppArmor policy does not match managed copy")
        try:
            result["profile_loaded"] = active_profile()
        except PermissionError:
            result["profile_loaded"] = None
    else:
        if not args.apply:
            parser.error("system changes require --apply; review plan before administrator execution")
        if args.action=="install":result=install(SOURCE,args.build_dir.resolve())
        elif args.action=="upgrade":result=upgrade(SOURCE,args.build_dir.resolve())
        else:result=uninstall()
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        raise SystemExit("deploy: " + str(error))
