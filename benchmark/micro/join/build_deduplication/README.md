# Join Build deduplication PoC microbenchmarks

These are ordinary DuckDB `.benchmark` files, registered in
`.github/regression/micro.csv` and executed by the existing Main → Regression →
Bench Micro CI job. No custom workflow, Python runner or runtime generation is
needed. Push the `join-dedup-bench` branch to the fork, or manually dispatch its
existing Main workflow with `duckdb_commit` set to the branch head. Manual
dispatch uses the same compatibility build selection as a push for this
experiment. The new correctness test is explicitly included in the smoke tests.

Each of 43 workloads has three files: `off`, `logical` and `physical`. Only the
43 `physical` files are registered for this measurement. Their
shared template sets the PoC mode in `init`, loads data outside the timed section,
executes the join, and checks its result using the standard benchmark runner.

The workloads cover:

- SEMI, ANTI and MARK joins, sweeping NDV from 10 to 2,000,000 at a fixed
  2,000,000 Build rows and 1,000,000 Probe rows.
- NDVs around the 16,384-slot cache capacity and the previous crossover regions.
- Cyclic, clustered, deterministically shuffled and skewed Build inputs with the
  same NDV. The skewed input starts with every distinct key and then favors zero.
- Build sizes of 200,000 and 20,000,000 at the same N/NDV ratio of 200, plus
  100,000 and 4,000,000 Probe rows at fixed Build size and NDV.
- Representative cases with the normal optimizer. Controlled cases disable
  Build-side swapping and perfect hash joins to isolate the general hash path.

Probe keys alternate matching and nonmatching values. All three modes must
produce the same independently specified count. `physical` is the current
bounded thread-local hash set with linear probing and growth. Keys are not
evicted on collisions. The set starts with up to 16,384 slots, grows to at most
262,144 slots, and keeps occupancy at or below one half. Stored hashes are reused
during growth. A per-worker memory budget also limits capacity and accounts for
the old and new allocations during growth. On reaching its capacity limit, the
filter releases its memory and passes subsequent rows to the normal Build. This
is not a globally unique Build table; cross-worker duplicates remain. Mode activation is
forced on eligible joins; there is no NDV heuristic yet.

The existing CI compares the old physical implementation (Base) against the
improved physical implementation (PR), using the same 43 workloads. Neither
logical nor off mode is timed in this run. The existing Regression matrix is
restricted to Micro for this experiment. The standard Micro job uses two threads
and a 6.5 GiB memory limit.

For this fork experiment only, Main's existing `OVERRIDE_JOBS` selects the
required release/relassert builds and regression jobs. Its `base_hash` pins the
comparison to `f7c2e9539daf25ab289c0d349caa3a58500653b2`, the previous physical PoC.
The Regression workflow honors an explicit base hash on fork pushes as well.
These experiment-specific CI selections should be removed before an upstream PR.

Results appear in GitHub Actions → Main → regression / Bench Micro → Run
regression benchmark. The log contains samples, per-query medians and the
comparison table. The existing regression criteria remain enabled, so an
experiment may report regressions or fail its aggregate gate while still
providing useful measurements. Inspect individual queries rather than relying
only on the aggregate result.
