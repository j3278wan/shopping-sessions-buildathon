#!/bin/sh
# Link against the C++ headers and shared libraries in the pinned PyArrow wheel.
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
out=${1:-"$root/build/shopping-sessions"}
mkdir -p "$(dirname -- "$out")"
python_cmd=${PYTHON:-python3}
arrow_dir=$("$python_cmd" -c 'from importlib.util import find_spec; import pathlib; print(pathlib.Path(find_spec("pyarrow").origin).parent)')
case "$(uname -s)" in
    Darwin) arrow=libarrow.2000.dylib; parquet=libparquet.2000.dylib ;;
    Linux)  arrow=libarrow.so.2000; parquet=libparquet.so.2000 ;;
    *) echo "Expected macOS or Linux" >&2; exit 1 ;;
esac
case "$(uname -m)" in x86_64) arch=-march=haswell ;; *) arch='' ;; esac
"${CXX:-c++}" -O3 -DNDEBUG -std=c++17 -pthread $arch -I"$arrow_dir/include" \
    "$root/native/main.cpp" "$root/native/ingest.cpp" "$root/native/reports.cpp" \
    "$arrow_dir/$parquet" "$arrow_dir/$arrow" "-Wl,-rpath,$arrow_dir" -o "$out"
