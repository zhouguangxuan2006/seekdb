# Full-text hot-path tuning

This document records the first shipped optimization. See the
[DAAT merge follow-up](fts-daat-merge-tuning.md) for the subsequent change and its
separate comparison against that shipped version.

## Scope and evidence

The reference is commit `8e0a7d05985f3a56bc3c32b47736acf3203116f8`, which
includes the AI function, file SQL and asynchronous AI-column work. Both binaries
use the same Release (`RelWithDebInfo`, `-O2`) build and dependencies.

CPU-clock sampling of the reference server at 499 Hz with frame-pointer call
stacks found repeated per-result expression and comparison work. Query self-sample
shares included `filter_on_demand` (3.90%), DOUBLE-to-BOOL conversion (2.96%),
bit-vector `set` (2.87%) and `at` (2.80%), `project_results` (2.18%),
`eval_one_datum_of_batch` (2.06%), `ObCharset::strcmpsp` (1.42%),
`ob_strnncollsp_binary` (1.15%) and `ObSRMergeCmp::get_id_datum` (0.96%).
The IK tokenization profile also contained `TCRWLock::wrlock` (1.44%). Source
inspection traced dictionary cache hits through an exclusive lock and found that
TOKENIZE serialized built-in defaults into JSON and immediately reparsed them.
These observations identify repeated work; their sample shares are not predicted
end-to-end speedups or a measurement of each change in isolation.

## Changes and semantic boundaries

* TOKENIZE without explicit parser properties consumes `ObFTParserProperty`'s
  typed defaults directly. It preserves the parser/name/charset checks, dictionary
  initialization, output options and validation of explicit properties. The existing
  three-argument helper initialization keeps its behavior; a separate overload
  supplies defaults. IK dictionary property identifiers remain identical.
* Dictionary loader cache hits acquire a read lock and retain the loader before
  releasing it. Cache misses acquire the write lock and recheck before creation,
  preventing duplicate creation by concurrent callers. Reference counts stay atomic.
* DAAT merge comparisons use an inline bytewise comparison only for non-NULL,
  fixed-size binary VARCHAR document IDs. Other types, collations, lengths and
  NULLs retain the original comparator. This also keeps sparse-vector IDs on their
  existing comparison path. The byte ordering, merge ordering and score addition
  order are unchanged.
* Vectorized DAAT retrieval directly applies DOUBLE truth conversion only to an
  exact `BOOL(relevance)` whose argument is the projected relevance expression.
  The nonzero test matches the existing evaluator; compound predicates and other
  types use the existing evaluator.
* Projection writes every output datum and sets evaluated bits and the projected
  flag once per batch, including partial final batches.

No posting lists, BM25 calculations, ranking work or required filters are skipped.
No benchmark data, SQL, timer, scoring script or fixed baseline is modified.

## Correctness validation

`unittest/storage/fts/run_fts_differential.py` captures full token JSON and complete
query results, including scores and errors. Both binaries agree on all **574**
results. Coverage includes five parsers for implicit/explicit defaults, non-default
and invalid options, changing parser/options across rows, concurrent first IK
lookups, mixed case, Unicode, embedded NUL, long tokens, charset fallback, NULLs,
both MATCH modes, predicates, ordering, limit/offset, joins, updates, deletes and
rollback. Randomized input uses a fixed seed, independent of the official data.

Both binaries pass all **15 `fts_index` mysqltests**, including DML,
index creation, online/offline DDL, index views, parser properties, partitioned
queries and Chinese tokenization. The broader run exposed an existing fixture
bug in `basic_dml`: after recreating a heap table it reused the old table ID and
expected obsolete rowkey/document mapping tables. The test now refreshes the ID,
asserts that the heap has no such mapping tables, and still checks its posting
columns. This behavior was verified on the unmodified reference binary. Two
matching bind-parameter numbers in a partitioned EXPLAIN expectation were also
synchronized with the reference plan; the plan operators and query results are
unchanged. These fixture repairs are separate from the kernel changes and do not change any
benchmark or performance expectation.

Additional unchanged suites pass on the candidate:

* Two sparse-vector suites: `sparse_vector_index_vsag_ddl` and
  `sparse_vector_index_vsag_query`.
* Five AI-column suites: DDL, DML, worker, retry/strict read and vector index.
* AI prompt mysqltest, AI prompt integration and AI-function enhancement.
* `file_sql_basic` and native file SQL integration (joins, aggregation,
  export/readback, file changes and large input).

Build/save the reference binary before applying the changes, then run from the
repository root using dedicated instance directories and free ports:

```bash
python3 unittest/storage/fts/run_fts_differential.py \
  --binary /absolute/path/seekdb-before --base-dir /tmp/fts-reference \
  --snapshot /tmp/fts-reference.json --public
python3 unittest/storage/fts/run_fts_differential.py \
  --binary build_release/src/observer/seekdb --base-dir /tmp/fts-candidate \
  --snapshot /tmp/fts-candidate.json --compare /tmp/fts-reference.json --public
```

The harness owns and stops its server and creates test databases/accounts there.
Never use an instance directory containing data to retain. The tests above cover
the changed paths and earlier competition features, not every seekdb subsystem.

## Local performance comparison

Environment: Intel Core i5-1240P, Ubuntu 22.04 under WSL2, 7.6 GiB VM RAM.
Both servers use `cpu_count=4`, `memory_limit=2G`, `datafile_size=2G` and
`log_disk_size=8G`. Each complete run uses a fresh instance and report, followed
by server shutdown. Both servers are bound to logical CPUs `0,2,4,6` (four separate
physical cores on this machine); the client is bound to CPU `8`. The supervisor
uses `0,2,4,6,8`, with a 15-second settling period after server readiness. No
compilation, other test server or profiler runs during the timed series.

The earlier query-only candidate had an overall **-1.13%** result in its own
three-pair series, so it did not establish an overall improvement. That series
bound client and server together to CPUs 0-3, including sibling hardware threads.
This revision adds tokenization/cache/comparison changes and uses separate client
and server cores for **both** versions. Conclusions below use only the new matched
series; they are not a comparison of timings across different configurations.
The earlier reports remain in the JSON artifact for transparency.

Each complete run uses the unmodified official benchmark with `ROWS=20000`,
`BATCH=500`, `ROUNDS=3000`, `QUERY_ROUNDS=200`, `SAMPLES=3`, `WARMUP=30`,
`SKIP_LOAD=0`. The sixteen runs follow two blocks of four pairs, each ordered:
reference/candidate, candidate/reference, candidate/reference, reference/candidate.
All nine metrics and all sixteen runs are included, including individual regressions.
Build times are seconds; tokenization and query times are milliseconds. Values
below are arithmetic means of eight complete runs per binary.

| Metric | Reference | Candidate | Time reduction |
| --- | ---: | ---: | ---: |
| `build_ik_all_sec` | 10.2043 | 10.1583 | +0.45% |
| `build_ik_content_sec` | 9.6729 | 9.9727 | -3.10% |
| `build_beng_en_sec` | 5.5116 | 5.3037 | +3.77% |
| `tokenize_ik_avg_ms` | 0.3036 | 0.2657 | +12.50% |
| `tokenize_beng_avg_ms` | 0.2034 | 0.2050 | -0.78% |
| `query_cn_avg_ms` | 2.6441 | 2.2384 | +15.34% |
| `query_beng_avg_ms` | 3.6639 | 2.9360 | +19.87% |
| `query_mixed_avg_ms` | 3.0173 | 2.7226 | +9.77% |
| `query_limit_avg_ms` | 3.5348 | 3.2737 | +7.39% |

| Category | Mean time reduction |
| --- | ---: |
| Build | +0.37% |
| Tokenize | +5.86% |
| Query | +13.09% |
| Three-category equal-weight mean | +6.44% |

Paired mean: **+5.91%**; approximate 95% confidence interval: **[+0.86%, +10.97%]**.

| Pair | Build | Tokenize | Query | Equal-weight overall |
| --- | ---: | ---: | ---: | ---: |
| 1 | -7.88% | +1.49% | +3.42% | -0.99% |
| 2 | +1.83% | +4.84% | +21.04% | +9.24% |
| 3 | +2.48% | +16.34% | +17.59% | +12.14% |
| 4 | -6.52% | +21.45% | +16.95% | +10.63% |
| 5 | +2.22% | +14.39% | +10.23% | +8.95% |
| 6 | -1.03% | +7.61% | +14.76% | +7.12% |
| 7 | +6.60% | -29.97% | +7.50% | -5.29% |
| 8 | +2.39% | +2.49% | +11.69% | +5.52% |

Each metric uses `(reference - candidate) / reference`, followed by an arithmetic
mean within each category and an equal-weight mean of the three categories.
Negative improvements are not clamped. Six pairs improved; pairs 1 and 7 regressed
and remain included. The initial two pairs showed mixed results, so the series
was extended to a fixed total of eight pairs without
changing the candidate, test parameters or configuration. The paired improvement
mean and a two-sided 95% Student t interval (7 degrees of freedom, critical value
2.365) provide an additional view of measurement variability. This interval concerns
the mean of the eight per-pair category-weighted improvements; the table above
uses category-weighted ratios of mean timings. It is a local estimate under the
usual paired-sample assumptions, not a guarantee for the official CI environment.

[All sixteen reports, configuration, binary hashes, pair results and calculations](fts-performance-runs.json)
are included for inspection. The official report's `git_head`/`git_dirty` fields
refer to the benchmark-script working tree, which is shared by both runs; the
recorded binary hashes identify the actual reference and candidate executables.
Every run returned 8,001 Chinese hits, 11,000 BENG hits, 7,332 mixed hits and 20 LIMIT rows; full-result correctness is checked by the
separate regression suite. Host scheduling and I/O variation remain possible on
this shared laptop. The competition uses its fixed baseline and environment;
these local improvements do not predict the official score or establish full marks.

To reproduce one complete measurement, start a dedicated server on CPUs 0,2,4,6
with the resource settings above, wait for readiness and the settling interval,
then run from the repository root:

```bash
export MYSQL='taskset -c 8 mysql -h127.0.0.1 -P2891 -uroot -N -s -A --default-character-set=utf8mb4'
export MYSQL_VERBOSE='taskset -c 8 mysql -h127.0.0.1 -P2891 -uroot -A --default-character-set=utf8mb4'
REPORT_FILE=$(mktemp /tmp/fts-report.XXXXXX)
LABEL=local ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 \
  SAMPLES=3 WARMUP=30 SKIP_LOAD=0 OUTPUT="$REPORT_FILE" \
  taskset -c 0,2,4,6,8 bash tools/benchmark/fts_large_bench.sh
python3 tools/benchmark/fts_large_bench_score.py "$REPORT_FILE"
```

The benchmark drops and recreates `fts_large_bench`; use only a dedicated test
instance. Adapt CPU numbers to the host topology identically for both binaries.
Keep compilation and profilers out of timed runs, use fresh report files, and
retain every result when calculating the category averages.
