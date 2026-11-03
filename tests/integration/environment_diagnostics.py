"""A failed environment probe is a supported diagnosis, not isolation success."""
import json
import os
from pathlib import Path
import subprocess
import sys

binary = str(Path(sys.argv[1]).resolve())
namespace = os.stat("/proc/self/ns/mnt").st_ino
uid = os.getuid()
paths = [Path("/proc/sys/kernel/apparmor_restrict_unprivileged_userns"),
         Path("/proc/sys/kernel/unprivileged_userns_clone")]
before = {str(path): path.read_text() for path in paths if path.exists()}
result = subprocess.run([binary, "doctor"], capture_output=True, text=True, timeout=6)
assert result.returncode == 0, result.stderr
report = json.loads(result.stdout)
probe = report["isolation_probe"]
assert isinstance(probe["available"], bool), report
assert report["execution_verified"] is False
assert probe.get("mechanism") == "clone3_parent_mapping", probe
if probe["available"]:
    assert probe["stage"] == "completed"
else:
    assert probe.get("stage") and probe.get("error"), probe
assert os.stat("/proc/self/ns/mnt").st_ino == namespace
assert os.getuid() == uid
assert before == {name: Path(name).read_text() for name in before}
print("environment diagnosis passed; probe available:", probe["available"])
