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

model = 'ai_column_schema_regression_model'
database = 'ai_column_schema_regression'
with instance():
    sql(f'DROP DATABASE IF EXISTS {database}; CREATE DATABASE {database}')
    sql(f"CALL DBMS_AI_SERVICE.DROP_AI_MODEL('{model}')", ok=False)
    sql(f'''CALL DBMS_AI_SERVICE.CREATE_AI_MODEL('{model}', '{{"type":"completion","model_name":"mock-complete"}}')''')
    sql(f'''CREATE TABLE {database}.docs (
        id BIGINT PRIMARY KEY,
        tags JSON AI COLUMN (AI_COMPLETE('{model}', body)) WITH (sync_mode='async',max_retries=2),
        body VARCHAR(200))''')
    sql(f'''ALTER TABLE {database}.docs ADD COLUMN label VARCHAR(200)
        AI COLUMN (AI_COMPLETE('{model}', body)) WITH (max_retries=0,sync_mode='async')''')
    table_id = int(sql(f"SELECT table_id FROM oceanbase.__all_table WHERE database_id=(SELECT database_id FROM oceanbase.__all_database WHERE database_name='{database}') AND table_name='docs'"))
    assert sql(f'SELECT count(*) FROM oceanbase.__all_ai_column WHERE table_id={table_id}') == '2'
    shown = sql(f'SHOW AI COLUMNS FROM {database}.docs')
    assert 'tags\t' in shown and 'label\t' in shown and 'valid JSON value' in shown, shown
    ddl = sql(f'SHOW CREATE TABLE {database}.docs').split('\t', 1)[1]
    assert ddl.count('AI COLUMN') == 2 and '__ai_' not in ddl, ddl
    copied = ddl.replace('CREATE TABLE `docs`', f'CREATE TABLE {database}.docs_copy', 1)
    sql(copied)
    assert sql(f"SELECT count(*) FROM oceanbase.__all_ai_column WHERE table_id=(SELECT table_id FROM oceanbase.__all_table WHERE database_id=(SELECT database_id FROM oceanbase.__all_database WHERE database_name='{database}') AND table_name='docs_copy')") == '2'
    invalid = sql(f"CREATE TABLE {database}.bad (body TEXT, first_ai TEXT AI COLUMN (AI_COMPLETE('{model}',body)), second_ai TEXT AI COLUMN (AI_COMPLETE('{model}',first_ai)))", ok=False)
    assert invalid.returncode and '1235' in invalid.stderr, invalid.stderr
    wrong_model = sql(f"CREATE TABLE {database}.bad_model(body TEXT, embedding VECTOR(4) AI COLUMN (AI_EMBED('{model}',body)))", ok=False)
    assert wrong_model.returncode and '1235' in wrong_model.stderr, wrong_model.stderr
    missing_model = sql(f"CREATE TABLE {database}.missing_model(body TEXT, label TEXT AI COLUMN (AI_COMPLETE('ai_column_missing_model',body)))", ok=False)
    assert missing_model.returncode, 'missing model must fail during DDL'
    sql(f'CREATE TABLE {database}.ordinary(id INT PRIMARY KEY, value INT); INSERT INTO {database}.ordinary VALUES(1,2); UPDATE {database}.ordinary SET value=3 WHERE id=1')
    assert sql(f'SELECT value FROM {database}.ordinary') == '3'
    print('PASS AI DDL: forward dependency, ADD, metadata, SHOW, DDL round trip, ordinary DML', flush=True)
with instance():
    assert sql(f'SHOW AI COLUMNS FROM {database}.docs') == shown
    assert sql(f'SHOW CREATE TABLE {database}.docs').split('\t', 1)[1] == ddl
    assert sql(f'SELECT count(*) FROM oceanbase.__all_ai_column WHERE table_id={table_id}') == '2'
    sql(f'DROP DATABASE {database}')
    assert sql(f'SELECT count(*) FROM oceanbase.__all_ai_column WHERE table_id={table_id}') == '0'
    assert sql(f'SELECT count(*) FROM oceanbase.__all_ai_column_task WHERE table_id={table_id}') == '0'
    sql(f"CALL DBMS_AI_SERVICE.DROP_AI_MODEL('{model}')")
    print('PASS AI DDL: persisted schema after restart and DROP cleanup', flush=True)
