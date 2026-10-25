"""Bounded dependency regressions; no network or multi-GiB overflow allocation."""
import unittest
from unittest.mock import Mock

from bson import BSON
from gridfs import GridFS, GridFSBucket
import pymongo
from pymongo.errors import InvalidURI
from pymongo.uri_parser import parse_uri


class PyMongoSecurityTests(unittest.TestCase):
    def test_installed_driver_includes_security_patch_floor(self):
        self.assertGreaterEqual(pymongo.version_tuple, (4, 18, 3))
        self.assertLess(pymongo.version_tuple, (5,))

    def test_percent_encoded_host_delimiters_are_rejected(self):
        for host in ('trusted%2cattacker', 'trusted%2Cattacker', 'trusted%3A27017'):
            with self.subTest(host=host), self.assertRaises(InvalidURI):
                parse_uri('mongodb://' + host + ':27017/')
        self.assertEqual(parse_uri('mongodb://localhost:27017/')['nodelist'], [('localhost', 27017)])

    def test_gridfs_delete_treats_mapping_id_as_exact_value(self):
        for api in (GridFS, GridFSBucket):
            with self.subTest(api=api.__name__):
                storage = object.__new__(api)
                storage._timeout = None
                storage._files = Mock()
                storage._files.delete_one.return_value.deleted_count = 1
                storage._chunks = Mock()
                hostile_id = {'$ne': None}
                storage.delete(hostile_id)
                storage._files.delete_one.assert_called_once_with({'_id': {'$eq': hostile_id}}, session=None)
                storage._chunks.delete_many.assert_called_once_with({'files_id': {'$eq': hostile_id}}, session=None)

    def test_bson_roundtrip_keeps_application_types(self):
        value = {'_id': 'one', 'name': 'فارسی', 'nested': {'v': 42}, 'items': [True, None, 1.25]}
        self.assertEqual(BSON(BSON.encode(value)).decode(), value)
