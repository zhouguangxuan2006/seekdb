# DAAT merge follow-up

This revision is measured against the last shipped commit
`3d8e1d9cd36c324101b826f35ab3a77d621b5046`. The original optimization and its
separate measurements remain in [Full-text hot-path tuning](fts-performance-tuning.md).

## Bottleneck and implementation

CPU sampling of the reference query path attributed 6.48% of self samples to the
small merger's `push`, 1.77% to `pop`, 3.41% to `fill_merge_heap` and 2.63% to
`collect_dims_by_id`. Container wrappers also appeared in the call stacks.
These observations identify repeated per-posting work; sample shares do not
predict the end-to-end improvement.

The revision has two changes confined to the DAAT retrieval implementation:

* Access existing contiguous arrays directly in the merge, comparison, buffering
  and projection loops. Initialization allocates ID/next-iterator arrays for the
  dimension count, validates optional weight count, and allocates result buffers
  for the maximum batch. Index/count assertions document these invariants; the
  runtime result-buffer capacity check remains. No pointer is retained across
  a resize or reuse, and memory ownership is unchanged.
* Allocate the existing small merger through a concrete final leaf type, allowing
  its operations to inline in the fill/collect loops. The heap algorithm, comparison
  rules and tie handling are unchanged. A flag records the actual allocated heap
  type and survives iterator reuse; it is not inferred from a potentially changed
  dimension count. Larger fan-ins continue through the existing loser tree.

Score accumulation order, filters, LIMIT/OFFSET and output datum writes remain
unchanged. No postings or required work are skipped. The benchmark data, SQL,
timers, scoring script, fixed baseline and CI workflow are unchanged.

## Correctness

* All **678** complete differential results match the reference: token JSON,
  document IDs, full-precision relevance scores, errors, updates and rollback.
  Coverage includes small/large fan-ins, repeated/missing terms, Boolean queries,
  joins and ordinary SQL ordering across NULLs, collations and several data types.
* All **15** public `fts_index` mysqltests pass.
* Both sparse-vector DDL/query suites pass, exercising the shared retrieval path.
* All **5** AI-column suites pass, including DDL/DML, background workers,
  retry/strict reads, vector indexes and rollback.
* AI prompt mysqltest, prompt integration, AI-function enhancement, file SQL
  public test and native file SQL integration all pass.

These are affected-path and previous-feature regressions, not every test in the
repository. The artifact records exact binary hashes and suite outcomes. Use the
differential commands in the original tuning document with the updated harness
and the reference above to reproduce the correctness comparison.

## Measurement and confirmation

The first fixed 16-pair series had a positive mean but a paired 95% interval
crossing zero. It did not qualify for publication on its own. A separate
**64-pair confirmation was fixed before it began**, reducing the expected
interval half-width from about 7.8 to 3.6 percentage points based on the initial
variation. There was no early stopping, removed pair or selective repeat.
Acceptance requires positive three-category means and paired 95% intervals
entirely above zero both in the independent confirmation and in all 80 pairs
combined.
Code, binary hashes and resource settings stayed unchanged between phases.

Each phase has two declared complete preconditioning runs (reference then
candidate), retained separately, followed by its measured pairs. Pair order
repeats reference/candidate, candidate/reference, candidate/reference,
reference/candidate. Both builds use RelWithDebInfo with -O2.

Every run uses the unchanged official script with 20,000 rows, batch 500, 3,000
tokenization rounds, 200 query rounds, 3 samples, 30 warmups and SKIP_LOAD=0.
Each has a fresh instance/report and 15 seconds settling after readiness.
Dictionary initialization, data loading and index construction still occur in
every measured run. No compilation, profiler or second test server runs during
timing. Preconditioning is intended to reduce cold-host effects.

Host: Intel Core i5-1240P, Ubuntu 22.04 on WSL2, 7.6 GiB VM RAM. Client and
server are both bound to logical CPU 0. Server settings: cpu_count=4,
memory_limit=2G, datafile_size=2G, log_disk_size=8G, WARN logging. Affinity
constrains build parallelism equally for both versions; this is a controlled
local comparison, not multi-core CI. Windows power settings were not changed.

A reference-only placement diagnostic measured SELECT 1 at 0.112-0.121 ms with
separated client/server CPUs and 0.026-0.028 ms with both on CPU 0. Placement is
not counted as a kernel improvement: every before/after comparison uses the
same placement. Earlier array-only and wider candidates, including regressions
and an interrupted series, remain in the raw artifact and are not pooled with
this revision.

## Results

| Phase | Pairs | Build | Tokenize | Query | Equal-weight overall | Paired mean | Paired 95% interval |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| Initial | 16 | +1.84% | +2.02% | +11.88% | +5.25% | +4.05% | [-3.72%, +11.81%] |
| Confirmation | 64 | -0.61% | -1.38% | +13.02% | +3.68% | +3.16% | [+0.86%, +5.45%] |
| Combined | 80 | -0.05% | -0.60% | +12.78% | +4.04% | +3.33% | [+1.03%, +5.64%] |

All 80 runs of each version are averaged below. Build times are seconds;
tokenization and query times are milliseconds.

| Metric | Reference | Candidate | Reduction |
| --- | ---: | ---: | ---: |
| `build_ik_all_sec` | 11.2489 | 11.3062 | -0.51% |
| `build_ik_content_sec` | 9.2577 | 9.3200 | -0.67% |
| `build_beng_en_sec` | 5.5303 | 5.4729 | +1.04% |
| `tokenize_ik_avg_ms` | 0.0970 | 0.0973 | -0.39% |
| `tokenize_beng_avg_ms` | 0.0528 | 0.0532 | -0.82% |
| `query_cn_avg_ms` | 1.6825 | 1.4202 | +15.59% |
| `query_beng_avg_ms` | 2.4322 | 2.0699 | +14.90% |
| `query_mixed_avg_ms` | 1.8611 | 1.5928 | +14.42% |
| `query_limit_avg_ms` | 2.6264 | 2.4636 | +6.20% |

| Confirmation pairs, in execution order | Paired mean reduction |
| --- | ---: |
| 1-8 | +4.42% |
| 9-16 | +2.75% |
| 17-24 | +4.27% |
| 25-32 | +4.70% |
| 33-40 | +4.84% |
| 41-48 | +3.71% |
| 49-56 | -2.35% |
| 57-64 | +2.90% |

Positive pairs: 12/16 initially, 55/64 in confirmation, 67/80 combined.

Confirmation pair 51 had the largest overall regression (-59.77%) and remains included. Its build, tokenization
and several query timings all increased; neither run recorded swap I/O.
The cause is unconfirmed. The negative 49-56 block is retained as well.

Metric/category tables use ratios of mean timings. Improvements are averaged
inside each category, then the three category means receive equal weight.
Regressions remain negative. Paired intervals use individual pair improvements
and Student t with 15, 63 and 79 degrees of freedom. The combined interval is
descriptive; confirmation is the independent check after the uncertain initial
result. These local estimates assume independent paired samples and cannot
establish performance on other hardware or predict the competition score.

This patch changes retrieval, not a separate tokenizer/build algorithm.
Variations in those timings count in the overall result but should not be
attributed to another algorithmic optimization. Every run returned 8,001 Chinese
hits, 11,000 English hits, 7,332 mixed hits and 20 LIMIT rows. Complete token
output, IDs and scores were checked separately by regression tests.

[Raw reports, protocols, hashes, patch, resources and calculations](fts-daat-merge-runs.json)
retain every measured pair, all preconditioning reports and previous unsuccessful
experiments. Report git_head/git_dirty describe the shared script worktree;
binary SHA-256 identifies the executable actually measured.

## Cleanup and reproduction

Each owned server is stopped and its disposable database directory removed after
its run. Correctness outputs are archived before removing test instances. Compact
reports remain; database files do not accumulate.

Follow the original tuning document's build and differential-test instructions
with the reference commit above. Bind client/server to CPU 0 and use the exact
resources, official parameters and phase orders given here. External orchestration
scripts are included in the raw artifact. Keep both versions identical in setup
and retain all runs. Use dedicated instances: the benchmark recreates its schema.
