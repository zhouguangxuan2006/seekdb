#!/usr/bin/env python3
"""Compare complete FTS results with a reference binary, including DML and rescan."""
import argparse
from contextlib import contextmanager
from concurrent.futures import ThreadPoolExecutor
import threading
import json
import os
from pathlib import Path
import random
import socket
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--binary', required=True)
parser.add_argument('--base-dir', required=True)
parser.add_argument('--snapshot', required=True)
parser.add_argument('--compare')
parser.add_argument('--port', type=int, default=2892)
parser.add_argument('--public', action='store_true')
parser.add_argument('--expect-filter-only', action='store_true')
args = parser.parse_args()
repo = Path(__file__).resolve().parents[3]
base = Path(args.base_dir).resolve()
base.mkdir(parents=True, exist_ok=True)
client = ['mysql', '-h127.0.0.1', f'-P{args.port}', '-uroot', '-N', '-s', '-A',
          '--raw', '--default-character-set=utf8mb4']

def sql(query):
    result = subprocess.run(client + ['-e', 'SET NAMES utf8mb4; SET ob_query_timeout=1000000000; ' + query],
                            text=True, capture_output=True)
    if result.returncode:
        raise AssertionError(result.stderr + '\n' + query)
    return result.stdout

def literal(value):
    return "CONVERT(X'" + value.encode('utf8').hex() + "' USING utf8mb4)"

@contextmanager
def instance():
    with socket.socket() as sock:
        if sock.connect_ex(('127.0.0.1', args.port)) == 0:
            raise RuntimeError('Test port already in use')
    with (base / 'regression-server.log').open('w') as log:
        server = subprocess.Popen([str(Path(args.binary).resolve()), '--nodaemon',
            '--base-dir=' + str(base), '--port=' + str(args.port), '--parameter', 'cpu_count=2',
            '--parameter', 'memory_limit=2G', '--parameter', 'datafile_size=2G',
            '--parameter', 'log_disk_size=8G', '--log-level=WARN'], cwd=base,
            stdout=log, stderr=subprocess.STDOUT)
        try:
            for _ in range(120):
                if server.poll() is not None:
                    raise RuntimeError('Server exited before readiness')
                if subprocess.run(client + ['-e', 'SELECT 1'], capture_output=True).returncode == 0:
                    break
                time.sleep(1)
            else:
                raise RuntimeError('Readiness timed out')
            yield
        finally:
            server.terminate()
            server.wait(timeout=60)

answers = []
def capture(query, allow_error=False):
    if allow_error:
        result = subprocess.run(client + ['-e', query], text=True, capture_output=True)
        answers.append({'sql': query, 'result': result.stdout, 'error': result.stderr, 'exit': result.returncode})
    else:
        answers.append({'sql': query, 'result': sql(query)})

with instance():
    sql('DROP DATABASE IF EXISTS fts_regression; CREATE DATABASE fts_regression; CREATE DATABASE IF NOT EXISTS test')
    rng = random.Random(29173)
    vocabulary = ['index', 'query', 'database', 'UPPER', 'MixedCase', 'running', 'the', 'and',
                  '中文', '分词', '全文检索', '数据库', '性能优化', '中华人民共和国', '一百二十三',
                  '3.14', '12,345', "can't", 'foo_bar', 'alpha-beta', 'café', 'Straße',
                  '日本語', '한글', '𠀀', '🙂', 'İı', 'ＡＢＣ', 'a\x00b', 'x\ty\nz']
    texts = ['', ' ', '!!!', 'lowercase 123', 'UPPERCASE MixedCase', '数据库全文检索',
             '中国人民银行', '3公斤一百二十三万元', 'a' * 200, ('数据库 index 123.4!\n' * 450)]
    texts += [' '.join(rng.choices(vocabulary, k=rng.randint(1, 30))) for _ in range(80)]
    # Race first dictionary lookups, and verify concurrent callers against their
    # own serial answers as well as the reference-binary snapshot.
    barrier = threading.Barrier(4)
    concurrent_queries = [f"SELECT TOKENIZE({literal(text)},'ik','[{{\"output\":\"all\"}}]');" * 16
                          for text in texts[3:7]]
    def concurrent_tokenize(query):
        barrier.wait(timeout=30)
        return sql(query)
    with ThreadPoolExecutor(max_workers=4) as pool:
        concurrent_answers = list(pool.map(concurrent_tokenize, concurrent_queries))
    for query, result in zip(concurrent_queries, concurrent_answers):
        if result != sql(query):
            raise AssertionError('Concurrent tokenization differs from serial evaluation')
        answers.append({'sql': query, 'result': result})

    # Grouping must retain the original frequency and last representation of
    # keys with equal collation weights, including repeated accented tokens.
    for parser_name in ['ik', 'beng', 'space', 'ngram', 'ngram2']:
        for text in ['cafe café CAFÉ cafe CAFÉ', 'e é E É e é' * 80,
                     'index INDEX index 中文 中文 𠀀 𠀀 ' * 100]:
            capture(f"SELECT TOKENIZE({literal(text)},'{parser_name}','[{{\"output\":\"all\"}}]')")

    # Exercise shared immutable stopwords with concurrent callers from all
    # built-in tokenizers, comparing each caller to its own serial result.
    for parser_name in ['beng', 'space', 'ngram', 'ngram2']:
        barrier = threading.Barrier(4)
        queries = [f"SELECT TOKENIZE({literal(text)},'{parser_name}','[{{\"output\":\"all\"}}]');" * 16
                   for text in ['the and FROM where café café', 'lowercase lower UPPER lower',
                                '中文 index index FULLTEXT 123', 'alpha beta gamma ' * 80]]
        with ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(concurrent_tokenize, queries))
        for query, result in zip(queries, results):
            if result != sql(query):
                raise AssertionError('Concurrent parser differs from serial: ' + parser_name)
            answers.append({'sql': query, 'result': result})
    # ASCII width/normalization fast paths must retain every delimiter and
    # mixed-charset fallback, including embedded NUL and non-ASCII suffixes.
    english_boundaries = [''.join('alpha' + chr(c) + 'beta ' for c in range(128)),
                          'lowercase ' * 120, 'lower UPPER lowercase café 中文 İı 𠀀',
                          'alpha\x00beta\tGAMMA\r\ndelta...epsilon__zeta']
    for collation in ['utf8mb4_general_ci', 'utf8mb4_unicode_ci', 'utf8mb4_bin']:
        for text in english_boundaries:
            capture(f"SELECT TOKENIZE({literal(text)} COLLATE {collation},'beng','[{{\"output\":\"all\"}}]')")

    default_options = {
        'ik': [{'ik_mode': 'smart'}],
        'space': [{'min_token_size': 3}, {'max_token_size': 84}],
        'beng': [{'min_token_size': 3}, {'max_token_size': 84}],
        'ngram': [{'ngram_token_size': 2}],
        'ngram2': [{'min_ngram_size': 2}, {'max_ngram_size': 3}],
    }
    option_text = literal('中国人民银行 数据库 ABC lowercase 123 ' + 'a' * 84)
    for parser_name, options in default_options.items():
        for output in ['all', 'default']:
            implicit = json.dumps([{'output': output}])
            explicit = json.dumps([{'output': output}, {'additional_args': options}])
            q1 = f"SELECT TOKENIZE({option_text},'{parser_name}','{implicit}')"
            q2 = f"SELECT TOKENIZE({option_text},'{parser_name}','{explicit}')"
            capture(q1)
            capture(q2)
            if answers[-1]['result'] != answers[-2]['result']:
                raise AssertionError('Built-in and explicit defaults differ: ' + parser_name)
    for parser_name, options in [('ik', [{'ik_mode': 'max_word'}]),
                                 ('beng', [{'min_token_size': 1}, {'max_token_size': 5}]),
                                 ('ngram', [{'ngram_token_size': 3}]),
                                 ('ik', [{'ik_mode': 'invalid'}]),
                                 ('beng', [{'min_token_size': 9}, {'max_token_size': 2}])]:
        config = json.dumps([{'output': 'all'}, {'additional_args': options}])
        capture(f"SELECT TOKENIZE({option_text},'{parser_name}','{config}')", allow_error=True)
    capture("SELECT TOKENIZE(txt,parser,config) FROM (SELECT 'ABC database' txt,'beng' parser,'[]' config UNION ALL SELECT '数据库全文','ik','[{\"additional_args\":[{\"ik_mode\":\"max_word\"}]}]' UNION ALL SELECT 'A B CD','space','[]') t", allow_error=True)
    for parser_name in ['ik', 'beng', 'space', 'ngram']:
        for text in texts:
            capture(f"SELECT TOKENIZE({literal(text)},'{parser_name}','[{{\"output\":\"all\"}}]')")
        capture(f"SELECT TOKENIZE(CAST(NULL AS CHAR CHARACTER SET utf8mb4),'{parser_name}')")
    for charset in ['latin1', 'utf16', 'utf16le', 'binary']:
        for parser_name in ['ik', 'beng', 'space', 'ngram']:
            capture(f"SELECT TOKENIZE(CONVERT('lower UPPER 123 abc' USING {charset}),'{parser_name}')", allow_error=True)
    for charset in ['utf8mb4', 'utf16', 'utf16le']:
        for parser_name in ['ik', 'beng']:
            capture(f"SELECT TOKENIZE(CONVERT({literal('中文𠀀🙂 upper LOWER 123')} USING {charset}),'{parser_name}')", allow_error=True)
    for charset, raw in [('utf8mb4', 'c3'), ('utf8mb4', 'e4b8'), ('utf8mb4', 'f09f92'),
                         ('utf16', 'd800'), ('utf16le', '00d8')]:
        for parser_name in ['ik', 'beng']:
            capture(f"SELECT TOKENIZE(CONVERT(X'{raw}' USING {charset}),'{parser_name}')", allow_error=True)
    sql('CREATE TABLE fts_regression.docs(id INT PRIMARY KEY, title VARCHAR(300), body TEXT, note INT)')
    rows = []
    for i in range(1, 801):
        title = ' '.join(rng.choices(vocabulary[:18], k=4))
        body = ' '.join(rng.choices(vocabulary, k=rng.randint(1, 40)))
        rows.append(f'({i},{literal(title)},{literal(body)},{i % 7})')
    for start in range(0, len(rows), 100):
        sql('INSERT INTO fts_regression.docs VALUES ' + ','.join(rows[start:start+100]))
    # Exercise the shared comparison path on ordinary SQL, including NULLs,
    # collation ties, mixed directions and several datum types. IDs break ties
    # so complete output is deterministic across reference and candidate builds.
    for expr in ['body COLLATE utf8mb4_general_ci', 'body COLLATE utf8mb4_bin',
                 "IF(id%7=0,NULL,body)", 'MOD(id,17)', 'CAST(id/7 AS DECIMAL(12,3))',
                 "DATE_ADD('2020-01-01', INTERVAL MOD(id,31) DAY)"]:
        for order in ['ASC', 'DESC']:
            for limit in ['', ' LIMIT 97 OFFSET 3']:
                capture(f"SELECT /*+ opt_param('enable_newsort','false') */ id,{expr} AS k "
                        f"FROM fts_regression.docs ORDER BY k {order},note DESC,id{limit}")
    # Compare repeated byte strings and distinct strings with equal collation
    # weights. Include empty/NULL values, embedded NULs, trailing spaces,
    # accents and supplementary characters across sort/group/join operations.
    sql("CREATE TABLE fts_regression.string_keys(id INT PRIMARY KEY, s VARCHAR(1024) CHARACTER SET utf8mb4)")
    key_values = [None, '', ' ', 'a', 'A', 'a ', 'a  ', 'a\x00', 'ä', 'Ä',
                  'é', 'e', 'ｅ', 'ß', 'ss', '中文', '𠀀', '🙂', 'prefix' * 100]
    key_rows = [f"({i}," + ('NULL' if value is None else literal(value)) + ')'
                for i, value in enumerate(key_values * 4, 1)]
    sql('INSERT INTO fts_regression.string_keys VALUES ' + ','.join(key_rows))
    for collation in ['utf8mb4_general_ci', 'utf8mb4_unicode_ci', 'utf8mb4_bin']:
        key = f's COLLATE {collation}'
        for newsort in ['false', 'true']:
            for order in ['ASC', 'DESC']:
                capture(f"SELECT /*+ opt_param('enable_newsort','{newsort}') */ id,HEX(s) "
                        f"FROM fts_regression.string_keys ORDER BY {key} {order},id",
                        allow_error=(newsort == 'true' and collation == 'utf8mb4_unicode_ci'))
        capture(f"SELECT MIN(id),COUNT(*),GROUP_CONCAT(id ORDER BY id) "
                f"FROM fts_regression.string_keys GROUP BY {key} ORDER BY MIN(id)")
        capture(f"SELECT COUNT(DISTINCT {key}) FROM fts_regression.string_keys")
        capture(f"SELECT a.id,b.id FROM fts_regression.string_keys a "
                f"JOIN fts_regression.string_keys b ON a.s COLLATE {collation}=b.s COLLATE {collation} "
                f"WHERE a.id<=19 ORDER BY a.id,b.id")
    # Exhaust ASCII weights and fallback boundaries without encoding raw
    # controls in the SQL output. Compare complete sorted IDs and group members.
    sql("CREATE TABLE fts_regression.ascii_keys(id INT PRIMARY KEY, s VARCHAR(1024) CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci)")
    ascii_values = [prefix + chr(c) + suffix
                    for prefix in ['', 'aA', 'sameprefix']
                    for c in range(128)
                    for suffix in ['', ' ', '  ', 'é中文🙂']]
    ascii_rows = [f"({i},{literal(value)})" for i, value in enumerate(ascii_values, 1)]
    for start in range(0, len(ascii_rows), 100):
        sql('INSERT INTO fts_regression.ascii_keys VALUES ' + ','.join(ascii_rows[start:start+100]))
    for newsort in ['false', 'true']:
        for order in ['ASC', 'DESC']:
            capture(f"SELECT /*+ opt_param('enable_newsort','{newsort}') */ id,HEX(s) "
                    f"FROM fts_regression.ascii_keys ORDER BY s {order},id")
    capture("SELECT MIN(id),COUNT(*),GROUP_CONCAT(id ORDER BY id) FROM fts_regression.ascii_keys GROUP BY s ORDER BY MIN(id)")
    capture("SELECT COUNT(DISTINCT s) FROM fts_regression.ascii_keys")

    sql('ALTER TABLE fts_regression.docs ADD FULLTEXT INDEX f_ik(title,body) WITH PARSER ik')
    sql('ALTER TABLE fts_regression.docs ADD FULLTEXT INDEX f_beng(body) WITH PARSER beng')

    def query_cases():
        for columns in ['title,body', 'body']:
            for term in ['database', '全文 数据库', 'query index', 'notpresent', 'index 中文',
                         'the and', 'UPPER MixedCase', '+database -query', 'data*',
                         'database query index', 'database query index 中文 全文',
                         'database database query', '+database +query -index',
                         'database absenttoken query anothermissingtoken']:
                for mode in ['', ' IN BOOLEAN MODE']:
                    match = f'MATCH({columns}) AGAINST({literal(term)}{mode})'
                    # Membership-only scans must return the full same row set,
                    # including empty results, Boolean mode and post-DML reads.
                    capture(f'SELECT id FROM fts_regression.docs WHERE {match} ORDER BY id', allow_error=bool(mode and term.endswith('*')))
                    capture(f'SELECT COUNT(*),SUM(id),MIN(id),MAX(id) FROM fts_regression.docs WHERE {match}', allow_error=bool(mode and term.endswith('*')))
                    capture(f'SELECT note,COUNT(*),SUM(id) FROM fts_regression.docs WHERE {match} GROUP BY note HAVING COUNT(*)>2 ORDER BY note', allow_error=bool(mode and term.endswith('*')))
                    # Compare IDs and full precision scores, not only hit counts.
                    capture(f'SELECT id,{match} AS score FROM fts_regression.docs WHERE {match} ORDER BY id', allow_error=bool(mode and term.endswith('*')))
                    capture(f'SELECT id,{match} AS score FROM fts_regression.docs WHERE {match} > 0.1 AND note <> 3 ORDER BY score DESC,id LIMIT 13 OFFSET 2', allow_error=bool(mode and term.endswith('*')))
        capture("SELECT d.id FROM (SELECT 1 AS k UNION ALL SELECT 2) a JOIN fts_regression.docs d ON d.note=a.k WHERE MATCH(d.body) AGAINST('database') ORDER BY d.id")
    # Scores referenced through aliases, expressions, grouping and ordering
    # must never be replaced by membership weights.
    m = "MATCH(body) AGAINST('database query')"
    for statement in [
        f"SELECT id,{m} AS score FROM fts_regression.docs WHERE {m} ORDER BY score DESC,id LIMIT 17",
        f"SELECT id,{m}*7+0.25 AS score FROM fts_regression.docs WHERE {m} ORDER BY id",
        f"SELECT ROUND({m},8),COUNT(*) FROM fts_regression.docs WHERE {m} GROUP BY ROUND({m},8) ORDER BY 1",
        f"SELECT COUNT(*) FROM (SELECT id,{m} AS score FROM fts_regression.docs WHERE {m}) d WHERE score>0.75",
        f"SELECT id FROM fts_regression.docs WHERE {m} ORDER BY {m} DESC,id LIMIT 17 OFFSET 3",
        f"SELECT id FROM fts_regression.docs WHERE {m}>1 AND {m} ORDER BY id",
        f"SELECT id FROM fts_regression.docs WHERE {m} AND NOT ({m}>100) ORDER BY id",
        "SELECT id FROM fts_regression.docs WHERE MATCH(body) AGAINST('') ORDER BY id",
        "SELECT id FROM fts_regression.docs WHERE MATCH(body) AGAINST(NULL) ORDER BY id",
        "SELECT id FROM fts_regression.docs WHERE MATCH(body) AGAINST('the and') ORDER BY id",
        "SELECT id FROM fts_regression.docs WHERE MATCH(body) AGAINST('database') OR note=1 ORDER BY id",
        "SELECT id FROM fts_regression.docs WHERE NOT MATCH(body) AGAINST('database') ORDER BY id",
        "SELECT id FROM fts_regression.docs WHERE MATCH(body) AGAINST('database') AND id<50 ORDER BY id",
        "SELECT COUNT(*) FROM (SELECT id FROM fts_regression.docs WHERE MATCH(body) AGAINST('database') LIMIT 20 OFFSET 3) d",
        "SELECT a.k,(SELECT COUNT(*) FROM fts_regression.docs d WHERE d.note=a.k AND MATCH(d.body) AGAINST('database')) FROM (SELECT 1 AS k UNION ALL SELECT 2 UNION ALL SELECT 5) a ORDER BY a.k",
        "PREPARE ft FROM 'SELECT id FROM fts_regression.docs WHERE MATCH(body) AGAINST(?) ORDER BY id'; SET @q='database'; EXECUTE ft USING @q; SET @q='notpresent'; EXECUTE ft USING @q; SET @q='query'; EXECUTE ft USING @q; DEALLOCATE PREPARE ft",
    ]:
        capture(statement)

    # More than 256 unique search terms uses the spill/hash path. Multiple
    # terms hit each document, including duplicate query words and empty rows.
    long_words = ['word' + chr(97+i//26) + chr(97+i%26) for i in range(300)]
    sql('CREATE TABLE fts_regression.long_queries(id INT PRIMARY KEY, body TEXT, FULLTEXT INDEX ft(body) WITH PARSER space)')
    docs = [' '.join(long_words), ' '.join(long_words[:120]), ' '.join(long_words[120:]), '', None]
    sql('INSERT INTO fts_regression.long_queries VALUES ' + ','.join(
        f"({i}," + ('NULL' if text is None else literal(text)) + ')' for i,text in enumerate(docs,1)))
    for text in [' '.join(long_words), ' '.join(long_words+long_words[:20]), ' '.join(long_words[::-1])]:
        match = f"MATCH(body) AGAINST({literal(text)})"
        capture(f"SELECT id FROM fts_regression.long_queries WHERE {match} ORDER BY id")
        capture(f"SELECT COUNT(*),SUM(id) FROM fts_regression.long_queries WHERE {match}")
    if args.expect_filter_only:
        cases = [
            (f"SELECT COUNT(*) FROM fts_regression.docs WHERE {m}", False),
            (f"SELECT id FROM fts_regression.docs WHERE {m} ORDER BY id", False),
            (f"SELECT id,{m} FROM fts_regression.docs WHERE {m} ORDER BY id", True),
            (f"SELECT id FROM fts_regression.docs WHERE {m}", True),
            (f"SELECT id FROM fts_regression.docs WHERE {m} LIMIT 20", True),
            (f"SELECT id FROM fts_regression.docs WHERE {m}>0.1 ORDER BY id", True),
            (f"SELECT id FROM fts_regression.docs WHERE {m} ORDER BY {m} DESC,id", True),
            ("SELECT id FROM fts_regression.docs WHERE MATCH(body) AGAINST('database' IN BOOLEAN MODE) ORDER BY id", True),
        ]
        for statement, scores in cases:
            plan = sql('EXPLAIN EXTENDED ' + statement)
            expected = 'calc_relevance=' + str(scores).lower()
            if (not scores and expected not in plan) or (scores and 'calc_relevance=false' in plan):
                raise AssertionError('Missing plan property ' + expected + ': ' + statement + '\n' + plan)
    query_cases()
    sql("UPDATE fts_regression.docs SET body='database changed 全文分词' WHERE id%11=0; DELETE FROM fts_regression.docs WHERE id%13=0")
    capture("SELECT id FROM fts_regression.docs WHERE MATCH(body) AGAINST('changed') ORDER BY id")
    capture("BEGIN; UPDATE fts_regression.docs SET body='rolledback keyword' WHERE id%17=0; SELECT id FROM fts_regression.docs WHERE MATCH(body) AGAINST('rolledback') ORDER BY id; ROLLBACK; SELECT id FROM fts_regression.docs WHERE MATCH(body) AGAINST('rolledback') ORDER BY id")
    query_cases()
    sql('DROP DATABASE fts_regression')
    Path(args.snapshot).write_text(json.dumps(answers, ensure_ascii=False, indent=2))
    if args.public:
        suite = repo / 'tools/deploy/mysql_test/test_suite/fts_index'
        results = base / 'mysqltest-results'
        results.mkdir(exist_ok=True)
        sql("CREATE USER IF NOT EXISTS 'admin'@'%' IDENTIFIED BY ''; GRANT ALL PRIVILEGES ON *.* TO 'admin'@'%'; CREATE USER IF NOT EXISTS 'root@sys'@'%' IDENTIFIED BY ''; GRANT ALL PRIVILEGES ON *.* TO 'root@sys'@'%'")
        sql("CALL DBMS_SCHEDULER.DISABLE('ASYNC_GATHER_STATS_JOB_PROC')")
        public_status = {}
        for test_file in sorted((suite / 't').glob('*.test')):
            name = test_file.stem
            result = subprocess.run([str(repo / 'deps/3rd/u01/obclient/bin/mysqltest'),
                '--host=127.0.0.1', '--port=' + str(args.port), '--user=root', '--database=test',
                '--default-character-set=utf8mb4', '--test-file=' + str(test_file),
                '--result-file=' + str(suite / 'r/mysql' / (name + '.result')),
                '--logdir=' + str(results)], cwd=repo / 'tools/deploy', env=dict(os.environ, OBMYSQL_MS0='127.0.0.1',
                    OBMYSQL_PORT=str(args.port), OBMYSQL_PWD=''), capture_output=True, text=True, timeout=900)
            (results / (name + '.output')).write_text(result.stdout + result.stderr)
            print('PUBLIC', name, result.returncode, flush=True)
            public_status[name] = result.returncode
            (results / 'status.json').write_text(json.dumps(public_status, indent=2))
            if result.returncode:
                print(result.stdout + result.stderr, flush=True)
        if any(public_status.values()):
            raise AssertionError('Public test failures: ' + str(public_status))
Path(args.snapshot).write_text(json.dumps(answers, ensure_ascii=False, indent=2))
if args.compare:
    expected = json.loads(Path(args.compare).read_text())
    if len(expected) != len(answers):
        raise AssertionError('Snapshot case counts differ')
    for i, (old, new) in enumerate(zip(expected, answers)):
        if old != new:
            raise AssertionError(f'Case {i} differs: {new["sql"]}\nOLD {old["result"][:2000]}\nNEW {new["result"][:2000]}')
print(f'PASS {len(answers)} complete TOKENIZE/MATCH/DML results' + (' match reference' if args.compare else ' captured'), flush=True)
