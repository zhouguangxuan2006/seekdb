#!/usr/bin/env python3
# AI-column DDL round-trip/restart regression against an isolated local instance.
import argparse
from contextlib import contextmanager
from pathlib import Path
import socket
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('--base-dir', required=True, help='Dedicated test data directory')
parser.add_argument('--port', type=int, default=2890)
args = parser.parse_args()
repo = Path(__file__).resolve().parents[3]
base = Path(args.base_dir).resolve()
base.mkdir(parents=True, exist_ok=True)
client = ['mysql', '-h127.0.0.1', f'-P{args.port}', '-uroot', '--batch', '--skip-column-names', '--raw']

def sql(query, ok=True):
    result = subprocess.run(client + ['-e', query], text=True, capture_output=True)
    if ok and result.returncode:
        raise AssertionError(result.stderr + '\n' + query)
    return result.stdout.rstrip('\n') if ok else result

@contextmanager
def instance():
    with socket.socket() as sock:
        if sock.connect_ex(('127.0.0.1', args.port)) == 0:
            raise RuntimeError('Test port is already in use')
    with (base / 'ddl-regression.log').open('a') as log:
        process = subprocess.Popen([str(repo / 'build_release/src/observer/seekdb'), '--nodaemon',
            '--base-dir=' + str(base), '--port=' + str(args.port), '--parameter', 'cpu_count=2',
            '--parameter', 'datafile_size=2G', '--parameter', 'log_disk_size=8G'],
            cwd=base, stdout=log, stderr=subprocess.STDOUT)
        try:
            for _ in range(120):
                if process.poll() is not None:
                    raise RuntimeError('Test instance exited before readiness')
                if subprocess.run(client + ['-e', 'SELECT 1'], capture_output=True).returncode == 0:
                    break
                time.sleep(1)
            else:
                raise RuntimeError('Test instance readiness timeout')
            yield
        finally:
            process.terminate()
            process.wait(timeout=60)


with instance():
    sql("DROP DATABASE IF EXISTS ai_column_transaction_test; CREATE DATABASE ai_column_transaction_test")
    sql("CALL DBMS_AI_SERVICE.DROP_AI_MODEL('ai_column_transaction_model')", ok=False)
    sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL('ai_column_transaction_model', '{\"type\":\"completion\",\"model_name\":\"unused\"}')")
    sql("CREATE TABLE ai_column_transaction_test.docs(id BIGINT PRIMARY KEY, body VARCHAR(100), title VARCHAR(100), a JSON AI COLUMN(AI_COMPLETE('ai_column_transaction_model',body)), b TEXT AI COLUMN(AI_COMPLETE('ai_column_transaction_model',concat('prefix: ',body))))")
    tid = sql("SELECT table_id FROM oceanbase.__all_table WHERE database_id=(SELECT database_id FROM oceanbase.__all_database WHERE database_name='ai_column_transaction_test') AND table_name='docs'")
    jobs = f"SELECT count(*) FROM oceanbase.__all_ai_column_task WHERE table_id={tid}"
    out = sql(f"BEGIN; INSERT INTO ai_column_transaction_test.docs(id,body) VALUES(1,'x'),(2,'y'); {jobs}; SELECT count(distinct request_id) FROM oceanbase.__all_ai_column_task WHERE table_id={tid}; ROLLBACK; {jobs}; SELECT count(*) FROM ai_column_transaction_test.docs")
    assert out.splitlines() == ['4','2','0','0'], out
    out = sql(f"BEGIN; INSERT INTO ai_column_transaction_test.docs(id,body) VALUES(1,'x'); UPDATE ai_column_transaction_test.docs SET title='unrelated' WHERE id=1; {jobs}; UPDATE ai_column_transaction_test.docs SET body='X' WHERE id=1; {jobs}; SELECT count(*) FROM oceanbase.__all_ai_column_task WHERE table_id={tid} AND input_payload LIKE '%X%'; UPDATE ai_column_transaction_test.docs SET id=3 WHERE id=1; SELECT count(*) FROM oceanbase.__all_ai_column_task WHERE table_id={tid} AND rowkey_predicate='`id` = 3'; DELETE FROM ai_column_transaction_test.docs; {jobs}; COMMIT")
    assert out.splitlines() == ['2','2','2','2','0'], out
    bad=sql("INSERT INTO ai_column_transaction_test.docs(id,body,a) VALUES(1,'x','{}')",ok=False)
    assert bad.returncode and '1235' in bad.stderr, bad.stderr
    sql("DROP DATABASE ai_column_transaction_test; CALL DBMS_AI_SERVICE.DROP_AI_MODEL('ai_column_transaction_model')")
    print('PASS AI transaction hooks: batch groups, rollback, dependency/PK changes, deletion, assignment protection')
