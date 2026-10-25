import importlib.util
import json
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('release_version_tool', ROOT / 'scripts/release_version.py')
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)


def fixture_repo(tmp_path):
    for name in (tool.CANONICAL, 'CMakeLists.txt', 'NexoraDB/pyproject.toml',
                 'vcpkg.json', 'frontend/package.json', 'frontend/package-lock.json', 'README.md', 'NexoraDB/README.md'):
        destination = tmp_path / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / name, destination)
    return tmp_path


def test_bump_updates_all_consumers_and_is_idempotent(tmp_path):
    root = fixture_repo(tmp_path)
    original_lock = json.loads((root / 'frontend/package-lock.json').read_text())
    target = '99.4.0'  # Independent of the actual version of this checkout.
    version, updates = tool.plan(root, target)
    assert version == target
    assert len(updates) == 8
    for name, data in updates.items():
        (root / name).write_bytes(data)
    assert tool.plan(root) == (target, {})
    lock = json.loads((root / 'frontend/package-lock.json').read_text())
    assert lock['version'] == lock['packages']['']['version'] == target
    for name in original_lock['packages']:
        if name:
            assert lock['packages'][name] == original_lock['packages'][name]
    readme = (root / 'README.md').read_text()
    assert 'Release '+target+':' in readme
    assert 'docs/releases/v'+target+'.md' in readme
    assert 'pip install NexoraDataBase=='+target in readme
    package_readme = (root / 'NexoraDB/README.md').read_text()
    assert 'Current version: **'+target+'**' in package_readme
    assert '## Version 0.3.0 upgrade notice' in package_readme


def test_plan_rejects_invalid_version_without_writes(tmp_path):
    import pytest
    root = fixture_repo(tmp_path)
    before = {str(p): p.read_bytes() for p in root.rglob('*') if p.is_file()}
    for value in ('v0.4.0', '0.4', '00.4.0', '0.4.0rc1', '0.4.0\nextra'):
        with pytest.raises(ValueError):
            tool.plan(root, value)
    assert before == {str(p): p.read_bytes() for p in root.rglob('*') if p.is_file()}
