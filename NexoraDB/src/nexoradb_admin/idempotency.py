"""Durable replay journal, deliberately fail-closed across ambiguous failures.

Not a distributed transaction with RocksDB: a reserved request may have run.
Never expire/delete reservations automatically, even after process restart.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import sqlite3

from fastapi import HTTPException


class RequestJournal:
    def __init__(self, path):
        self.path = Path(path)

    def run(self, principal, key, payload, action):
        if not re.fullmatch(r"[A-Za-z0-9_.:-]{1,128}", key):
            raise HTTPException(400, "Invalid Idempotency-Key")
        fingerprint = hashlib.sha256(json.dumps(payload, sort_keys=True, separators=(",", ":"),
                                                allow_nan=False).encode()).hexdigest()
        identity = hashlib.sha256((principal + "\0" + key).encode()).hexdigest()
        self.path.parent.mkdir(parents=True, exist_ok=True)
        # Create private before sqlite opens the file. Parent must be trusted.
        descriptor = os.open(self.path, os.O_CREAT | os.O_RDWR | getattr(os, "O_NOFOLLOW", 0), 0o600)
        os.close(descriptor)
        connection = sqlite3.connect(self.path, timeout=2, isolation_level=None)
        try:
            connection.execute("PRAGMA synchronous=FULL")
            connection.execute("PRAGMA max_page_count=16384")
            connection.execute("CREATE TABLE IF NOT EXISTS requests (id TEXT PRIMARY KEY, fingerprint TEXT NOT NULL, response TEXT)")
            connection.execute("BEGIN IMMEDIATE")
            row = connection.execute("SELECT fingerprint,response FROM requests WHERE id=?", (identity,)).fetchone()
            if row:
                connection.execute("ROLLBACK")
                if row[0] != fingerprint:
                    raise HTTPException(409, "Idempotency-Key was used with a different request")
                if row[1] is None:
                    raise HTTPException(409, "Request in progress or outcome unknown; reconcile before retrying")
                return json.loads(row[1])
            if connection.execute("SELECT count(*) FROM requests").fetchone()[0] >= 10000:
                raise HTTPException(503, "Request journal capacity reached; operator action required")
            connection.execute("INSERT INTO requests VALUES (?,?,NULL)", (identity, fingerprint))
            connection.execute("COMMIT")
            directory = os.open(self.path.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
            # No database transaction/lock spans native query execution.
            result = action()
            encoded = json.dumps(result, ensure_ascii=False, allow_nan=False, separators=(",", ":"))
            connection.execute("BEGIN IMMEDIATE")
            connection.execute("UPDATE requests SET response=? WHERE id=?", (encoded, identity))
            connection.execute("COMMIT")
            return result
        except sqlite3.Error as exc:
            # A completed native action may precede a failed journal commit.
            raise HTTPException(503, "Request journal unavailable; outcome may be unknown") from exc
        finally:
            connection.close()
