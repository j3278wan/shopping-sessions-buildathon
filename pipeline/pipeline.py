#!/usr/bin/env python3
"""Produce the user and campaign daily reports for Shopping Sessions."""

import json
import os
from pathlib import Path

import duckdb

from advertising import prepare_campaign_daily
from events import prepare_events
from identity import prepare_identities


def group_sessions(con):
    # event_id breaks timestamp ties consistently in both windows.
    con.execute("""
        CREATE TEMP TABLE session_events AS
        WITH person_events AS (
            SELECT coalesce(i.person, e.user_id) AS user_id,
                   e.event_id, e.ts_ms, e.product_id, e.purchase_cents
            FROM events e LEFT JOIN identities i ON e.user_id = i.alias
        ), gaps AS (
            SELECT *, ts_ms - lag(ts_ms) OVER (
                PARTITION BY user_id ORDER BY ts_ms, event_id
            ) AS gap_ms
            FROM person_events
        )
        SELECT user_id, product_id, purchase_cents,
            max(CASE WHEN gap_ms IS NULL OR gap_ms > 1800000 THEN ts_ms END)
            OVER (
                PARTITION BY user_id ORDER BY ts_ms, event_id
                ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
            ) AS start_ms
        FROM gaps
    """)
    con.execute("DROP TABLE events")
    con.execute("""
        CREATE TEMP TABLE sessions AS
        SELECT user_id, start_ms, epoch_ms(start_ms)::DATE AS day,
               count(*) AS events, coalesce(sum(purchase_cents), 0)::BIGINT AS revenue_cents
        FROM session_events
        GROUP BY user_id, start_ms
    """)


def prepare_user_daily(con):
    con.execute("""
        CREATE TEMP TABLE user_daily AS
        WITH category_totals AS (
            SELECT e.user_id, epoch_ms(e.start_ms)::DATE AS day,
                   coalesce(p.category, 'unknown') AS category,
                   sum(e.purchase_cents) AS spent
            FROM session_events e LEFT JOIN products p USING (product_id)
            WHERE e.purchase_cents IS NOT NULL
            GROUP BY e.user_id, day, coalesce(p.category, 'unknown')
        ), favorites AS (
            SELECT user_id, day, first(category ORDER BY spent DESC, category) AS top_category
            FROM category_totals GROUP BY user_id, day
        ), totals AS (
            SELECT user_id, day, count(*) AS sessions, sum(events)::BIGINT AS events,
                   sum(revenue_cents)::BIGINT AS revenue_cents
            FROM sessions GROUP BY user_id, day
        )
        SELECT t.*, f.top_category FROM totals t LEFT JOIN favorites f USING (user_id, day)
    """)
    con.execute("DROP TABLE session_events")


def write_reports(con, output):
    for table in ("user_daily", "campaign_daily"):
        folder = output / table
        folder.mkdir(parents=True, exist_ok=True)
        con.execute(f"COPY {table} TO ? (FORMAT PARQUET)", [str(folder / "part-0.parquet")])


def main():
    data = Path(os.environ.get("DATA_DIR", "/data"))
    output = Path(os.environ.get("OUTPUT_DIR", "/output"))
    scratch = Path(os.environ.get("SCRATCH_DIR", "/scratch"))
    manifest = json.loads((data / "manifest.json").read_text())
    with duckdb.connect() as con:
        con.execute("SET threads = ?", [int(os.environ.get("NCPU", os.cpu_count() or 1))])
        memory_gb = int(os.environ.get("MEM_GB", "8"))
        con.execute("SET memory_limit = ?", [f"{max(1, int(memory_gb * 0.8))}GB"])
        con.execute("SET temp_directory = ?", [str(scratch / "duckdb_tmp")])
        con.execute("SET preserve_insertion_order = false")
        prepare_identities(con, data)
        prepare_events(con, data, manifest)
        con.execute("""
            CREATE TEMP TABLE products AS SELECT * FROM read_csv(?, header=true,
                columns={'product_id': 'VARCHAR', 'category': 'VARCHAR'})
        """, [str(data / "products.csv")])
        group_sessions(con)
        prepare_user_daily(con)
        prepare_campaign_daily(con, data, manifest)
        write_reports(con, output)


if __name__ == "__main__":
    main()
