#!/usr/bin/env python3
# Native vector-index delta upsert/rollback regression without AI columns.
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
    sql("DROP DATABASE IF EXISTS ai_vector_transitions; CREATE DATABASE ai_vector_transitions")
    sql("CREATE TABLE ai_vector_transitions.docs(id BIGINT PRIMARY KEY, emb VECTOR(4))")
    sql("INSERT INTO ai_vector_transitions.docs VALUES(1,[0,0,0,0]),(2,[10,0,0,0]),(3,[20,0,0,0])")
    sql("CREATE VECTOR INDEX idx_emb ON ai_vector_transitions.docs(emb) WITH(distance=l2,type=hnsw,lib=vsag,m=10,ef_construction=12,ef_search=40)")
    for iteration in range(3):
        sql("UPDATE ai_vector_transitions.docs SET emb=NULL WHERE id=2")
        assert sql("SELECT count(*) FROM (SELECT id FROM ai_vector_transitions.docs ORDER BY l2_distance(emb,[0,0,0,0]) APPROXIMATE LIMIT 3) a") == '2'
        sql("UPDATE ai_vector_transitions.docs SET emb=[1,0,0,0] WHERE id=2")
        assert sql("SELECT id FROM ai_vector_transitions.docs ORDER BY l2_distance(emb,[1,0,0,0]) APPROXIMATE LIMIT 1") == '2'
        sql("BEGIN; UPDATE ai_vector_transitions.docs SET emb=NULL WHERE id=2; ROLLBACK")
        assert sql("SELECT id FROM ai_vector_transitions.docs ORDER BY l2_distance(emb,[1,0,0,0]) APPROXIMATE LIMIT 1") == '2'
    sql("DROP DATABASE ai_vector_transitions")
    print('PASS ordinary vector repeated NULL/regeneration transitions, ANN and rollback')
