#!/usr/bin/env python3
# AI-column transaction, input, strict-read and crash-recovery regressions.
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
    sql("DROP DATABASE IF EXISTS ai_column_extra; CREATE DATABASE ai_column_extra")
    sql("CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('ai_column_extra_ep')",ok=False)
    sql("CALL DBMS_AI_SERVICE.DROP_AI_MODEL('ai_column_extra')",ok=False)
    sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL('ai_column_extra','{\"type\":\"completion\",\"model_name\":\"mock-complete\"}')")
    sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('ai_column_extra_ep','{\"ai_model_name\":\"ai_column_extra\",\"url\":\"http://127.0.0.1:19125/v1/chat/completions\",\"access_key\":\"extra-test\",\"provider\":\"openai\"}')")
    sql("CREATE TABLE ai_column_extra.docs(id BIGINT PRIMARY KEY, body LONGTEXT, result LONGTEXT AI COLUMN(AI_COMPLETE('ai_column_extra',body,'{\"temperature\":0.2}')))")
    sql("BEGIN; SAVEPOINT keep; INSERT INTO ai_column_extra.docs(id,body) VALUES(98,'rolled-back'); ROLLBACK TO SAVEPOINT keep; COMMIT")
    time.sleep(0.3)
    assert Path('/tmp/seekdb_ai_column_mock_19125/events.jsonl').read_text() == '', 'rollback triggered model call'
    failed = sql("INSERT INTO ai_column_extra.docs(id,body) VALUES(99,'first'),(99,'duplicate')", ok=False)
    assert failed.returncode, 'duplicate-key statement unexpectedly succeeded'
    assert sql("SELECT count(*) FROM ai_column_extra.docs") == '0'
    time.sleep(0.3)
    assert Path('/tmp/seekdb_ai_column_mock_19125/events.jsonl').read_text() == '', 'failed statement triggered model call'
    sql("INSERT INTO ai_column_extra.docs(id,body) VALUES(1,REPEAT('x',20000))")
    wait_sql("SELECT result = CONCAT('mock:',body) FROM ai_column_extra.docs WHERE id=1")
    sql("UPDATE ai_column_extra.docs SET body=CONCAT(body,'y') WHERE id=1")
    wait_sql("SELECT result = CONCAT('mock:',body) FROM ai_column_extra.docs WHERE id=1")
    subprocess.run(['python3',str(mock),'stop',str(port)],check=True)
    prepared = subprocess.run(client + ['--force','-e', "SET ai_column_strict_read=ON; PREPARE check_ai FROM 'SELECT result IS NOT NULL FROM ai_column_extra.docs WHERE id=1'; EXECUTE check_ai; SET ai_column_strict_read=OFF; ALTER TABLE ai_column_extra.docs RETRY AI COLUMN result WHERE id=1; SET ai_column_strict_read=ON; EXECUTE check_ai; DEALLOCATE PREPARE check_ai"],text=True,capture_output=True)
    assert '11120' in prepared.stderr and prepared.stdout.splitlines()[0] == '1', prepared.stdout + prepared.stderr
    subprocess.run(['python3',str(mock),'start',str(port)],check=True)
    sql("ALTER TABLE ai_column_extra.docs RETRY AI COLUMN result")
    wait_sql("SELECT result = CONCAT('mock:',body) FROM ai_column_extra.docs WHERE id=1")
    sql("CREATE TABLE ai_column_extra.case_keys(id VARCHAR(20) COLLATE utf8mb4_bin PRIMARY KEY, body VARCHAR(100), result TEXT AI COLUMN(AI_COMPLETE('ai_column_extra',body)))")
    out=sql("BEGIN; INSERT INTO ai_column_extra.case_keys(id,body) VALUES('a','lower'),('A','upper'); UPDATE ai_column_extra.case_keys SET body='changed' WHERE id='a'; SELECT count(*) FROM oceanbase.__all_ai_column_task WHERE table_id=(SELECT table_id FROM oceanbase.__all_table WHERE database_id=(SELECT database_id FROM oceanbase.__all_database WHERE database_name='ai_column_extra') AND table_name='case_keys'); ROLLBACK")
    assert out == '2', out
    sql("CREATE TABLE ai_column_extra.json_input(id BIGINT PRIMARY KEY, doc JSON, result TEXT AI COLUMN(AI_COMPLETE('ai_column_extra',json_unquote(json_extract(doc,'$.text')))))")
    sql("INSERT INTO ai_column_extra.json_input(id,doc) VALUES(1,JSON_OBJECT('text',CONCAT('quote',CHAR(39),' slash',CHAR(92),' end')))")
    wait_sql("SELECT result=CONCAT('mock:',json_unquote(json_extract(doc,'$.text'))) FROM ai_column_extra.json_input WHERE id=1")
    rejected=sql("UPDATE ai_column_extra.docs SET result='manual' WHERE id=1",ok=False)
    assert rejected.returncode and '1235' in rejected.stderr, rejected.stderr
    sql("CREATE TABLE ai_column_extra.short_result(id BIGINT PRIMARY KEY, body VARCHAR(100), result VARCHAR(2) AI COLUMN(AI_COMPLETE('ai_column_extra',body)) WITH(max_retries=0))")
    sql("INSERT INTO ai_column_extra.short_result(id,body) VALUES(1,'too-long')")
    wait_sql("SELECT count(*) = 0 FROM oceanbase.__all_ai_column_task WHERE table_id=(SELECT table_id FROM oceanbase.__all_table WHERE database_id=(SELECT database_id FROM oceanbase.__all_database WHERE database_name='ai_column_extra') AND table_name='short_result')")
    assert sql("SELECT result IS NULL FROM ai_column_extra.short_result") == '1'
    bad=sql("SET ai_column_strict_read=ON; SELECT result FROM ai_column_extra.short_result",ok=False)
    assert bad.returncode and '11120' in bad.stderr, bad.stderr
    sql("BEGIN; INSERT INTO ai_column_extra.docs(id,body) VALUES(2,'restart-pending'); UPDATE oceanbase.__all_ai_column_task SET state=1,claim_owner='terminated-process',claim_epoch=7,lease_until=DATE_ADD(NOW(6),INTERVAL 1 HOUR) WHERE table_id=(SELECT table_id FROM oceanbase.__all_table WHERE database_id=(SELECT database_id FROM oceanbase.__all_database WHERE database_name='ai_column_extra') AND table_name='docs'); COMMIT")
  with instance():
    wait_sql("SELECT result='mock:restart-pending' FROM ai_column_extra.docs WHERE id=2")
    assert sql("SELECT count(*) FROM oceanbase.__all_ai_column_task WHERE claim_owner='terminated-process'") == '0'
    sql("DROP DATABASE ai_column_extra; CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('ai_column_extra_ep'); CALL DBMS_AI_SERVICE.DROP_AI_MODEL('ai_column_extra')")
    print('PASS savepoint rollback, out-of-row TEXT inputs, JSON configuration, truncation rejection, failed strict read, durable lease recovery')
finally:
  subprocess.run(['python3',str(mock),'stop',str(port)])
