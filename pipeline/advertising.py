"""Attribute sessions to the last qualifying click and count distinct campaign assists."""

from pathlib import Path

from events import ISO_TIME, load_raw


WEEK_MS = 7 * 24 * 60 * 60 * 1000


def prepare_campaign_daily(con, data: Path, manifest: dict) -> None:
    con.execute("""
        CREATE TEMP TABLE campaigns AS SELECT * FROM read_csv(?, header=true,
            columns={'campaign_id': 'VARCHAR', 'channel': 'VARCHAR'})
    """, [str(data / "campaigns.csv")])
    load_raw(con, data / "clicks", "raw_clicks", ("click_id", "user_id", "campaign_id", "ts"))
    # Repeated click IDs have identical values. For a given person, timestamp
    # and campaign, only the smallest click ID can ever win a tie.
    con.execute("""
        CREATE TEMP TABLE clicks AS
        WITH typed AS (
            SELECT *, CASE
                WHEN regexp_full_match(ts, '[0-9]+') THEN try_cast(ts AS BIGINT)
                WHEN regexp_full_match(ts, $iso) THEN epoch_ms(try_cast(ts AS TIMESTAMPTZ))
            END AS ts_ms
            FROM raw_clicks
            WHERE click_id IS NOT NULL AND user_id IS NOT NULL
        )
        SELECT coalesce(i.person, t.user_id) AS person, ts_ms, campaign_id,
               min(click_id) AS click_id
        FROM typed t JOIN campaigns USING (campaign_id)
        LEFT JOIN identities i ON i.alias = t.user_id
        WHERE ts_ms >= epoch_ms(CAST($window_start AS TIMESTAMPTZ)) - $week
          AND ts_ms < epoch_ms(CAST($window_end AS TIMESTAMPTZ))
        GROUP BY coalesce(i.person, t.user_id), ts_ms, campaign_id
    """, {"iso": ISO_TIME, "window_start": manifest["window_start"],
          "window_end": manifest["window_end"], "week": WEEK_MS})
    con.execute("DROP TABLE raw_clicks")

    con.execute("""
        CREATE TEMP TABLE credits AS
        WITH last_click AS (
            SELECT person, ts_ms, arg_min(campaign_id, click_id) AS campaign_id
            FROM clicks GROUP BY person, ts_ms
        ), attributed AS (
            SELECT s.day, s.revenue_cents,
                   CASE WHEN s.start_ms - c.ts_ms <= $week
                        THEN c.campaign_id ELSE 'organic' END AS campaign_id
            FROM sessions s ASOF LEFT JOIN last_click c
              ON s.user_id = c.person AND s.start_ms >= c.ts_ms
        )
        SELECT campaign_id, day, count(*) AS sessions,
               count(*) FILTER (WHERE revenue_cents > 0) AS purchasing_sessions,
               sum(revenue_cents)::BIGINT AS revenue_cents
        FROM attributed GROUP BY campaign_id, day
    """, {"week": WEEK_MS})

    # Each click covers the next seven days, including both endpoints. Merge
    # overlapping windows within a campaign so a session gets at most one assist.
    con.execute("""
        CREATE TEMP TABLE coverage AS
        WITH previous AS (
            SELECT person, campaign_id, ts_ms, lag(ts_ms) OVER (
                PARTITION BY person, campaign_id ORDER BY ts_ms
            ) AS prev_ms FROM clicks
        ), islands AS (
            SELECT *, count(*) FILTER (WHERE prev_ms IS NULL OR ts_ms - prev_ms > $week)
                OVER (PARTITION BY person, campaign_id ORDER BY ts_ms) AS island
            FROM previous
        )
        SELECT person, campaign_id, min(ts_ms) AS lo, max(ts_ms) + $week AS hi
        FROM islands GROUP BY person, campaign_id, island
    """, {"week": WEEK_MS})
    con.execute("DROP TABLE clicks")

    # A user's day has at most 48 session starts. Joining one row per user/day
    # avoids expanding heavily repeated clicks into all possible session pairs.
    con.execute("""
        CREATE TEMP TABLE campaign_daily AS
        WITH days AS (
            SELECT user_id, day, min(start_ms) AS first_ms, max(start_ms) AS last_ms,
                   list(start_ms) AS starts
            FROM sessions GROUP BY user_id, day
        ), covered_days AS (
            SELECT c.campaign_id, d.day,
                   CASE WHEN c.lo <= d.first_ms AND c.hi >= d.last_ms
                        THEN len(d.starts)
                        ELSE len(list_filter(d.starts, t -> t BETWEEN c.lo AND c.hi))
                   END AS n
            FROM coverage c JOIN days d
              ON c.person = d.user_id AND c.lo <= d.last_ms AND c.hi >= d.first_ms
        ), assists AS (
            SELECT campaign_id, day, sum(n)::BIGINT AS assisted_sessions
            FROM covered_days WHERE n > 0 GROUP BY campaign_id, day
        ), totals AS (
            SELECT campaign_id, day, coalesce(sessions, 0) AS sessions,
                   coalesce(purchasing_sessions, 0) AS purchasing_sessions,
                   coalesce(revenue_cents, 0) AS revenue_cents,
                   coalesce(assisted_sessions, 0) AS assisted_sessions
            FROM credits FULL OUTER JOIN assists USING (campaign_id, day)
        )
        SELECT t.campaign_id, CASE WHEN t.campaign_id = 'organic' THEN 'organic'
                                   ELSE c.channel END AS channel,
               t.day, t.sessions, t.purchasing_sessions, t.revenue_cents, t.assisted_sessions
        FROM totals t LEFT JOIN campaigns c USING (campaign_id)
    """)
    con.execute("DROP TABLE coverage")
    con.execute("DROP TABLE credits")
