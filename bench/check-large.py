from pathlib import Path
import sys
import duckdb

left, right = map(Path, sys.argv[1:])
con = duckdb.connect(':memory:')
con.execute("set threads=4")
con.execute("set memory_limit='4GB'")
for table, keys in [('user_daily', ['user_id','day']), ('campaign_daily', ['campaign_id','day'])]:
    files = [f"{p}/{table}/*.parquet" for p in (left,right)]
    expr = ','.join(keys)
    for view, path in zip(['a','b'],files):
        con.read_parquet(path).create_view(view, replace=True)
    assert {row[:2] for row in con.execute('describe a').fetchall()} == {row[:2] for row in con.execute('describe b').fetchall()}
    cols = ','.join(row[0] for row in con.execute('describe a').fetchall())
    totals = con.execute(f'select count(*),count(distinct ({expr})) from a').fetchone()
    other = con.execute(f'select count(*),count(distinct ({expr})) from b').fetchone()
    assert totals == other and totals[0] == totals[1], (table,totals,other)
    for l,r in [('a','b'),('b','a')]:
        result = con.execute(f'select count(*) from (select {cols} from {l} except all select {cols} from {r})').fetchone()[0]
        assert result == 0, (table,l,result)
    print(table,totals[0],'rows, all columns match and keys unique',flush=True)
print('MATCH',flush=True)
