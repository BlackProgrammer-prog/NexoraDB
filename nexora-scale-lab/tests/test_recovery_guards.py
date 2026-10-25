import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/recover_cleanup.py'
spec = importlib.util.spec_from_file_location('cleanup_recovery_guards', SCRIPT)
recovery = importlib.util.module_from_spec(spec)
spec.loader.exec_module(recovery)


class RecoveryGuardTests(unittest.TestCase):
    def test_only_directly_owned_private_trial_is_eligible(self):
        with tempfile.TemporaryDirectory() as folder:
            lab = Path(folder)
            root = lab / 'state/protocol'
            store = root / 'trial_owned'
            store.mkdir(parents=True)
            attempt = lab / 'results/study/job-0001/attempt-0000'
            owner = {'job_dir': str(attempt), 'namespace': 'scale_lab_test'}
            (store / 'owner.json').write_text(json.dumps(owner))
            checkpoint = {'private_store': str(store), 'namespace': owner['namespace']}
            with patch.object(recovery, 'LAB', lab):
                self.assertEqual(recovery.private_store(attempt, checkpoint), store)
                with self.assertRaises(RuntimeError):
                    recovery.private_store(attempt, {**checkpoint, 'private_store': str(root)})
                with self.assertRaises(RuntimeError):
                    recovery.private_store(attempt, {**checkpoint, 'private_store': str(lab)})
                with self.assertRaises(RuntimeError):
                    recovery.private_store(attempt, {**checkpoint, 'namespace': 'wrong'})
                link = root / 'trial_alias'
                link.symlink_to(store, target_is_directory=True)
                with self.assertRaises(RuntimeError):
                    recovery.private_store(attempt, {**checkpoint, 'private_store': str(link)})
            self.assertTrue((store / 'owner.json').exists())

    def test_redirected_state_root_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            lab = Path(folder)
            (lab / 'state').mkdir()
            outside = lab / 'unrelated'
            store = outside / 'trial_looks_owned'
            store.mkdir(parents=True)
            (lab / 'state/protocol').symlink_to(outside, target_is_directory=True)
            with patch.object(recovery, 'LAB', lab), self.assertRaises(RuntimeError):
                recovery.private_store(lab / 'results/attempt', {'private_store': str(store)})
