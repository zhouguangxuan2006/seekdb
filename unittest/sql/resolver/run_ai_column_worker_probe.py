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
    mock=repo/'tools/deploy/mysql_test/test_suite/ai_column/include/ai_column_mock.py'
    subprocess.run(['python3',str(mock),'start','19122'],check=True)
    try:
      sql("DROP DATABASE IF EXISTS ai_worker_probe; CREATE DATABASE ai_worker_probe")
      sql("CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('ai_worker_probe_ep')",ok=False)
      sql("CALL DBMS_AI_SERVICE.DROP_AI_MODEL('ai_worker_probe')",ok=False)
      sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL('ai_worker_probe','{\"type\":\"completion\",\"model_name\":\"mock-complete\"}')")
      sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('ai_worker_probe_ep','{\"ai_model_name\":\"ai_worker_probe\",\"url\":\"http://127.0.0.1:19122/v1/chat/completions\",\"access_key\":\"probe\",\"provider\":\"openai\"}')")
      sql("CREATE TABLE ai_worker_probe.docs(id BIGINT PRIMARY KEY, body VARCHAR(100), result TEXT AI COLUMN(AI_COMPLETE('ai_worker_probe',body)))")
      sql("INSERT INTO oceanbase.__all_ai_column_task(table_id,column_id,definition_version,request_id,rowkey_predicate,state,retry_count,max_retries,eligible_ts,claim_epoch,error_code,input_payload) SELECT table_id,column_id,definition_version,UUID(),'`id` = 99',0,0,0,NOW(6),0,0,JSON_ARRAY('ai_worker_probe','probe') FROM oceanbase.__all_ai_column WHERE model_name='ai_worker_probe'")
      import json
      drained = False
      for i in range(100):
        count = sql("SELECT count(*) FROM oceanbase.__all_ai_column_task WHERE table_id=(SELECT table_id FROM oceanbase.__all_ai_column WHERE model_name='ai_worker_probe')")
        if count == '0':
          drained = True
          break
        time.sleep(0.1)
      assert drained, 'worker did not drain an eligible durable task'
      events = [json.loads(line) for line in Path('/tmp/seekdb_ai_column_mock_19122/events.jsonl').read_text().splitlines()]
      assert len(events) == 1 and events[0]['user'] == 'probe', events
      assert sql("SELECT count(*) FROM ai_worker_probe.docs") == '0', 'writeback recreated a deleted row'
      print('PASS durable task claim, native model invocation, missing-row cleanup', flush=True)
      sql("DROP DATABASE ai_worker_probe; CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('ai_worker_probe_ep'); CALL DBMS_AI_SERVICE.DROP_AI_MODEL('ai_worker_probe')")
    finally:
      subprocess.run(['python3',str(mock),'stop','19122'])
