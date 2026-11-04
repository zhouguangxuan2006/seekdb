#!/usr/bin/env python3
"""Run the unchanged public AI-column mysqltests on an isolated instance."""
import argparse
import json
from pathlib import Path
import socket
import subprocess
import time

TESTS = ['ai_column_ddl', 'ai_column_t1_dml', 'ai_column_worker',
         'ai_column_retry_strict', 'ai_column_vector_index']
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--base-dir', required=True, help='Dedicated test instance directory')
parser.add_argument('--port', type=int, default=2890)
parser.add_argument('tests', nargs='*')
args = parser.parse_args()
if any(test not in TESTS for test in args.tests):
    parser.error('Unknown test name')
repo = Path(__file__).resolve().parents[3]
base = Path(args.base_dir).resolve()
base.mkdir(parents=True, exist_ok=True)
results = base / 'mysqltest-results'
results.mkdir(exist_ok=True)
suite = repo / 'tools/deploy/mysql_test/test_suite/ai_column'
client = ['mysql', '-h127.0.0.1', f'-P{args.port}', '-uroot']
with socket.socket() as sock:
    if sock.connect_ex(('127.0.0.1', args.port)) == 0:
        raise SystemExit('Test port is already in use')
with (base / 'mysqltest-server.log').open('w') as log:
    process = subprocess.Popen([str(repo / 'build_release/src/observer/seekdb'),
        '--nodaemon', '--base-dir=' + str(base), '--port=' + str(args.port),
        '--parameter', 'cpu_count=2', '--parameter', 'datafile_size=2G',
        '--parameter', 'log_disk_size=8G'], cwd=base, stdout=log, stderr=subprocess.STDOUT)
    outcomes = {}
    try:
        for _ in range(120):
            if process.poll() is not None:
                raise RuntimeError('Test server exited before readiness')
            if subprocess.run(client + ['-e', 'SELECT 1'], capture_output=True).returncode == 0:
                break
            time.sleep(1)
        else:
            raise RuntimeError('Test server readiness timeout')
        subprocess.run(client + ['-e', 'CREATE DATABASE IF NOT EXISTS test'], check=True)
        # Keep EXPLAIN reproducible when reusing this dedicated test directory.
        subprocess.run(client + ['-e', "CALL DBMS_SCHEDULER.DISABLE('ASYNC_GATHER_STATS_JOB_PROC')"], check=True)
        for test in args.tests or TESTS:
            try:
                result = subprocess.run([str(repo / 'deps/3rd/u01/obclient/bin/mysqltest'),
                    '--host=127.0.0.1', '--port=' + str(args.port), '--user=root',
                    '--database=test', '--default-character-set=utf8mb4',
                    '--test-file=' + str(suite / 't' / (test + '.test')),
                    '--result-file=' + str(suite / 'r/mysql' / (test + '.result')),
                    '--logdir=' + str(results)], cwd=repo / 'tools/deploy',
                    capture_output=True, text=True, timeout=300)
                outcomes[test] = result.returncode
                (results / (test + '.output')).write_text(result.stdout + result.stderr)
                print(test, 'PASS' if result.returncode == 0 else 'FAIL', flush=True)
                if result.returncode:
                    print(result.stdout + result.stderr, flush=True)
                (results / 'status.json').write_text(json.dumps(outcomes, indent=2))
            finally:
                for port in range(19121, 19125):
                    subprocess.run(['python3', str(suite / 'include/ai_column_mock.py'),
                                    'stop', str(port)], capture_output=True)
    finally:
        process.terminate()
        process.wait(timeout=60)
raise SystemExit(int(any(outcomes.values())))
