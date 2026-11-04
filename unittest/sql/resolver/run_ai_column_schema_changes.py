#!/usr/bin/env python3
# AI-column schema lifecycle, hidden primary keys and vector regressions.
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


import json
mock = repo/'tools/deploy/mysql_test/test_suite/ai_column/include/ai_column_mock.py'
port = 19125

def wait_sql(query, expected='1'):
    for _ in range(300):
        value = sql(query)
        if value == expected:
            return
        time.sleep(0.1)
    raise AssertionError(query + ': ' + value)

subprocess.run(['python3',str(mock),'start',str(port)],check=True)
try:
  with instance():
    sql("DROP DATABASE IF EXISTS ai_changes; CREATE DATABASE ai_changes")
    sql("CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('ai_changes_ep')",ok=False)
    sql("CALL DBMS_AI_SERVICE.DROP_AI_MODEL('ai_changes')",ok=False)
    sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL('ai_changes','{\"type\":\"completion\",\"model_name\":\"mock-complete\"}')")
    sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('ai_changes_ep','{\"ai_model_name\":\"ai_changes\",\"url\":\"http://127.0.0.1:19125/v1/chat/completions\",\"access_key\":\"schema-test\",\"provider\":\"openai\"}')")
    sql("CREATE TABLE ai_changes.docs(id BIGINT PRIMARY KEY, body VARCHAR(200), unrelated INT)")
    sql("INSERT INTO ai_changes.docs VALUES(1,'before-add',0),(2,'second-row',0)")
    sql("ALTER TABLE ai_changes.docs ADD COLUMN result TEXT AI COLUMN(AI_COMPLETE('ai_changes',body))")
    wait_sql("SELECT count(*)=2 AND SUM(result=CONCAT('mock:',body))=2 FROM ai_changes.docs")
    print('PASS ADD backfills existing rows',flush=True)
    sql("ALTER TABLE ai_changes.docs ADD COLUMN extra INT")
    sql("ALTER TABLE ai_changes.docs MODIFY COLUMN unrelated BIGINT")
    sql("UPDATE ai_changes.docs SET body='after-unrelated-ddl' WHERE id=1")
    wait_sql("SELECT result='mock:after-unrelated-ddl' FROM ai_changes.docs WHERE id=1")
    print('PASS unrelated schema changes preserve task definition identity',flush=True)
    sql("UPDATE ai_changes.docs SET body='slow-old' WHERE id=1")
    sql("ALTER TABLE ai_changes.docs MODIFY COLUMN result TEXT AI COLUMN(AI_COMPLETE('ai_changes',CONCAT('new:',body))) WITH(max_retries=0)")
    wait_sql("SELECT count(*)=2 AND SUM(result=CONCAT('mock:new:',body))=2 FROM ai_changes.docs")
    time.sleep(2)
    assert sql("SELECT count(*)=2 AND SUM(result=CONCAT('mock:new:',body))=2 FROM ai_changes.docs")=='1'
    shown=sql("SHOW AI COLUMNS FROM ai_changes.docs")
    assert 'new:' in shown,shown
    print('PASS MODIFY invalidates values, replaces tasks and fences late old-definition results',flush=True)
    bad=sql("ALTER TABLE ai_changes.docs DROP COLUMN body",ok=False)
    assert bad.returncode, 'dropping a live AI dependency must fail'
    sql("UPDATE ai_changes.docs SET body='slow-old-drop' WHERE id=1")
    table_id=sql("SELECT table_id FROM oceanbase.__all_table WHERE table_name='docs' AND database_id=(SELECT database_id FROM oceanbase.__all_database WHERE database_name='ai_changes')")
    sql("ALTER TABLE ai_changes.docs DROP COLUMN result")
    assert sql(f"SELECT count(*) FROM oceanbase.__all_ai_column WHERE table_id={table_id}")=='0'
    assert sql(f"SELECT count(*) FROM oceanbase.__all_ai_column_task WHERE table_id={table_id}")=='0'
    assert sql(f"SELECT count(*) FROM oceanbase.__all_column WHERE table_id={table_id} AND column_name LIKE '__ai_%'")=='0'
    sql("ALTER TABLE ai_changes.docs DROP COLUMN body")
    sql("INSERT INTO ai_changes.docs(id) VALUES(3)")
    print('PASS DROP cleans metadata, hidden columns and running tasks',flush=True)
    sql("CREATE TABLE ai_changes.heap(body VARCHAR(100), result TEXT AI COLUMN(AI_COMPLETE('ai_changes',body)))")
    sql("INSERT INTO ai_changes.heap(body) VALUES('heap-one'),('heap-two')")
    wait_sql("SELECT count(*)=2 AND SUM(result=CONCAT('mock:',body))=2 FROM ai_changes.heap")
    sql("UPDATE ai_changes.heap SET body='heap-updated' WHERE body='heap-one'")
    wait_sql("SELECT result='mock:heap-updated' FROM ai_changes.heap WHERE body='heap-updated'")
    sql("ALTER TABLE ai_changes.heap ADD COLUMN extra TEXT AI COLUMN(AI_COMPLETE('ai_changes',CONCAT('extra:',body)))")
    wait_sql("SELECT count(*)=2 AND SUM(extra=CONCAT('mock:extra:',body))=2 FROM ai_changes.heap")
    print('PASS tables with native hidden primary keys, including ADD backfill',flush=True)
    sql("CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('ai_changes_vec_ep')",ok=False)
    sql("CALL DBMS_AI_SERVICE.DROP_AI_MODEL('ai_changes_vec')",ok=False)
    sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL('ai_changes_vec','{\"type\":\"dense_embedding\",\"model_name\":\"mock-embed\"}')")
    sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('ai_changes_vec_ep','{\"ai_model_name\":\"ai_changes_vec\",\"url\":\"http://127.0.0.1:19125/v1/embeddings\",\"access_key\":\"schema-test\",\"provider\":\"openai\"}')")
    sql("CREATE TABLE ai_changes.vectors(id BIGINT PRIMARY KEY, body VARCHAR(100), emb VECTOR(4) AI COLUMN(AI_EMBED('ai_changes_vec',body)))")
    sql("INSERT INTO ai_changes.vectors(id,body) VALUES(1,'vec-ten'),(2,'vec-twenty')")
    wait_sql("SELECT count(*)=2 FROM ai_changes.vectors WHERE emb IS NOT NULL")
    sql("CREATE VECTOR INDEX idx_emb ON ai_changes.vectors(emb) WITH(distance=l2,type=hnsw,lib=vsag,m=10,ef_construction=12,ef_search=40)")
    sql("ALTER TABLE ai_changes.vectors MODIFY COLUMN emb VECTOR(4) AI COLUMN(AI_EMBED('ai_changes_vec',CONCAT('vec-zero:',body)))")
    wait_sql("SELECT count(*)=2 AND SUM(emb=[0,0,0,0])=2 FROM ai_changes.vectors")
    assert sql("SELECT count(*)=2 AND MAX(distance)=0 FROM (SELECT l2_distance(emb,[0,0,0,0]) AS distance FROM ai_changes.vectors ORDER BY distance APPROXIMATE LIMIT 2) a")=='1'
    print('PASS vector definition replacement maintains ANN index',flush=True)
    for iteration in range(3):
        sql("UPDATE ai_changes.vectors SET body=CONCAT(body,'-changed') WHERE id=1")
        wait_sql("SELECT count(*)=2 AND SUM(emb=[0,0,0,0])=2 FROM ai_changes.vectors")
        sql("ALTER TABLE ai_changes.vectors RETRY AI COLUMN emb WHERE id=2")
        wait_sql("SELECT count(*)=2 AND SUM(emb=[0,0,0,0])=2 FROM ai_changes.vectors")
        sql("BEGIN; UPDATE ai_changes.vectors SET body='rollback' WHERE id=1; ROLLBACK")
        assert sql("SELECT count(*)=2 AND MAX(distance)=0 FROM (SELECT l2_distance(emb,[0,0,0,0]) AS distance FROM ai_changes.vectors ORDER BY distance APPROXIMATE LIMIT 2) a")=='1'
    print('PASS repeated AI vector dependency updates, retry and rollback',flush=True)

    sql("DROP DATABASE ai_changes; CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('ai_changes_ep'); CALL DBMS_AI_SERVICE.DROP_AI_MODEL('ai_changes'); CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('ai_changes_vec_ep'); CALL DBMS_AI_SERVICE.DROP_AI_MODEL('ai_changes_vec')")
finally:
  subprocess.run(['python3',str(mock),'stop',str(port)])
