"""Map linked user IDs to the smallest ID in their connected component."""

from itertools import islice
import json
from pathlib import Path
import re

from events import FIELD


LINK = re.compile(f'({FIELD}),({FIELD})')


def prepare_identities(con, data: Path) -> None:
    """Create identities(alias, person). An ID absent from links represents itself."""
    parents: dict[str, str] = {}

    def root(user: str) -> str:
        parent = parents.setdefault(user, user)
        while parent != user:
            # Path halving keeps even long identity chains cheap to traverse.
            parents[user] = parents[parent]
            user = parent
            parent = parents[user]
        return user

    for path in sorted((data / 'links').glob('*.csv')):
        with path.open(encoding='utf-8', newline='\n') as source:
            next(source, None)
            for line in source:
                match = LINK.fullmatch(line.removesuffix('\n'))
                if match is None:
                    continue
                left, right = (field.strip('"') for field in match.groups())
                if not left or not right:
                    continue
                a, b = root(left), root(right)
                # For valid UTF-8, code point order is also UTF-8 byte order.
                if a < b:
                    parents[b] = a
                elif b < a:
                    parents[a] = b

    con.execute('CREATE TEMP TABLE identities(alias VARCHAR, person VARCHAR)')
    # Pass one text parameter per batch. Binding a Python list makes DuckDB
    # convert every string individually; JSON also preserves control characters.
    aliases = iter(parents)
    while batch := list(islice(aliases, 65536)):
        pairs = json.dumps([(alias, root(alias)) for alias in batch],
                           ensure_ascii=False, separators=(',', ':'))
        con.execute("""
            INSERT INTO identities
            SELECT pair[1], pair[2] FROM unnest(?::JSON::VARCHAR[][]) AS mapping(pair)
        """, [pairs])
