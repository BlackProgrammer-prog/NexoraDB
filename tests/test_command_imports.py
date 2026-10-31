"""Server startup must not import the terminal UI, even in full dev environments."""
from pathlib import Path
import importlib.util
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
BLOCK_TEXTUAL = '''
import importlib.abc
import sys
sys.path.insert(0, sys.argv[1])
class NoTextual(importlib.abc.MetaPathFinder):
    def find_spec(self, fullname, path=None, target=None):
        if fullname == "textual" or fullname.startswith("textual."):
            raise ModuleNotFoundError("Textual intentionally unavailable", name=fullname)
sys.meta_path.insert(0, NoTextual())
'''


@pytest.mark.parametrize('command', [
    'from nexoradb.commands.server import ControlledServer; assert ControlledServer.__name__ == "ControlledServer"',
    'from nexoradb.commands.server import main; main(["--help"])',
    'from nexoradb.commands import main; main(["--help"])',
    '''from nexoradb.commands import main
assert "nexoradb.cli.main" not in sys.modules
try:
    main(["cli"])
except ModuleNotFoundError as error:
    assert error.name == "textual"
else:
    raise AssertionError("CLI dispatch did not load its Textual dependency")
''',
])
def test_server_and_help_without_textual(command):
    result = subprocess.run([sys.executable, '-c', BLOCK_TEXTUAL+'\n'+command,
                             str(ROOT/'NexoraDB/src')], cwd=ROOT,
                            capture_output=True, text=True, timeout=15)
    assert result.returncode == 0, result.stdout+result.stderr


def test_probe_reports_child_stderr_and_exit_code(tmp_path):
    spec = importlib.util.spec_from_file_location('signal_probe_import_test', ROOT/'scripts/verify_execution_lifecycle.py')
    probe = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(probe)
    (tmp_path/'stderr.log').write_text("ModuleNotFoundError: No module named 'textual'\n")
    class Exited:
        def poll(self): return 7
    with pytest.raises(AssertionError) as failure:
        probe.check_server_running(Exited(), tmp_path)
    message = str(failure.value)
    assert 'exit 7' in message and 'textual' in message and str(tmp_path/'stderr.log') in message
