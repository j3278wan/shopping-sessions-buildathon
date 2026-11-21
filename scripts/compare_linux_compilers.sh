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
train() {
    local name=$1
    mkdir -p "$workdir/train-$name"
    DATA_DIR="$workdir/train" OUTPUT_DIR="$workdir/train-$name" NCPU=4 "$workdir/$name"
}
printf '%s\n' 'CPU model and advertised instruction sets'
lscpu | head -32
compiler=clang++-18
profiler=llvm-profdata-18
command -v "$compiler"
command -v "$profiler"
compile gcc g++
compile clang "$compiler"
mkdir "$workdir/gcc-profile"
compile gcc-pgo g++ -fprofile-generate="$workdir/gcc-profile" -fprofile-update=atomic
train gcc-pgo
compile gcc-pgo g++ -fprofile-use="$workdir/gcc-profile" -fprofile-correction
compile clang-pgo "$compiler" -fprofile-instr-generate
LLVM_PROFILE_FILE="$workdir/clang.profraw" train clang-pgo
"$profiler" merge -o "$workdir/clang.profdata" "$workdir/clang.profraw"
compile clang-pgo "$compiler" -fprofile-instr-use="$workdir/clang.profdata"
for name in gcc clang gcc-pgo clang-pgo; do
    python scripts/run_fixtures.py fixtures -- "$workdir/$name" > "$workdir/fixtures-$name.log"
    tail -1 "$workdir/fixtures-$name.log"
done
for trial in 1 2 3; do
    order=(gcc clang gcc-pgo clang-pgo)
    if [[ $trial == 2 ]]; then order=(clang-pgo gcc-pgo clang gcc); fi
    for name in "${order[@]}"; do
        find "$workdir/evaluate" -type f -exec cat {} + > /dev/null
        dest="$workdir/out-$name-$trial"
        mkdir "$dest"
        env NCPU=4 DATA_DIR="$workdir/evaluate" OUTPUT_DIR="$dest" SHOPPING_TRACE=1 SHOPPING_TIMINGS=1 \
            /usr/bin/time -f "BENCH $name $trial %e s %M KiB" "$workdir/$name"
    done
done
for name in clang gcc-pgo clang-pgo; do
    python scripts/compare.py "$workdir/out-gcc-3" "$workdir/out-$name-3"
done
