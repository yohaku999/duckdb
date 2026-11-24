# Join Build deduplication PoC microbenchmarks

These are ordinary DuckDB `.benchmark` files, registered in
`.github/regression/micro.csv` and executed by the existing Main → Regression →
Bench Micro CI job. No custom workflow, Python runner or runtime generation is
needed. Push the `join-dedup-bench` branch to the fork, or manually dispatch its
existing Main workflow with `duckdb_commit` set to the branch head. Manual
dispatch uses the same compatibility build selection as a push for this
experiment.

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
produce the same independently specified count. In the current `physical` mode,
eligible joins use `KEY_SET_JOIN`: a zero-aggregate `GroupedAggregateHashTable`
retains the distinct Build keys and is searched directly with `LookupGroups`.
Each sink worker builds its own table. Combine adopts the first table and merges
other workers' distinct keys into it; the final table is immutable during
parallel Probe. There is no subsequent generic `JoinHashTable` build or scan of
its saved Build rows. Build NULL presence is retained separately for MARK, and
probe NULLs never match under ordinary equality.

This first direct-lookup PoC handles single integer-key `=` SEMI, ANTI and
uncorrelated MARK joins. Other joins keep the existing path. It is in-memory:
there is no spill implementation, capacity cutoff or NDV activation heuristic.
`debug_force_external` and recursive/delim joins use the existing path. Local
Build and Probe are parallel, but merging local distinct sets is serialized.
Existing min/max runtime-filter pushdown is retained; JoinHashTable-specific
IN, Bloom and prefix-range filters and perfect hash join are not used by the new
operator. The controlled cases isolate the key-set comparison, while the natural
cases also measure these differences from the generic join path.

The existing CI compares the improved physical prefilter (Base, approach 3)
against direct key-set lookup (PR, approach 4), using the same 43 workloads.
Neither logical nor off mode is timed in this run. The existing Regression
matrix is restricted to Micro for this experiment. The standard Micro job uses
two threads and a 6.5 GiB memory limit.

For this fork experiment only, Main's existing `OVERRIDE_JOBS` selects the
required release/relassert builds and regression jobs. Its `base_hash` pins the
comparison to `667be65779c318f5f426159793230f5aa80ca308`, the improved physical
prefilter.
The Regression workflow honors an explicit base hash on fork pushes as well.
These experiment-specific CI selections should be removed before an upstream PR.

Results appear in GitHub Actions → Main → regression / Bench Micro → Run
regression benchmark. The log contains samples, per-query medians and the
comparison table. The existing regression criteria remain enabled, so an
experiment may report regressions or fail its aggregate gate while still
providing useful measurements. Inspect individual queries rather than relying
only on the aggregate result.
