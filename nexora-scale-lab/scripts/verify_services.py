"""Verify authenticated access, versions and rejection of unauthenticated access."""
import json
import os

import psycopg
from psycopg.conninfo import conninfo_to_dict
from pymongo import MongoClient
from pymongo.errors import OperationFailure

dsn = os.environ['SCALE_LAB_POSTGRES_DSN']
with psycopg.connect(dsn, connect_timeout=5) as connection:
    pg = {'version': connection.execute('SHOW server_version').fetchone()[0],
          'fsync': connection.execute('SHOW fsync').fetchone()[0],
          'synchronous_commit': connection.execute('SHOW synchronous_commit').fetchone()[0],
          'listen_addresses': connection.execute('SHOW listen_addresses').fetchone()[0],
          'port': connection.execute('SHOW port').fetchone()[0]}
    assert pg['fsync'] == 'on' and pg['synchronous_commit'] == 'on'
invalid = conninfo_to_dict(dsn)
invalid['password'] = 'deliberately-invalid-scale-lab-password'
try:
    with psycopg.connect(**invalid, connect_timeout=5):
        raise AssertionError('PostgreSQL accepted an invalid password')
except psycopg.OperationalError as error:
    if 'password authentication failed' not in str(error):
        raise
pg['invalid_password_rejected'] = True

with MongoClient(os.environ['SCALE_LAB_MONGO_URI'], serverSelectionTimeoutMS=5000) as client:
    client.scale_lab.list_collection_names()
    mongo = {'version': client.server_info()['version'], 'authenticated_access': True}
with MongoClient('mongodb://127.0.0.1:57017/scale_lab', serverSelectionTimeoutMS=5000) as client:
    try:
        client.scale_lab.list_collection_names()
        raise AssertionError('MongoDB allowed unauthenticated collection access')
    except OperationFailure as error:
        if error.code != 13:
            raise
mongo['unauthenticated_access_rejected'] = True
print(json.dumps({'postgresql': pg, 'mongodb': mongo}, indent=2))
