"""Create the first benchmark principal through MongoDB's localhost exception.

Use only on a fresh installation with authorization enabled and loopback binding.
"""
import os
from pymongo import MongoClient

password = os.environ['SCALE_LAB_TEMP_PASSWORD']
with MongoClient('mongodb://127.0.0.1:57017', serverSelectionTimeoutMS=10000) as client:
    client.admin.command('createUser', 'scale_lab_admin', pwd=password,
                         roles=[{'role': 'userAdminAnyDatabase', 'db': 'admin'}])
with MongoClient('mongodb://127.0.0.1:57017', username='scale_lab_admin',
                 password=password, authSource='admin', serverSelectionTimeoutMS=10000) as client:
    client.admin.command('createUser', 'scale_lab', pwd=password,
                         roles=[{'role': 'readWrite', 'db': 'scale_lab'}])
print('MongoDB benchmark user created')
