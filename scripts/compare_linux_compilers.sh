#!/usr/bin/env bash
# Compare compiler choices on identical inputs in one standard Linux job.
set -euo pipefail
workdir=$(mktemp -d)
python3 -m venv "$workdir/venv"
source "$workdir/venv/bin/activate"
python -m pip --disable-pip-version-check -q install pyarrow==20.0.0 polars
curl --fail --silent --show-error --location --retry 2 \
    https://www.joinbuilders.org/buildathon/tracks/shopping-sessions/files/generator.zip \
    --output "$workdir/generator.zip"
(cd "$workdir" && sha256sum --check <<< '13724cadbac5a99d847e27cea911cd2e338ff972b46c2039da87452011bb101e  generator.zip')
unzip -qo "$workdir/generator.zip" -d "$workdir"
cargo build --release --locked --offline --manifest-path "$workdir/generator/Cargo.toml"
gen="$workdir/generator/target/release/generate"
"$gen" --seed 27 --events 250000 --clicks 62500 --output "$workdir/train" --threads 2
"$gen" --seed 73 --events 20000000 --clicks 5000000 --output "$workdir/evaluate" --threads 4
arrow_dir=$(python -c 'import pyarrow; print(pyarrow.get_library_dirs()[0])')
common=(-O3 -std=c++17 -DNDEBUG -march=haswell -pthread "-I$arrow_dir/include")
libs=("$arrow_dir/libparquet.so.2000" "$arrow_dir/libarrow.so.2000" "-Wl,-rpath,$arrow_dir")
compile() {
    local name=$1 cxx=$2
    shift 2
    "$cxx" "${common[@]}" "$@" native/main.cpp native/ingest.cpp native/reports.cpp "${libs[@]}" -o "$workdir/$name"
}
lscpu | sed -n '1,32p'
baseline=aa826e3d4c002af8a21496bb2845aef61ca44889
git fetch -q --depth=1 https://github.com/j3278wan/shopping-sessions-buildathon.git "$baseline"
git worktree add -q --detach "$workdir/previous" FETCH_HEAD
(cd "$workdir/previous" && compile scored g++)
docker build -q -t shopping-candidate .
chmod 755 "$workdir"
mkdir "$workdir/container-result" && chmod 777 "$workdir/container-result"
IMAGE=shopping-candidate CPUS=4 MEM=12g sh scripts/docker_run.sh "$workdir/evaluate" "$workdir/container-result"
compile release clang++-18
git apply bench/catalog.patch
compile catalog clang++-18
git apply -R bench/catalog.patch
git apply bench/group-tickets.patch
compile groups clang++-18
git apply -R bench/group-tickets.patch
git apply bench/pages.patch
clang++-18 -std=c++17 -O3 -march=haswell -Inative bench/check-pages.cpp -o "$workdir/check-pages"
"$workdir/check-pages"
compile pages clang++-18
for name in release catalog groups pages; do
    python scripts/run_fixtures.py fixtures -- "$workdir/$name" > "$workdir/fixtures-$name.log"
    tail -1 "$workdir/fixtures-$name.log"
done
for trial in 1 2 3; do
    order=(scored release catalog groups pages)
    if [[ $trial == 2 ]]; then order=(pages groups catalog release scored); fi
    for name in "${order[@]}"; do
        find "$workdir/evaluate" -type f -exec cat {} + > /dev/null
        dest="$workdir/out-$name-$trial"
        mkdir "$dest"
        env NCPU=4 DATA_DIR="$workdir/evaluate" OUTPUT_DIR="$dest" SHOPPING_TRACE=1 SHOPPING_TIMINGS=1 \
            /usr/bin/time -f "BENCH $name $trial %e s %M KiB" "$workdir/$name"
    done
done
for name in release catalog groups pages; do
    python scripts/compare.py "$workdir/out-scored-3" "$workdir/out-$name-3"
done
python scripts/compare.py "$workdir/out-scored-3" "$workdir/container-result"
rm -r "$workdir/evaluate"
python -m pip --disable-pip-version-check -q install duckdb
"$gen" --seed 412 --events 60000000 --clicks 15000000 --output "$workdir/final-size" --threads 4
for name in scored release; do
    dest="$workdir/final-size-$name"
    mkdir "$dest"
    env NCPU=4 DATA_DIR="$workdir/final-size" OUTPUT_DIR="$dest" \
       /usr/bin/time -f "FINAL-SIZE $name %e s %M KiB" "$workdir/$name"
done
python bench/check-large.py "$workdir/final-size-scored" "$workdir/final-size-release"
