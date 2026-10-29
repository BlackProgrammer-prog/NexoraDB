"""COR-001 real native boundaries. No server or large dataset is needed."""
import json
import tempfile
import unittest
import nexoradb


class PaginationContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='nexora-pagination-')
        cls.engine = nexoradb.DocEngine(cls.directory.name + '/db')
        assert cls.engine.create_collection('items').success
        docs = [json.dumps({'_id': str(i), 'value': i, 'text': '[]{}\\"\n✓', 'tags': [1, {'ok': True}]}) for i in range(2503)]
        assert cls.engine.insert_many('items', docs).success
        assert cls.engine.create_collection('empty').success

    @classmethod
    def tearDownClass(cls):
        assert cls.engine.prepare_shutdown().success
        del cls.engine
        cls.directory.cleanup()

    def test_constants_and_early_invalid_ranges(self):
        self.assertEqual(nexoradb.PAGE_MAX_RECORDS, 1000)
        self.assertEqual(nexoradb.PAGE_DEFAULT_BYTES, 4 * 1024**2)
        self.assertEqual(nexoradb.PAGE_MAX_BYTES, 64 * 1024**2)
        for limit in (0, 1001, 4096):
            self.assertFalse(self.engine.find_page('empty', limit=limit).success)
            with self.assertRaises(ValueError):
                self.engine.iter_documents('missing', page_size=limit)
            with self.engine.iter_documents('items') as cursor:
                with self.assertRaises(ValueError):
                    cursor.next_ndjson_batch(limit)
                self.assertGreater(cursor.next_ndjson_batch(1)[1], 0)
        for budget in (0, 1, nexoradb.PAGE_MAX_BYTES + 1):
            self.assertFalse(self.engine.find_page('empty', max_page_bytes=budget).success)
            with self.assertRaises(ValueError):
                self.engine.iter_documents('missing', max_page_bytes=budget)

    def test_empty_and_byte_boundaries(self):
        for budget in (2, nexoradb.PAGE_DEFAULT_BYTES, nexoradb.PAGE_MAX_BYTES):
            result = self.engine.find_page('empty', limit=1000, max_page_bytes=budget)
            self.assertTrue(result.success)
            self.assertEqual(result.data, '[]')
            self.assertEqual(result.continuation_token, '')
            with self.engine.iter_documents('empty', page_size=1000, max_page_bytes=budget) as stream:
                self.assertEqual(list(stream), [])
        with self.engine.iter_documents('empty', page_size=1000, max_page_bytes=2) as stream:
            self.assertEqual(stream.next_ndjson_batch(1000), (b'', 0))

    def test_object_and_ndjson_complete_without_duplicates(self):
        for page_size in (1, 17, 1000):
            with self.engine.iter_documents('items', page_size=page_size, max_page_bytes=4096) as cursor:
                rows = list(cursor)
            self.assertEqual(len(rows), 2503)
            self.assertEqual({row['_id'] for row in rows}, {str(i) for i in range(2503)})
        with self.engine.iter_documents('items', page_size=1000, max_page_bytes=4096) as cursor:
            rows = []
            while True:
                data, count = cursor.next_ndjson_batch(1000)
                if not count:
                    break
                batch = [json.loads(line)['document'] for line in data.splitlines()]
                self.assertEqual(count, len(batch))
                self.assertLessEqual(len(data), 4096 + 13 * count - 1)
                rows.extend(batch)
        self.assertEqual(len(rows), 2503)
        self.assertEqual({row['_id'] for row in rows}, {str(i) for i in range(2503)})
        self.assertTrue(all(row['tags'] == [1, {'ok': True}] for row in rows))

    def test_direct_pages_close_and_preserve_token_on_bad_arguments(self):
        page = self.engine.find_page('items', limit=1)
        token = page.continuation_token
        self.assertTrue(token)
        self.assertFalse(self.engine.find_page('items', limit=4096, continuation_token=token).success)
        second = self.engine.find_page('items', limit=1, continuation_token=token, max_page_bytes=4096)
        self.assertTrue(second.success)
        self.assertNotEqual(page.data, second.data)
        self.engine.close_page(second.continuation_token)
        self.engine.close_page(second.continuation_token)
        self.assertFalse(self.engine.find_page('items', continuation_token=second.continuation_token).success)

    def test_oversized_valid_document_requires_larger_budget(self):
        self.assertTrue(self.engine.create_collection('large').success)
        document = {'_id': 'big', 'payload': 'x' * (4 * 1024**2)}
        self.assertTrue(self.engine.insert_one('large', json.dumps(document)).success)
        self.assertFalse(self.engine.find_page('large').success)
        with self.engine.iter_documents('large') as stream:
            with self.assertRaisesRegex(RuntimeError, 'byte budget'):
                next(stream)
        with self.engine.iter_documents('large', max_page_bytes=8 * 1024**2) as stream:
            self.assertEqual(list(stream), [document])
        with self.engine.iter_documents('large') as stream:
            with self.assertRaisesRegex(RuntimeError, 'byte budget'):
                stream.next_ndjson_batch(1000)

    def test_revoked_native_session_is_error_not_successful_end(self):
        with tempfile.TemporaryDirectory(prefix='nexora-revoked-page-') as directory:
            engine = nexoradb.DocEngine(directory + '/db')
            self.assertTrue(engine.create_collection('items').success)
            self.assertTrue(engine.insert_many('items', ['{"_id":"a"}', '{"_id":"b"}']).success)
            with engine.iter_documents('items', page_size=1) as stream:
                self.assertEqual(next(stream)['_id'], 'a')
                self.assertEqual(json.loads(engine.memory_report())['page_live_snapshots'], 1)
                # Shutdown revokes the registry, just as expiry does; no public test clock.
                self.assertTrue(engine.prepare_shutdown().success)
                with self.assertRaisesRegex(RuntimeError, 'Expired or invalid'):
                    next(stream)
            self.assertEqual(json.loads(engine.memory_report())['page_live_snapshots'], 0)
            del engine


if __name__ == '__main__':
    unittest.main()
