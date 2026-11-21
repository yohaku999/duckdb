# Join Build deduplication PoC microbenchmarks

These are ordinary DuckDB `.benchmark` files, registered in
`.github/regression/micro.csv` and executed by the existing Main → Regression →
Bench Micro CI job. No custom workflow, Python runner or runtime generation is
needed. Push the `join-dedup-bench` branch to the fork to run the CI.

Each of 43 workloads has three files: `off`, `logical` and `physical`. Their
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
bounded thread-local cache, not a complete distinct set. Mode activation is
forced on eligible joins; there is no NDV heuristic yet.

The existing CI compares the Base and PR binaries for each file. Base has no PoC,
so all three modes use its original behavior. On the PR binary, each file enables
its named mode. Compare the three rows for the same workload to compare modes;
Base-to-PR deltas show each mode's change relative to the original implementation.
The standard Micro job uses two threads and a 6.5 GiB memory limit.

For this fork experiment only, Main's existing `OVERRIDE_JOBS` selects the
required release/relassert builds and regression jobs. Its `base_hash` pins the
comparison to `b3a028e6c89199f19dc18bb09899771473283624`, immediately before the
PoC. The Regression workflow honors an explicit base hash on fork pushes as well.
These experiment-specific CI selections should be removed before an upstream PR.

Results appear in GitHub Actions → Main → regression / Bench Micro → Run
regression benchmark. The log contains samples, per-query medians and the
comparison table. The existing regression criteria remain enabled, so an
experiment may report regressions or fail its aggregate gate while still
providing useful measurements. Inspect individual queries rather than relying
only on the aggregate result.
