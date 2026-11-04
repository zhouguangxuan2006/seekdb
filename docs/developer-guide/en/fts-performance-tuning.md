# Full-text hot-path tuning

## Scope and evidence

The reference is commit `8e0a7d05985f3a56bc3c32b47736acf3203116f8`, which
includes the AI function, file SQL and asynchronous AI-column work. Both binaries
use the same Release (`RelWithDebInfo`, `-O2`) build and dependencies. This change
optimizes the document-at-a-time (DAAT) query path; it leaves index construction,
tokenizers, dictionaries, scoring formulas and storage formats unchanged.

CPU-clock sampling of the reference server at 499 Hz with frame-pointer call
stacks identified repeated per-result expression work. Self-sample shares included
`filter_on_demand` (3.90%), DOUBLE-to-BOOL conversion (2.96%), bit-vector `set`
(2.87%) and `at` (2.80%), `project_results` (2.18%), and
`eval_one_datum_of_batch` (2.06%). These are samples of that workload, not predicted
end-to-end speedups.

## Changes and semantic boundaries

Vectorized DAAT retrieval directly applies DOUBLE truth conversion only to an
exact `BOOL(relevance)` expression whose argument is the projected relevance
expression. Relevance has already been calculated and materializing it for a
scalar expression evaluation would repeat work for every matching document.
The direct conversion uses the same nonzero test as the existing evaluator,
including zero, negative zero and non-finite DOUBLE values. Compound predicates,
other types and non-vectorized execution continue through the existing evaluator.

Projection still writes every output datum. It sets the evaluated bit-vector and
projected flag once per completed batch rather than setting them for every row.
The bit-vector range operation preserves the same prefix of evaluated rows,
including partial final batches. The existing batch scope guard remains in place.

No posting lists, BM25 computations, ranking work or required filters are skipped.
No benchmark data, SQL, timer, scoring script or fixed baseline is modified.

## Correctness validation

`unittest/storage/fts/run_fts_differential.py` captures full token JSON and complete
query results, including scores. It uses a fixed random seed rather than benchmark
input or expected-answer special cases. The reference and optimized binaries
agree on all 544 captured results, including typed NULL, punctuation, mixed case,
Unicode, embedded NUL, long input, four parsers, charset fallback, both MATCH modes,
filters, ordering, limit/offset, joins, update/delete and rollback. Unsupported
syntax and charset cases also compare the returned errors.

Both binaries pass the unchanged `fts_index` mysqltests `tokenize_function`,
`domain_id_merge_with_compare_null`, and `simple_query`.

Build/save the reference binary before applying the changes, then run from the
repository root (use dedicated instance directories and free ports):

```bash
python3 unittest/storage/fts/run_fts_differential.py \
  --binary /absolute/path/seekdb-before --base-dir /tmp/fts-reference \
  --snapshot /tmp/fts-reference.json --public
python3 unittest/storage/fts/run_fts_differential.py \
  --binary build_release/src/observer/seekdb --base-dir /tmp/fts-candidate \
  --snapshot /tmp/fts-candidate.json --compare /tmp/fts-reference.json --public
```

The harness owns and stops its server. It recreates `fts_regression`; public tests
also use the `test` database in that isolated instance. Never point the instance
directory at a deployment containing data to retain.

Additional unchanged suites passed on the optimized binary:

* `vector_index/sparse_vector_index_vsag_ddl` and `sparse_vector_index_vsag_query`.
* All five AI-column mysqltests: DDL, DML, worker, retry/strict read and vector index.
* AI prompt mysqltest, AI prompt integration and AI-function-enhancement suite.
* `file_sql_basic` and the native file SQL integration checks (including joins,
  aggregation, export/readback, file changes and large input).

No reference result file was updated to make these suites pass.

## Local performance comparison

Environment: Intel Core i5-1240P, x86_64, Ubuntu 22.04 under WSL2, 7.6 GiB VM RAM.
Each server uses `cpu_count=4`, `memory_limit=2G`, `datafile_size=2G` and
`log_disk_size=8G`. Both binaries run on the same machine, sequentially, with fresh
instance directories and a fresh report file for every complete run.

Each run executes the unchanged official benchmark with `ROWS=20000`, `BATCH=500`,
`ROUNDS=3000`, `QUERY_ROUNDS=200`, `SAMPLES=3`, `WARMUP=30`, `SKIP_LOAD=0`.
For the final comparison, the supervisor, server and client inherit CPU affinity
0-3, with a 15-second settling period after server readiness. Runs are sequential
in the order reference/candidate, candidate/reference, reference/candidate.
Only reports for the final source change are used below.

All values below are arithmetic means of three complete runs per binary. No
individual metric or run is discarded; a negative reduction means a regression.
Build times are seconds; tokenization and query times are milliseconds.

| Metric | Reference | Candidate | Time reduction |
| --- | ---: | ---: | ---: |
| `build_ik_all_sec` | 17.8670 | 15.8363 | +11.37% |
| `build_ik_content_sec` | 12.6200 | 11.6690 | +7.54% |
| `build_beng_en_sec` | 6.8637 | 7.3273 | -6.76% |
| `tokenize_ik_avg_ms` | 0.3801 | 0.4691 | -23.41% |
| `tokenize_beng_avg_ms` | 0.2860 | 0.3045 | -6.44% |
| `query_cn_avg_ms` | 3.1951 | 3.0350 | +5.01% |
| `query_beng_avg_ms` | 5.2465 | 4.4907 | +14.40% |
| `query_mixed_avg_ms` | 3.7418 | 3.6950 | +1.25% |
| `query_limit_avg_ms` | 4.9606 | 4.4974 | +9.34% |

| Category | Mean time reduction |
| --- | ---: |
| Build | +4.05% |
| Tokenize | -14.93% |
| Query | +7.50% |
| Three-category equal-weight mean | -1.13% |

The category calculation first takes `(reference - candidate) / reference`
for each metric, averages within each category, then averages the three categories.
It does not average all nine metrics together or clamp individual regressions.

[All six reports, resource configuration, binary hashes and calculations](fts-performance-runs.json)
are included for inspection. This is a shared laptop, so host scheduling and I/O
variation remain relevant even with CPU affinity. Index construction and tokenization
have no source changes in this candidate; their measured variation is still included
in the overall result. Do not attribute all differences in those categories to this
query optimization. The measured results support only this local comparison.

Every run returned 8,001 Chinese hits, 11,000 BENG hits, 7,332 mixed hits and 20
LIMIT rows. Full-result correctness is checked separately by the regression suite.


These measurements are local comparisons against our reference binary. The
competition score uses its own fixed baseline and environment; neither a local
speedup nor the local invocation of the official score script predicts that score.
The local fixed-baseline script reports 100.00, 100.00 and 98.02 out of 100 for
the candidate, and 100.00 for all three reference runs. These local scores reflect
the fixed baseline and this machine; they do not establish a gain from this patch
or a formal competition result. The task score is out of 50, while the performance
report score is out of 100.

To reproduce one complete measurement against an already running dedicated server:

```bash
# Apply the same affinity to the server and client for the local comparison.
# Start both versions with the same resource configuration described above.
export MYSQL='mysql -h127.0.0.1 -P2891 -uroot -N -s -A --default-character-set=utf8mb4'
export MYSQL_VERBOSE='mysql -h127.0.0.1 -P2891 -uroot -A --default-character-set=utf8mb4'
REPORT_FILE=$(mktemp /tmp/fts-report.XXXXXX)
LABEL=local ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 \
  SAMPLES=3 WARMUP=30 SKIP_LOAD=0 OUTPUT="$REPORT_FILE" \
  taskset -c 0-3 bash tools/benchmark/fts_large_bench.sh
python3 tools/benchmark/fts_large_bench_score.py "$REPORT_FILE"
```

This benchmark drops and recreates `fts_large_bench`. Run it only on a dedicated
test instance. Keep compilation, profilers and other benchmarks out of the timed
runs. Preserve all reports, including regressions; category averages must include
negative improvements and the three categories have equal weight.
