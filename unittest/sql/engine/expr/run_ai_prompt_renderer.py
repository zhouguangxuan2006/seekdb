#!/usr/bin/env python3
# Copyright (c) 2026 OceanBase.
# SPDX-License-Identifier: Apache-2.0
"""Compile and test the actual C++ renderer using an initialized CMake build.

Run after building the oblib target. Does not require a running database.
Usage: python3 unittest/sql/engine/expr/run_ai_prompt_renderer.py [build_directory]
"""
import json
from pathlib import Path
import random
import shlex
import subprocess
import sys

repo = Path(__file__).resolve().parents[4]
build = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else repo / 'build_release'
out = build / 'ai_prompt_tests'
out.mkdir(exist_ok=True)
entries = json.loads((build / 'compile_commands.json').read_text())
entry = next(e for e in entries if 'ob_sql_engine_expr' in e['file'])
args = shlex.split(entry['command'])
flags = []
i = 1
while i < len(args):
    if args[i] == '-o':
        i += 2
        continue
    if args[i] != '-c' and args[i] != entry['file'] and not args[i].startswith(('-O', '-g')):
        flags.append(args[i])
    i += 1
for source, name in [(repo / 'src/sql/engine/expr/ob_expr_ai/ob_ai_func_utils.cpp', 'utils.o'),
                     (Path(__file__).with_name('test_ai_prompt_renderer.cpp'), 'main.o')]:
    subprocess.run([args[0], *flags, '-O0', '-g0', '-ffunction-sections', '-fdata-sections',
                    '-c', str(source), '-o', str(out / name)], cwd=entry['directory'], check=True)
link_dir = build / 'src/observer'
link = shlex.split((link_dir / 'CMakeFiles/seekdb.dir/link.txt').read_text())
libs = []
for arg in link[1:]:
    if arg.endswith(('.a', '.so')):
        path = (Path(arg) if arg.startswith('/') else link_dir / arg).resolve()
        # Core utility/allocator libraries and external dependencies only.
        if not path.is_relative_to(build / 'src') or path.is_relative_to(build / 'src/oblib'):
            if path.exists():
                libs.append(str(path))
    elif arg.startswith(('-l', '-L')):
        libs.append(arg)
subprocess.run([args[0], '-fuse-ld=lld', '-Wl,--gc-sections', '-o', str(out / 'prompt_renderer'),
                str(out / 'main.o'), str(out / 'utils.o'), '-Wl,--start-group', *libs,
                '-Wl,--end-group', '-lpthread', '-ldl', '-lrt'], check=True)

cases = []
def add(name, template, arguments, expected):
    cases.append((name, {'template': template, 'args': arguments}, expected))

add('repeated', '{0}' * 40, ['x' * 1000], 'x' * 40000)
add('nested', 'outer[{0}] {1}', [{'template': '{1}/{0}/{1}', 'args': ['a', 'b']}, 'end'], 'outer[b/a/b] end')
add('json-array', '{0}', [[1, True, None]], '[1, true, null]')
add('json-object', '{0}', [{'key': 2}], '{"key": 2}')
add('json-number', '{0}', [42], '42')
add('json-null', '{0}', [None], 'null')
add('utf8-escapes', '\u4e16\u754c[{0}]', ['"\\\n\t\x00\U0001f30a'], '\u4e16\u754c["\\\n\t\x00\U0001f30a]')
add('longtext', 't' * 80000 + '{0}/{0}', ['v' * 80000], 't' * 80000 + 'v' * 80000 + '/' + 'v' * 80000)
add('not-reparsed', '{0}/{1}', ['{1}', 'end'], '{1}/end')
add('literal-braces', '{x} {} { unclosed {0}', ['ok'], '{x} {} { unclosed ok')
add('leading-zero', '{01}/{0}/{1}', ['a', 'b'], 'b/a/b')
add('empty-arg', '{0}', [''], '')
for name, template, arguments in [('invalid-index', '{1}', ['a']),
                                  ('huge-index', '{18446744073709551616}', ['a']),
                                  ('empty-template', '', []), ('no-args', '{0}', [])]:
    add(name, template, arguments, None)
rng = random.Random(943)
for i in range(300):
    arguments = [''.join(rng.choice('abc{}\\"\n\x00\u4e16\u754c') for _ in range(rng.randrange(100)))
                 for _ in range(rng.randrange(1, 9))]
    slots = [rng.randrange(len(arguments)) for _ in range(rng.randrange(1, 60))]
    add('random-' + str(i), 'prefix|' + '|'.join('{%d}' % s for s in slots) + '|suffix',
        arguments, 'prefix|' + '|'.join(arguments[s] for s in slots) + '|suffix')
result = subprocess.run([str(out / 'prompt_renderer')],
                        input='\n'.join(json.dumps(c[1]) for c in cases) + '\n',
                        text=True, capture_output=True, timeout=60, check=True)
lines = result.stdout.splitlines()
assert len(lines) == len(cases), (len(lines), result.stderr)
for (name, _, expected), line in zip(cases, lines):
    code, actual = line.split('\t', 1)
    if expected is None:
        assert code != '0', (name, 'expected rejection')
    else:
        assert code == '0', (name, code)
        assert bytes.fromhex(actual).decode() == expected, name
print('PASS: %d real C++ renderer cases' % len(cases))
