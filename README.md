# Shopping Sessions

A DuckDB pipeline for the Builders Buildathon's Databricks track. It reads shopping events, ad clicks and identity links, then writes two daily Parquet tables. The data and expected answers used for verification come from the organizer's [Shopping Sessions track](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions).

## How it works

- `pipeline/events.py` reads the track's CSV dialect and cleans events. Deduplication keeps the earliest ingested copy before validation.
- `pipeline/identity.py` resolves linked IDs across files. Every event and click uses the smallest ID in its connected component.
- `pipeline/pipeline.py` sorts each person's events, starts a session after gaps strictly over 30 minutes, and groups its events and purchases by the UTC day the session began.
- `pipeline/advertising.py` assigns each session to the latest eligible click, breaking timing ties by click ID. Overlapping seven-day intervals from the same campaign are merged so that it assists a given session only once.

The full input and output specifications are in `rules.md`.

## Verify locally

Use Python 3.10+ and install the runtime and test dependencies in a virtual environment:

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r requirements.txt polars
scratch_dir=$(mktemp -d)
export SCRATCH_DIR="$scratch_dir"
python scripts/run_fixtures.py fixtures -- python pipeline/pipeline.py
```

The 36 fixtures cover dirty CSVs, millisecond time boundaries, cross-device shopping sessions and ad attribution. To inspect a single example's two outputs:

```sh
DATA_DIR=fixtures/A11/input OUTPUT_DIR="$scratch_dir/example" python pipeline/pipeline.py
python scripts/compare.py "$scratch_dir/example" fixtures/A11/expected
```

For the public development set, download the official generator and reference answers from the track page. Build the generator according to its README, then run:

```sh
generate --seed 1 --events 1000000 --clicks 250000 --output dev
DATA_DIR=dev OUTPUT_DIR=out python pipeline/pipeline.py
python scripts/compare.py out dev-answer
```

The final line should print `MATCH`. The two Parquet outputs are in `out/user_daily/` and `out/campaign_daily/`. The public answers for this seed have 117,038 user-day rows and 27,400 campaign-day rows. Keep the generated dataset, output and answers outside Git.

## Running on the scorer

The top-level `Dockerfile` packages the pipeline. The scorer mounts input at `/data`, output at `/output`, and writable temporary space at `/scratch`. Its time includes container start-up, reading, computing, and writing both tables. To reproduce the target platform locally with Docker and buildx, use `make build` followed by `make fixtures` or `make run-dev`. Local public-data timings and the leaderboard use different machines and datasets.

## Credits

Based on the organizer's DuckDB starter. The Dockerfile, Makefile, runner, official fixtures, specification and comparison scripts originated in that kit. The processing code uses DuckDB and the Python standard library. Polars is used by the organizer's local verification scripts.
