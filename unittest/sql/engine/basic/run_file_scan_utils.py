#!/usr/bin/env python3
"""Build isolated native reader tests; PyArrow is optional and only generates fixtures."""
import argparse
import base64
import datetime
import itertools
import os
from pathlib import Path
import subprocess
import tempfile

parser=argparse.ArgumentParser()
parser.add_argument('--parquet-encodings', action='store_true', help='Generate independent Parquet fixtures with PyArrow')
parser.add_argument('--sanitize', action='store_true')
args=parser.parse_args()
repo=Path(__file__).resolve().parents[4]
temporary=tempfile.TemporaryDirectory(prefix='seekdb-file-tests-')
root=Path(temporary.name)
data=repo/'tools/deploy/mysql_test/test_suite/file_sql/data'
for name in ['basic.csv','basic.jsonl','invalid.txt']:
 (root/name).write_bytes((data/name).read_bytes())
(root/'simple.parquet').write_bytes(base64.b64decode((data/'simple.parquet.b64').read_bytes()))
base=[os.environ.get('CXX','g++'),'-std=c++17','-O1','-g','-I'+str(repo/'src'),'-I'+str(repo/'deps/3rd/usr/local/oceanbase/deps/devel/include')]
if args.sanitize:
 base+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
objects=[]
for name in ['ob_file_scan_utils','ob_file_parquet_reader']:
 obj=str(root/(name+'.o'))
 subprocess.run(base+['-c',str(repo/'src/sql/engine/basic'/f'{name}.cpp'),'-o',obj],check=True)
 objects.append(obj)
for driver in ['test_file_scan_utils','file_scan_probe']:
 subprocess.run(base+[str(repo/'unittest/sql/engine/basic'/f'{driver}.cpp')]+objects+['-o',str(root/driver)],check=True)
subprocess.run([str(root/'test_file_scan_utils'),str(root)],check=True)
# Corrupted/truncated inputs must fail cleanly, never overread or crash.
original=(root/'simple.parquet').read_bytes()
mutations=[original[:length] for length in [0,1,4,8,12,len(original)//2,len(original)-1]]
for offset in range(0,len(original),max(1,len(original)//32)):
 damaged=bytearray(original);damaged[offset]^=0xff;mutations.append(bytes(damaged))
for damaged in mutations:
 path=root/'damaged.parquet';path.write_bytes(damaged)
 result=subprocess.run([str(root/'file_scan_probe'),'parquet',str(path),str(root)],capture_output=True,text=True,timeout=5)
 assert result.returncode in (0,1),(result.returncode,result.stderr)
 assert 'AddressSanitizer' not in result.stderr and 'runtime error:' not in result.stderr,result.stderr
print('PASS malformed/truncated Parquet smoke checks',len(mutations),flush=True)
if not args.parquet_encodings:
 raise SystemExit(0)
import pyarrow as pa
import pyarrow.parquet as pq
columns={'id':pa.array([i if i%5 else None for i in range(1500)],type=pa.int64()),'name':pa.array(['alpha'+str(i%13) if i%7 else None for i in range(1500)]),'score':pa.array([i/8 if i%9 else None for i in range(1500)]),'flag':pa.array([bool(i%2) if i%11 else None for i in range(1500)]),'day':pa.array([datetime.date(2026,1,1)+datetime.timedelta(days=i%200) if i%17 else None for i in range(1500)]),'created':pa.array([datetime.datetime(2026,1,1)+datetime.timedelta(seconds=i,microseconds=i) if i%19 else None for i in range(1500)])}
table=pa.table(columns)
expected=[]
for item in table.to_pylist():
 row=[]
 for v in item.values():
  if v is None:row.append(None)
  elif isinstance(v,bool):row.append('true' if v else 'false')
  elif isinstance(v,datetime.datetime):row.append(v.strftime('%Y-%m-%d %H:%M:%S.%f'))
  elif isinstance(v,float):row.append(format(v,'.17g'))
  else:row.append(str(v))
 expected.append(row)
cases=[]
for compression,dictionary,version in itertools.product(['NONE','snappy'],[False,True],['1.0','2.0']):
 cases.append((f'{compression}-{dictionary}-{version}',dict(compression=compression,use_dictionary=dictionary,data_page_version=version)))
for encoding in ['DELTA_BINARY_PACKED','DELTA_LENGTH_BYTE_ARRAY','DELTA_BYTE_ARRAY','BYTE_STREAM_SPLIT']:
 column='id' if encoding=='DELTA_BINARY_PACKED' else 'score' if encoding=='BYTE_STREAM_SPLIT' else 'name'
 cases.append((encoding,dict(compression='snappy',use_dictionary=False,column_encoding={column:encoding})))
for name,kwargs in cases:
 path=root/(name+'.parquet');pq.write_table(table,path,row_group_size=333,data_page_size=1024,write_page_checksum=True,**kwargs)
 result=subprocess.run([str(root/'file_scan_probe'),'parquet',str(path),str(root)],text=True,capture_output=True)
 if result.returncode:raise RuntimeError(name+': '+result.stderr)
 actual=[[None if c=='NULL' else bytes.fromhex(c).decode() for c in row.split('\t')] for row in result.stdout.splitlines()]
 assert actual==expected,(name,next((i,a,e) for i,(a,e) in enumerate(zip(actual,expected)) if a!=e) if len(actual)==len(expected) else (len(actual),len(expected)))
 print('PASS',name,len(actual),'rows',flush=True)
print('All Parquet page/encoding/nullable/multi-row-group checks passed')
