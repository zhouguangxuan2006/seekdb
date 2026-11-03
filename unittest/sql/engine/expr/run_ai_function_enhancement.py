#!/usr/bin/env python3
# Copyright (c) 2026 OceanBase.
# SPDX-License-Identifier: Apache-2.0
"""Run the unchanged AI enhancement mysqltest against a dedicated test instance.

SEEKDB_TEST_PORT defaults to 2888. Requires local port 8000 for the protocol mock.
This suite creates/drops the fixed enhance_* model names; use a test instance.
No external AI service, credentials, or GPUs are needed.
"""
import http.server
import json
import os
from pathlib import Path
import subprocess
import threading
import uuid

requests = []
class Mock(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        requests.append({'path': self.path, 'body': body})
        if self.path.startswith('/api/'):
            if self.headers.get('Content-Type') != 'application/json' or self.headers.get('Authorization'):
                self.send_error(400, 'Expected JSON without authorization for local Ollama')
                return
        if self.path.endswith('/api/generate') and body.get('stream') is not False:
            self.send_error(400, 'Non-streaming response must be requested')
            return
        if self.path.endswith('/chat/completions'):
            result = {'choices': [{'message': {'content': 'local completion response'}}]}
        elif self.path.endswith('/embeddings') or self.path.endswith('/api/embed'):
            values = body.get('input', [])
            if isinstance(values, str): values = [values]
            if any(not isinstance(value, str) for value in values):
                self.send_error(400, 'Embedding input must contain strings only')
                return
            dimension = body.get('dimensions', 768)
            vectors = [[0.01] * dimension for _ in values]
            result = {'embeddings': vectors} if self.path.endswith('/api/embed') else {
                'data': [{'index': i, 'embedding': v} for i, v in enumerate(vectors)]}
        elif self.path.endswith('/api/generate'):
            result = {'response': 'local completion response', 'done': True}
        elif self.path.endswith('/rerank'):
            result = {'results': [{'index': i, 'relevance_score': 1.0 / (i + 1)}
                                  for i, _ in enumerate(body.get('documents', []))][:body.get('top_n', len(body.get('documents', [])))]}
        else:
            self.send_error(404)
            return
        response = json.dumps(result).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(response)))
        self.end_headers()
        self.wfile.write(response)
    def log_message(self, *_): pass


def main():
    repo = Path(__file__).resolve().parents[4]
    suite = repo / 'tools/deploy/mysql_test/test_suite/ai_function'
    output = repo / 'build_release/ai_function_enhancement_tests'
    output.mkdir(exist_ok=True)
    port = os.environ.get('SEEKDB_TEST_PORT', '2888')
    mysql = [os.environ.get('SEEKDB_TEST_MYSQL', 'mysql'), '-h', '127.0.0.1', '-P', port, '-u', 'root']
    database = 'enhancement_' + uuid.uuid4().hex[:10]
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 8000), Mock)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        subprocess.run(mysql + ['-e', 'CREATE DATABASE ' + database], check=True)
        try:
            result = subprocess.run([str(repo / 'deps/3rd/u01/obclient/bin/mysqltest'),
                '--host=127.0.0.1', '--port=' + port, '--user=root', '--database=' + database,
                '--default-character-set=utf8mb4', '--test-file=' + str(suite / 't/ai_function_enhancement.test'),
                '--result-file=' + str(suite / 'r/mysql/ai_function_enhancement.result'),
                '--logdir=' + str(output)], text=True, capture_output=True, timeout=180)
            (output / 'run.log').write_text(result.stdout + result.stderr)
            (output / 'requests.json').write_text(json.dumps(requests, indent=2))
            print(result.stdout + result.stderr)
            return result.returncode
        finally:
            subprocess.run(mysql + ['-e', 'DROP DATABASE IF EXISTS ' + database], check=True)
    finally:
        server.shutdown()
        server.server_close()

if __name__ == '__main__':
    raise SystemExit(main())
