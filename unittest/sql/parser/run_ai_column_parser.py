#!/usr/bin/env python3
"""Run native AI-column parser/definition tests using an existing seekdb build.

Requires a completed Linux build, including native and Rust link dependencies.
Does not start a database server or contact an AI endpoint.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--build-dir", default="build_release")
args = parser.parse_args()
repo = Path(__file__).resolve().parents[3]
build = (repo / args.build_dir).resolve()
# Generate from the grammar, retaining timestamps for byte-identical headers to
# avoid rebuilding unrelated native modules.
parser_dir = repo / "src/sql/parser"
before = {path: (path.read_bytes(), path.stat()) for path in parser_dir.iterdir()
          if path.suffix in (".c", ".h")}
try:
    subprocess.run(["bash", "gen_parser.sh"], cwd=parser_dir, check=True)
finally:
    for path, (data, stat) in before.items():
        if path.exists() and path.read_bytes() == data:
            os.utime(path, ns=(stat.st_atime_ns, stat.st_mtime_ns))
objects = "src/sql/parser/CMakeFiles/ob_sql_server_parser_objects.dir"
archive = "src/sql/parser/CMakeFiles/ob_sql_server_parser_static.dir"
for makefile, target in [(objects + "/build.make", objects + "/build"),
                         (archive + "/build.make", "src/sql/parser/libob_sql_server_parser_static.a")]:
    subprocess.run(["make", "-f", makefile, target, "-j2"], cwd=build, check=True)
flags_text = (build / objects / "flags.make").read_text().splitlines()
flags = []
for key in ["CXX_DEFINES", "CXX_INCLUDES", "CXX_FLAGS"]:
    flags += shlex.split(next(line.split(" = ", 1)[1] for line in flags_text
                             if line.startswith(key + " = ")))
link = shlex.split((build / "src/observer/CMakeFiles/seekdb.dir/link.txt").read_text())
with tempfile.TemporaryDirectory(prefix="seekdb-ai-column-parser-") as temp:
    obj = str(Path(temp) / "test.o")
    exe = str(Path(temp) / "test")
    subprocess.run([link[0]] + flags + ["-c", str(Path(__file__).with_name("test_ai_column_parser.cpp")),
                                       "-o", obj], check=True)
    # Use seekdb's configured dependencies, replacing only its executable entry point.
    link = [arg for arg in link if not arg.startswith("CMakeFiles/ob_main.dir/")]
    link[link.index("-o") + 1] = exe
    link.insert(link.index("-o"), obj)
    subprocess.run(link, cwd=build / "src/observer", check=True)
    subprocess.run([exe], cwd=temp, check=True)
