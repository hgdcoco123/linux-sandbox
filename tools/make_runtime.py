"""Build the minimal static-fixture runtime. Does not collect arbitrary host dependencies."""
import argparse
from pathlib import Path
import shutil
import struct

p = argparse.ArgumentParser()
p.add_argument("--fixture", type=Path, required=True)
p.add_argument("--output", type=Path, required=True)
a = p.parse_args()
binary = a.fixture.read_bytes()
if binary[:4] != b"\x7fELF" or binary[4:6] != b"\x02\x01":
    p.error("expected a trusted little-endian ELF64 static fixture")
phoff = struct.unpack_from("<Q", binary, 32)[0]
phentsize, phnum = struct.unpack_from("<HH", binary, 54)
for n in range(phnum):
    if struct.unpack_from("<I", binary, phoff + n * phentsize)[0] == 3:
        p.error("fixture has PT_INTERP; rebuild sandbox-fixture with static linking")
if a.output.exists():
    p.error("output must not exist")
a.output.mkdir(parents=True, mode=0o755)
for name in ("usr/bin", "input", "output", "work", "tmp", "proc", "dev", ".oldroot"):
    (a.output / name).mkdir(parents=True, mode=0o755)
shutil.copyfile(a.fixture, a.output / "usr/bin/sandbox-fixture")
(a.output / "usr/bin/sandbox-fixture").chmod(0o755)
print(a.output.resolve())
