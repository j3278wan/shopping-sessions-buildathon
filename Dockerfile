FROM python:3.12-slim-bookworm AS arrow-runtime
# This wheel supplies the prebuilt Apache Arrow/Parquet C++ libraries.
RUN python -m pip install --no-cache-dir --no-deps --only-binary=:all: pyarrow==20.0.0

FROM arrow-runtime AS build
RUN apt-get update && apt-get install -y --no-install-recommends clang-19 libc6-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY native/ native/
COPY scripts/build_native.sh scripts/build_native.sh
RUN CXX=clang++-19 sh scripts/build_native.sh /out/shopping-sessions

FROM arrow-runtime
WORKDIR /app
COPY --from=build /out/shopping-sessions /app/shopping-sessions
COPY run.sh /app/run.sh
RUN chmod 755 /app/run.sh && ! ldd /app/shopping-sessions | grep 'not found'
USER 1000:1000
CMD ["/app/run.sh"]
