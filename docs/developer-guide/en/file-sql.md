# Query local files with seekdb SQL

File SQL is disabled by default. An administrator must configure `secure_file_priv`
to a directory on the seekdb server. Clients can then enable the feature for their
session:

```sql
SET GLOBAL secure_file_priv = '/srv/seekdb-files';
-- Reconnect to inherit the global directory setting.
SET SESSION enable_file_sql = ON;
SELECT id, name FROM read_csv('/srv/seekdb-files/people.csv') WHERE id > 10;
SELECT * FROM read_json('/srv/seekdb-files/events.jsonl', format = 'newline_delimited');
SELECT COUNT(*), SUM(amount) FROM read_parquet('/srv/seekdb-files/payments.parquet');
DESCRIBE SELECT * FROM read_csv('/srv/seekdb-files/people.csv');
SELECT file FROM glob('/srv/seekdb-files/*.csv') ORDER BY file;
COPY (SELECT id, name FROM read_csv('/srv/seekdb-files/people.csv') ORDER BY id)
  TO '/srv/seekdb-files/export.csv' (FORMAT CSV, HEADER, NULLSTR 'NULL');
```

`read_ndjson` is an alias for `read_json`. CSV options are `header`, `delimiter`
(or `delim`), `quote`, `escape`, and `nullstr`. The default CSV header is enabled;
the delimiter is a comma, quoting and escaping use double quotes, and unquoted
empty fields are NULL. Quoted empty strings remain strings. When `quote` changes,
`escape` follows it unless supplied explicitly. JSONL accepts flat objects only.
CSV and JSONL infer their schema from the entire file; `sample_size` only accepts
`-1`. Column names and JSON keys must be unambiguous under case-insensitive lookup.
Repeated CSV header names receive `__2`, `__3`, etc. suffixes.

Inference supports signed BIGINT, finite DOUBLE, BOOLEAN, VARCHAR, DATE,
DATETIME, and all-NULL columns. Integer overflow is preserved as text. Integer
and floating-point values widen to DOUBLE; DATE and DATETIME widen to DATETIME;
other heterogeneous scalars widen to VARCHAR. JSON strings retain their string
type except valid ISO dates/timestamps. Missing JSON fields are NULL.

The resolver constructs dynamic columns for the native function-table operator.
Its bounded streaming reader supplies rows/batches to seekdb; projection,
expressions, filtering, sorting, aggregation and joins use the ordinary native
execution plan. No external SQL engine or runtime is involved. File-dependent
plans are not cached because files have no catalogue schema version.

Parquet reads the footer and projected columns one data page at a time, across
row groups. Supported page formats are V1 and V2, with uncompressed or Snappy
compression. Encodings include PLAIN, dictionary/RLE, DELTA_BINARY_PACKED,
DELTA_LENGTH_BYTE_ARRAY, DELTA_BYTE_ARRAY and BYTE_STREAM_SPLIT. Flat primitive
columns, nullable fields, DATE and millisecond/microsecond timestamps are
supported. Nested/repeated values, external column files, encryption and other
codecs/logical types fail explicitly. Page checksums are checked when present.
Format definitions: [Apache Parquet specification](https://parquet.apache.org/docs/file-format/).

Paths are canonicalized and checked against `secure_file_priv`; regular input
files are opened read-only using directory descriptors and `O_NOFOLLOW`.
Device/inode, size, modification and change timestamps are checked before and
after scans. A changed file fails the statement. Empty/NULL access directories
are denied. `glob` only discovers files and never combines their contents.

COPY runs the native SELECT INTO plan, writes a private temporary file in the
authorized destination directory and publishes it only after successful query
completion. It does not replace existing destinations. Closing a failed query
removes the temporary file. COPY defaults to no header; `HEADER` enables one.
CSV quoting preserves embedded newlines, delimiters, quotes, empty strings and
NULL text for subsequent `read_csv` calls.

Resource limits are 256 columns, 1 MiB per field, 8 MiB per CSV/JSONL record,
8 MiB Parquet footer, 16 MiB per Parquet page, 262,144 values per page and
128 MiB of retained Parquet page/dictionary buffers. Glob is limited to 10,000
matches/directories and 64 path components. Exceeding a limit fails explicitly.

## Validation

Run isolated native reader and security tests after dependency initialization:

```bash
python3 unittest/sql/engine/basic/run_file_scan_utils.py --sanitize
```

Optional independent Parquet fixture tests use PyArrow only to **write test
files**, with twelve encoding/compression/page-version combinations:

```bash
python3 unittest/sql/engine/basic/run_file_scan_utils.py --parquet-encodings --sanitize
```

On an isolated local seekdb server with an administrative account and its Unix
socket (required to change `secure_file_priv`):

```bash
SEEKDB_TEST_PORT=2888 SEEKDB_TEST_SOCKET=/path/to/instance/run/sql.sock \
  python3 unittest/sql/engine/basic/run_file_sql.py
```

The SQL runner stages local fixtures instead of running the public test's SSH
setup/cleanup scripts. It executes the public SQL and compares against the
unchanged expected result, then tests cross-format joins, rescan, aggregation,
security, COPY, schema refresh and a larger streaming scan. It temporarily changes
`secure_file_priv` and restores the previous setting. Do not run it against a
production instance.
