# Asynchronous AI columns

AI columns use native schema metadata, DML, internal transactional SQL and the
AI service worker. Model requests run after the business transaction commits.
No external SQL engine or application-side scheduler is involved.

## Transaction and scheduling model

CREATE TABLE and ALTER TABLE ADD/MODIFY validate the expression, dependencies, model
kind, result type and async options. Definitions are stored in column metadata
and `__all_ai_column`; three hidden columns hold state, request identity and the
result specification. SHOW AI COLUMNS and SHOW CREATE TABLE expose definitions.
Users cannot directly assign an AI result column.

INSERT and dependency/primary-key UPDATE capture the expression arguments without
calling the model. They invalidate results and create durable
`__all_ai_column_task` rows in the business transaction through its nested SQL
connection. Unrelated updates leave results alone. DELETE removes pending tasks;
rollback restores both data and tasks. A request identity groups the AI columns
triggered by one row change.

Four AI-service workers claim committed tasks using transactions, leases and
fencing epochs. Calls use the existing AI model/endpoint implementation outside
the claim transaction. Typed system instructions preserve the user input. Output
validation checks JSON, numeric bounds, UTF-8, string length and vector dimensions.
Failures have bounded retries and a final FAILED state. Startup recovers abandoned
claims; completed groups can be published after interruption.

A separate transaction publishes terminal results and statuses for a group.
Publication locks the business row before task rows, matching the DML lock order.
Writeback checks definition versions and current row request identities. It only
updates existing rows, preventing deleted-row resurrection and stale-result
overwrite. Native UPDATE maintains ordinary and vector indexes. Failed results
are NULL, but a separate state distinguishes them from PENDING and READY.
Dependency and RETRY updates participate in native vector-ID generation.
Generated expressions depending on invalidated AI results are re-evaluated before
index maintenance, including within transactions that later roll back.

ALTER TABLE RETRY AI COLUMN lowers to native UPDATE with the original predicate.
Only the requested column is invalidated. Unquoted `status` is the AI-state
pseudo-column; quoted `status` remains a business column. The
`ai_column_strict_read` session variable checks the entirety of each referenced
AI column at execution time, including when using a cached plan. Ordinary-column
queries are unaffected.

Adding a column backfills committed existing rows in bounded batches through
native RETRY DML. MODIFY retains hidden-column identities, invalidates results
and replaces metadata/tasks in the DDL transaction. Definition identities remain
stable across unrelated schema changes. DROP removes the definition, hidden
columns and tasks, and rejects dropping live dependencies. Tables without an
explicit primary key enqueue inserts after the native hidden key is allocated.

## Validation entry points

After configuring and building seekdb on Linux:

```sh
python3 unittest/sql/parser/run_ai_column_parser.py
python3 unittest/share/schema/run_ai_column_schema.py
python3 unittest/sql/engine/expr/run_ai_column_protocol.py
python3 unittest/sql/resolver/run_ai_column_ddl.py --base-dir /tmp/seekdb-ai-tests
python3 unittest/sql/resolver/run_ai_column_transaction.py --base-dir /tmp/seekdb-ai-tests
python3 unittest/sql/resolver/run_ai_column_runtime.py --base-dir /tmp/seekdb-ai-tests
python3 unittest/sql/resolver/run_ai_column_vector_transitions.py --base-dir /tmp/seekdb-ai-tests
python3 unittest/sql/resolver/run_ai_column_schema_changes.py --base-dir /tmp/seekdb-ai-tests
python3 unittest/sql/resolver/run_ai_column_mysqltest.py --base-dir /tmp/seekdb-ai-tests
```

The integration runners require an unused port (default 2890) and a dedicated
data directory. They start and stop their own server. Runtime tests use the local
mock service, with no real model credentials. The original mysqltest suites are
under `tools/deploy/mysql_test/test_suite/ai_column/`: DDL, transactional DML,
worker/retry ordering, conditional retry/strict reads, and vector indexes.

## Scope and limits

The implementation targets the competition's single-process MySQL-mode async
scenario. It bounds expression size, captured input, output and group sizes.
Model transport can retry an ambiguous request after response loss; inference
is not guaranteed exactly once.

New system tables and metadata columns currently require fresh-instance
bootstrap. An upgrade of an existing data directory is not implemented.

## Local validation record

Validation on 2026-11-04, Ubuntu 22.04 x86_64:

- Release build passed.
- Native parser/definition, schema serialization and task protocol tests passed.
- All five original AI-column mysqltest suites passed their complete result
  comparisons, including vector-index EXPLAIN. The public test scripts and
  expected results were not modified.
- Extended DDL and transaction checks passed: persistence after restart, task
  grouping, rollback, primary-key changes, deletion and assignment protection.
- Runtime checks passed: savepoints, failed-statement rollback, large text/JSON
  inputs, cached strict reads, output validation and abandoned-lease recovery.
- Schema lifecycle checks passed: ADD backfill, unrelated schema edits, MODIFY
  during an old request, DROP cleanup, hidden primary keys, vector definition
  replacement, repeated dependency updates, conditional retry and rollback.
- Ordinary vector NULL/regeneration, ANN and rollback regression passed.
- Previous AI-function enhancement and file-SQL regressions passed.

The mysqltest runner records per-suite status and complete output under the test
instance's `mysqltest-results` directory. These local results do not establish a
hidden-test score.
