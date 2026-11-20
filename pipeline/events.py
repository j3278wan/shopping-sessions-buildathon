"""Read the competition's event logs into a DuckDB temporary table."""

from pathlib import Path


# Fields never contain commas, embedded quotes or newlines. Whole-field quotes
# are permitted, and a carriage return counts as data even before a newline.
FIELD = '(?:"[^",]*"|[^",]*)'
HOUR = '(?:[01][0-9]|2[0-3])'
ISO_TIME = (
    f'[0-9]{{4}}-[0-9]{{2}}-[0-9]{{2}}T{HOUR}:[0-5][0-9]:[0-5][0-9]'
    f'(?:Z|[+-]{HOUR}:[0-5][0-9])'
)


def load_raw(con, directory: Path, table_name: str, columns: tuple[str, ...]) -> None:
    """Read a folder's CSV parts into a VARCHAR table. Names come from our schema."""
    schema = ', '.join(f'{column} VARCHAR' for column in columns)
    con.execute(f'CREATE TEMP TABLE {table_name} ({schema})')
    fields = ', '.join(f"""nullif(trim(fields[{i}], '"'), '')"""
                       for i in range(1, len(columns) + 1))
    pattern = ','.join([FIELD] * len(columns))

    def insert_lines(source: str, params: dict):
        con.execute(f"""
            INSERT INTO {table_name}
            WITH parsed AS (
                SELECT string_split(line, ',') AS fields FROM {source}
                WHERE regexp_full_match(line, $pattern)
            )
            SELECT {fields} FROM parsed
        """, {**params, 'pattern': pattern})

    native_files = []
    for path in sorted(directory.glob('*.csv')):
        # The native scanner can read files in parallel as raw lines using an
        # absent delimiter. Fall back when it could reinterpret data (e.g. CRLF)
        # or when a line might exceed the scanner's default 2 MiB limit.
        with path.open('rb') as raw:
            needs_literal_read = any(
                b'\r' in block or b'\x1f' in block or b'\0' in block
                or (len(block) == 1024 * 1024 and b'\n' not in block)
                for block in iter(lambda: raw.read(1024 * 1024), b'')
            )
        if not needs_literal_read:
            if path.stat().st_size:
                native_files.append(str(path))
            continue
        with path.open(encoding='utf-8', newline='\n') as source:
            next(source, None)
            while lines := source.readlines(4 * 1024 * 1024):
                insert_lines("unnest(string_split($block, chr(10))) AS input(line)",
                             {'block': ''.join(lines)})
    if native_files:
        insert_lines(r"""read_csv($paths, auto_detect=false, header=true,
                            columns={'line': 'VARCHAR'}, delim=chr(31),
                            quote='', escape='', new_line='\n')""", {'paths': native_files})


def prepare_events(con, data: Path, manifest: dict) -> None:
    """Create events(event_id, user_id, ts_ms, product_id, purchase_cents)."""
    load_raw(con, data / 'events', 'raw_events',
             ('event_id', 'user_id', 'ts', 'ingest_ts', 'event_type', 'product_id', 'price_cents'))
    # ingest_ts is always UTC, in the same fixed-width form, so its text
    # order is chronological. The earliest copy wins even if it is invalid.
    con.execute("""
        CREATE TEMP TABLE events AS
        WITH dedup AS (
            SELECT * FROM raw_events WHERE event_id IS NOT NULL
            QUALIFY row_number() OVER (PARTITION BY event_id ORDER BY ingest_ts) = 1
        ), typed AS (
            SELECT *,
                CASE WHEN regexp_full_match(ts, '[0-9]+') THEN try_cast(ts AS BIGINT)
                     WHEN regexp_full_match(ts, $iso) THEN epoch_ms(try_cast(ts AS TIMESTAMPTZ))
                END AS ts_ms,
                CASE WHEN event_type = 'purchase' AND regexp_full_match(price_cents, '[0-9]+')
                     THEN try_cast(price_cents AS INTEGER)
                END AS purchase_cents
            FROM dedup
        )
        SELECT event_id, user_id, ts_ms, product_id, purchase_cents
        FROM typed
        WHERE user_id IS NOT NULL AND product_id IS NOT NULL
          AND event_type IN ('view', 'cart', 'purchase')
          AND (event_type <> 'purchase' OR purchase_cents BETWEEN 1 AND 10000000)
          AND ts_ms >= epoch_ms(CAST($window_start AS TIMESTAMPTZ))
          AND ts_ms <  epoch_ms(CAST($window_end AS TIMESTAMPTZ))
          AND ts_ms >= epoch_ms(try_cast(ingest_ts AS TIMESTAMPTZ)) - 86400000
    """, {'iso': ISO_TIME, 'window_start': manifest['window_start'],
          'window_end': manifest['window_end']})
    con.execute('DROP TABLE raw_events')
