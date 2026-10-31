"""Independent oracle self-tests and small generated native differential suite."""
from pathlib import Path
import sys
import pytest
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from reference_model import Model,equal,generate,shrink
from verify_reference_model import canonical,run_sequence,load_native_module,AdminApiSettings
import json
import verify_reference_model as runner


def test_oracle_has_independent_numeric_missing_and_type_semantics():
    assert equal(1,1.0) and equal(0,-0.0)
    assert not equal(True,1) and not equal('1',1)
    assert not equal(2**53+1,float(2**53))
    assert not equal([1],[1]) and not equal({},'{}')
    model=Model()
    assert model.apply({'op':'insert','document':{'_id':'a'}})
    assert model.apply({'op':'query','value':None})==[]
    assert model.apply({'op':'bulk','documents':[{'_id':'b'},{'_id':'a'}]}) is False
    assert set(model.rows)=={'a'}


def test_shrinker_preserves_failure_and_removes_irrelevant_operations():
    original=[{'op':'delete','id':'x'},{'op':'insert','document':{'_id':'bad'}},{'op':'query','value':0}]
    def fails(sequence): return any(item['op']=='insert' for item in sequence)
    result=shrink(original,fails)
    assert result==[original[1]] and len(original)==3
    for index in range(len(result)): assert not fails(result[:index]+result[index+1:])


def test_failure_artifacts_and_replay_keep_seed_and_same_failure(monkeypatch,tmp_path):
    corpus={'seed':123,'operations':[{'op':'delete','id':'irrelevant'},
                                  {'op':'insert','document':{'_id':'trigger'}}]}
    path=tmp_path/'input.json';path.write_text(json.dumps(corpus))
    def faulty(native,sequence,parent=None):
        if any(item['op']=='insert' for item in sequence): raise runner.Mismatch('injected','self-test only')
        return {'trace':[],'state':{}}
    monkeypatch.setattr(runner,'run_sequence',faulty)
    for name,replay in [('first',path),('again',tmp_path/'first/minimized.json')]:
        output=tmp_path/name
        monkeypatch.setattr(sys,'argv',['verify_reference_model','--directory',str(output),'--replay',str(replay)])
        with pytest.raises(runner.Mismatch,match='injected'): runner.main()
        assert json.loads((output/'report.json').read_text())['status']=='failed'
        minimized=json.loads((output/'minimized.json').read_text())
        assert minimized['seed']==123 and minimized['failure']=='injected'
        assert minimized['operations']==[corpus['operations'][1]]


@pytest.mark.parametrize('seed',range(505006,505014))
def test_seeded_bulk_single_scan_index_replay_codec_and_transactions(seed,tmp_path):
    native=load_native_module(AdminApiSettings())
    sequence=generate(seed,40)
    assert sequence==generate(seed,40)
    assert canonical(run_sequence(native,sequence,tmp_path))==canonical(run_sequence(native,sequence,tmp_path))


def test_reference_regression_corpus(tmp_path):
    # Real failures are promoted here after minimization. No XFAIL or skip:
    # every checked-in case must agree with the independent oracle.
    paths=sorted((ROOT/'tests/corpus/reference').glob('*.json'))
    assert paths,'The versioned contract regression corpus must not silently disappear'
    native=load_native_module(AdminApiSettings())
    for path in paths:
        case=json.loads(path.read_text(encoding='utf-8'))
        run_sequence(native,case['operations'],tmp_path)
