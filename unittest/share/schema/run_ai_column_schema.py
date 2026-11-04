#!/usr/bin/env python3
"""Run native AI-column schema tests after a complete seekdb build."""
import argparse
from pathlib import Path
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--build-dir", default="build_release")
args = parser.parse_args()
repo = Path(__file__).resolve().parents[3]
build = (repo / args.build_dir).resolve()
lines = (build / "src/sql/CMakeFiles/ob_sql.dir/flags.make").read_text().splitlines()
flags = []
for key in ["CXX_DEFINES", "CXX_INCLUDES", "CXX_FLAGS"]:
    flags += shlex.split(next(line.split(" = ", 1)[1] for line in lines if line.startswith(key + " = ")))
link = shlex.split((build / "src/observer/CMakeFiles/seekdb.dir/link.txt").read_text())
with tempfile.TemporaryDirectory(prefix="seekdb-ai-schema-") as temp:
    obj = str(Path(temp) / "test.o")
    exe = str(Path(temp) / "test")
    subprocess.run([link[0]] + flags + ["-c", str(Path(__file__).with_name("test_ai_column_schema.cpp")), "-o", obj], check=True)
    link = [arg for arg in link if not arg.startswith("CMakeFiles/ob_main.dir/")]
    link[link.index("-o") + 1] = exe
    link.insert(link.index("-o"), obj)
    subprocess.run(link, cwd=build / "src/observer", check=True)
    subprocess.run([exe], cwd=temp, check=True)
