#!/usr/bin/env python3
"""Run the unchanged public SQL cases and local integration checks on an isolated server."""
import base64
import os
from pathlib import Path
import subprocess
import tempfile

repo=Path(__file__).resolve().parents[4]
port=os.environ.get('SEEKDB_TEST_PORT','2888')
mysql=['mysql','-h','127.0.0.1','-P',port,'-u','root','--default-character-set=utf8mb4','--batch','--raw','--skip-column-names']
def sql(text, *, enabled=True, fail=False, administrative=False):
 command=('SET SESSION enable_file_sql=ON; ' if enabled else '')+text
 client=mysql
 if administrative:
  socket=os.environ['SEEKDB_TEST_SOCKET']
  client=['mysql','--protocol=SOCKET','--socket='+socket,'-uroot','--batch','--raw','--skip-column-names']
 result=subprocess.run(client+['-e',command],text=True,capture_output=True)
 if fail:
  assert result.returncode!=0,(text,result.stdout)
 else:
  assert result.returncode==0,(text,result.stderr)
 return result.stdout.strip()
def expect(text, expected):
 actual=sql(text)
 assert actual==expected,(text,actual,expected)
 print('PASS',text[:130],flush=True)

with tempfile.TemporaryDirectory(prefix='seekdb-file-sql-') as temporary:
 root=Path(temporary)
 data=root/'data';data.mkdir()
 fixtures=repo/'tools/deploy/mysql_test/test_suite/file_sql/data'
 for name in ['basic.csv','basic.jsonl','invalid.txt']:
  (data/name).write_bytes((fixtures/name).read_bytes())
 (data/'simple.parquet').write_bytes(base64.b64decode((fixtures/'simple.parquet.b64').read_bytes()))
 old=sql('SELECT @@global.secure_file_priv',enabled=False)
 sql("SET GLOBAL secure_file_priv='"+str(root)+"'",enabled=False,administrative=True)
 try:
  sql('CREATE DATABASE IF NOT EXISTS file_sql_regression',enabled=False)
  suite=repo/'tools/deploy/mysql_test/test_suite/file_sql'
  source=(suite/'t/file_sql_basic.test').read_text()
  lines=[]
  for line in source.splitlines():
   if line.startswith('--source '):continue
   if line.startswith('let $FILE_SQL_DATA_DIR ='):line='let $FILE_SQL_DATA_DIR = '+str(data)+';'
   lines.append(line)
  test=root/'file_sql_basic.test';test.write_text('\n'.join(lines)+'\n')
  result=subprocess.run([str(repo/'deps/3rd/u01/obclient/bin/mysqltest'),'--host=127.0.0.1','--port='+port,'--user=root','--database=file_sql_regression','--default-character-set=utf8mb4','--test-file='+str(test),'--result-file='+str(suite/'r/mysql/file_sql_basic.result'),'--logdir='+str(root)],text=True,capture_output=True)
  print(result.stdout,result.stderr,flush=True)
  if result.returncode:
   for reject in root.glob('*.reject'):print(reject.read_text(),flush=True)
   raise AssertionError('Public file_sql_basic comparison failed')
  print('PASS unchanged public file_sql_basic test/result',flush=True)
  csv=f"read_csv('{data}/basic.csv')"
  js=f"read_json('{data}/basic.jsonl')"
  pq=f"read_parquet('{data}/simple.parquet')"
  expect(f'SELECT a.id,b.name FROM {csv} a INNER JOIN {js} b ON a.id=b.id WHERE a.id<=2 ORDER BY a.id','1\tAlice\n2\tBob')
  expect(f'SELECT a.id,b.name FROM {csv} a LEFT JOIN {js} b ON a.id=b.id AND b.id<3 ORDER BY a.id','1\tAlice\n2\tBob\n3\tNULL')
  expect(f'SELECT a.id,p.c1 FROM {csv} a JOIN {pq} p ON a.id=p.c1 ORDER BY a.id','1\t1\n2\t2\n3\t3')
  expect(f'SELECT /*+ USE_NL(b) */ a.id,b.id FROM {csv} a JOIN {js} b ON a.id<b.id ORDER BY a.id,b.id','1\t2\n1\t3\n2\t3')
  expect(f'SELECT active,COUNT(*),SUM(id),MIN(id),MAX(id) FROM {csv} GROUP BY active HAVING COUNT(*)>1','1\t2\t4\t1\t3')
  expect(f'SELECT id+10 AS shifted FROM {csv} WHERE score>80 ORDER BY score DESC LIMIT 2','11\n13')
  expect(f"SELECT COUNT(*) FROM read_ndjson('{data}/basic.jsonl')",'3')
  expect(f'SELECT COUNT(*) FROM {pq}','7')
  expect(f'SELECT COUNT(*) FROM {csv} WHERE 1=0','0')
  expect('SELECT 1+2,UPPER(\'normal\')','3\tNORMAL')
  sql(f'SET SESSION enable_file_sql=OFF; SELECT * FROM {csv}',fail=True)
  sql("SELECT * FROM read_csv('/etc/passwd')",fail=True)
  (root/'outside.csv').symlink_to('/etc/passwd')
  sql(f"SELECT * FROM read_csv('{root}/outside.csv')",fail=True)
  sql(f"SELECT * FROM read_csv('{data}/../../etc/passwd')",fail=True)
  sql(f"SELECT * FROM read_csv('{data}/basic.csv', sample_size=5)",fail=True)
  sql(f"SELECT * FROM read_csv('{data}/basic.csv', bogus=true)",fail=True)
  sql(f"SELECT * FROM read_json('{data}/basic.jsonl', sample_size=10)",fail=True)
  (root/'bad.jsonl').write_text('{"x":{"nested":1}}\n')
  sql(f"SELECT * FROM read_json('{root}/bad.jsonl')",fail=True)
  (root/'bad.csv').write_text('a,b\n1,2,3\n')
  sql(f"SELECT * FROM read_csv('{root}/bad.csv')",fail=True)
  target=data/'export.csv';before=target.read_bytes()
  sql(f"COPY (SELECT 999 AS id) TO '{target}' (HEADER)",fail=True)
  assert target.read_bytes()==before
  sql('DROP TABLE IF EXISTS file_sql_regression.export_table; CREATE TABLE file_sql_regression.export_table (id BIGINT, note VARCHAR(200)); INSERT INTO file_sql_regression.export_table VALUES (1,\'x,y\'),(2,NULL)')
  table_csv=root/'table.csv'
  sql(f"COPY file_sql_regression.export_table TO '{table_csv}' (FORMAT CSV, HEADER, NULLSTR 'NULL')")
  expect(f"SELECT id,note FROM read_csv('{table_csv}',nullstr='NULL') ORDER BY id",'1\tx,y\n2\tNULL')
  sql('DROP TABLE file_sql_regression.export_table')
  bad_source=root/'runtime-error.csv';bad_source.write_text('payload\n"{""x"":1}"\ninvalid\n')
  failed=root/'failed.csv'
  sql(f"COPY (SELECT JSON_EXTRACT(payload,'$.x') FROM read_csv('{bad_source}')) TO '{failed}' (HEADER)",fail=True)
  assert not failed.exists()
  assert not list(root.glob('.seekdb-copy-*.tmp'))
  changing=root/'changing.csv';changing.write_text('id\n1\n')
  query=f"SELECT COUNT(*) FROM read_csv('{changing}')"
  expect(query,'1');changing.write_text('id\n1\n2\n');expect(query,'2')
  big=root/'large.csv'
  with big.open('w') as output:
   output.write('id,value\n')
   for i in range(100000):output.write(f'{i},{i%7}\n')
  expect(f"SELECT COUNT(*),SUM(id) FROM read_csv('{big}')",'100000\t4999950000')
  print('All native file SQL integration checks passed',flush=True)
 finally:
  value='NULL' if old=='NULL' else "'"+old.replace("'","''")+"'"
  sql('SET GLOBAL secure_file_priv='+value,enabled=False,administrative=True)
