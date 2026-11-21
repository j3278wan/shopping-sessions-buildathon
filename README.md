# Shopping Sessions

This repository contains the SJJJ team's solution to the [Databricks Shopping Sessions challenge](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions). The submission reads the supplied CSVs, resolves linked shoppers, and writes the two required daily reports as Apache Parquet. The optimized executable is C++17. The earlier DuckDB implementation remains in `pipeline/` for comparison.

## How the executable works

1. Read the reporting window from the manifest and resolve identity links using a union-find whose canonical name is the smallest ID in the component.
2. Process complete lines in parallel from the memory-mapped CSV input. The CSV scanner checks the exact quoting rules and preserves whitespace. For every event ID the earliest ingested copy wins, including when that copy is invalid. Hash collisions are resolved by comparing the original IDs.
3. Group each shopper's surviving events and ad clicks into the same partition. Sort the shopper's timeline and start a new session only when the gap exceeds 30 minutes.
4. Advance a seven-day click window alongside each shopper's sessions. The latest eligible click wins the revenue. Each distinct campaign in the window also records one assist.
5. Write the user-day shards and the combined campaign-day table through Arrow's Parquet writer. All partitioning and dictionaries are derived from the supplied input.

The organizer's full input/output contract is in `rules.md`.

## Run and check

The root Dockerfile compiles for the image platform. The scorer uses Linux/amd64, 8 vCPUs and 24 GB of memory. The C++ build links the Arrow and Parquet libraries in the pinned PyArrow wheel. Input is mounted at `/data` and the scorer supplies a fresh `/output` directory. The runtime needs no network.

```sh
make build
make fixtures
```

For a native run on macOS or Linux, install a C++17 compiler, Python 3.10+ and the local test dependencies:

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install pyarrow==20.0.0 polars
make native
python scripts/run_fixtures.py fixtures -- ./build/shopping-sessions
```

To verify the public development set, build the generator from the track page, download and extract `dev-answer.zip`, then run:

```sh
generate --seed 1 --events 1000000 --clicks 250000 --output dev
out=$(mktemp -d)
DATA_DIR=dev OUTPUT_DIR="$out" NCPU=8 ./build/shopping-sessions
python scripts/compare.py "$out" dev-answer
```

For repeated native runs, supply a fresh output directory each time. The official comparison ignores row order and file count, and checks column types, duplicate keys and both full tables.

## Reproduce the larger benchmark

Use the official generator with an independent seed. Keep generated input and reports outside Git. The previous DuckDB version and the native program can read the same dataset.

```sh
generate --seed 73 --events 20000000 --clicks 5000000 --output dev-20m
out=$(mktemp -d)
time env DATA_DIR=dev-20m OUTPUT_DIR="$out" NCPU=8 ./build/shopping-sessions
```

For a stage breakdown, set `SHOPPING_TRACE=1` and `SHOPPING_TIMINGS=1`. To check against the earlier implementation, install `requirements.txt`, use a separate writable scratch and output directory, run `python pipeline/pipeline.py` with the same input, then compare the two outputs using `scripts/compare.py`. Hidden practice and final data are only tested by the organizer.

## Measured locally

On an Apple M1 Pro with eight worker threads, the official generator's seed 73 produced 20 million events and 5 million clicks. Both versions read the same input after warming the file cache and wrote to a fresh output directory for each trial. Wall time includes process startup and Parquet output.

| Implementation | Three runs (seconds) | Median |
| --- | --- | --- |
| DuckDB, commit `ec136af` | 28.54, 29.65, 35.26 | 29.65 s |
| Native C++, commit `a1daa54` | 2.62, 2.39, 1.95 | 2.39 s |

That revision achieved a 12.4× local speedup and an official practice score of 2.60 s. The next revision batches identity lookups, stores common-length identifiers losslessly in an atomic table, and uses radix sorting for large timelines whose observed range fits 32 bits. Other identifier shapes and time ranges use the general algorithms. The organizer measures rankings on separate Linux machines with hidden data.

## Credits

Based on the organizer's DuckDB starter, fixtures, specification, Makefile and verification scripts. The native implementation uses Apache Arrow and Apache Parquet for serialization. Polars is used by the organizer's verification scripts.
