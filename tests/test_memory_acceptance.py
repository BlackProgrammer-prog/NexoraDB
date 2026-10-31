import hashlib
import importlib.util
import json
from pathlib import Path

import pytest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('mem007_acceptance',ROOT/'scripts/verify_memory_acceptance.py')
acceptance=importlib.util.module_from_spec(spec);spec.loader.exec_module(acceptance)


def write(path,value):
    path.parent.mkdir(parents=True,exist_ok=True);path.write_text(json.dumps(value))


@pytest.fixture
def evidence(tmp_path):
    """Synthetic evidence documents exercise gates; this does not execute 10M."""
    config=dict(records=10_000_000,repetitions=2,validation_mode='full',databases=['nexoradb'],contain_memory=True)
    cells=[dict(workload=w,clients=c,working_set=1.0) for w in ('ycsb-a','ycsb-b','ycsb-c','indexed-read') for c in (1,10,50)]
    source=tmp_path/'source/fake.py';source.parent.mkdir();source.write_text('fixture')
    source_hash=hashlib.sha256(b'fake.py\0'+hashlib.sha256(source.read_bytes()).digest()).hexdigest()
    manifest=dict(config=config,config_sha256=hashlib.sha256(json.dumps(config,sort_keys=True).encode()).hexdigest(),
                  source_sha256=source_hash,status='completed',schedule=[dict(id=f'{i:04}',repetition=i+1,cells=cells) for i in range(2)])
    write(tmp_path/'manifest.json',manifest)
    baseline=dict(algorithm='sha256-sum256-xor256-v1',records=10_000_000,sum256='1'*64,xor256='2'*64)
    validation=dict(mode='full-stream-every-field',verified_records=10_000_000,actual=baseline,baseline=baseline)
    for job in manifest['schedule']:
        directory=tmp_path/('job-'+job['id'])/'attempt-0000'
        write(directory/'checkpoint.json',dict(status='completed',private_store_removed=True))
        write(directory/'ingestion.json',dict(baseline_digest=baseline,validation=validation))
        binary=directory/'native-binary/fixture.so';binary.parent.mkdir();binary.write_bytes(b'fixture')
        write(directory/'engine.json',dict(native_binary_sha256=hashlib.sha256(b'fixture').hexdigest(),
            native_binary_artifact=str(binary),native_build=dict(build_type='Release'),allocator_policy=dict(native_trim=False)))
        for index in range(12):
            write(directory/f'cell-{index:04}'/'measurement.json',dict(status='verified-and-restored',counts=dict(attempted=1,failed=0),
                post_validation=validation,restored_validation=validation))
        write(directory/'close.json',dict(status='closed',total_seconds=.2,after_prepare_native=dict(
            page_live_snapshots=0,active_query_executions=0,execution_budget=dict(reserved_bytes=0))))
        rows=[]
        for index in range(12):
            rows.append(dict(monotonic_seconds=index*60,deployment_memory=dict(pss_bytes=1024,rss_bytes=2048),
                memory_containment=dict(worker=dict(events=dict(high=0,max=0,oom=0,oom_kill=0),
                    current_bytes=4096,anon_bytes=2048,swap_current_bytes=0,max_bytes=str(8*1024**3),
                    pressure='full avg10=0.00 avg60=0.00 avg300=0.00 total=0')),
                native=dict(page_live_snapshots=index%2)))
        (directory/'job-resources.jsonl').write_text(''.join(json.dumps(row)+'\n' for row in rows))
    return tmp_path


def test_completed_and_integrity_still_do_not_approve_release(evidence):
    report=acceptance.audit(evidence)
    assert report['status']=='accepted_10m_limited'
    assert report['approved_records']==10_000_000 and not report['release_ready']


@pytest.mark.parametrize('fault',['integrity','snapshot','binary','source','close','oom'])
def test_each_failed_gate_rejects_completed_manifest(evidence,fault):
    job=evidence/'job-0000/attempt-0000'
    if fault=='integrity':
        path=job/'cell-0000/measurement.json';value=acceptance.read(path);value['post_validation']['verified_records']=1;write(path,value)
    elif fault=='snapshot':
        path=job/'close.json';value=acceptance.read(path);value['after_prepare_native']['page_live_snapshots']=1;write(path,value)
    elif fault=='close':
        path=job/'close.json';value=acceptance.read(path);value['total_seconds']=31;write(path,value)
    elif fault=='source': (evidence/'source/fake.py').write_text('changed')
    elif fault=='binary': (job/'native-binary/fixture.so').write_bytes(b'changed')
    else:
        path=job/'job-resources.jsonl';rows=list(acceptance.lines(path));rows[-1]['memory_containment']['worker']['events']['oom']=1
        path.write_text(''.join(json.dumps(row)+'\n' for row in rows))
    assert acceptance.audit(evidence)['status']=='rejected'


def test_high_event_and_growth_are_review_never_silently_accepted(evidence):
    path=evidence/'job-0000/attempt-0000/job-resources.jsonl';rows=list(acceptance.lines(path))
    for index,row in enumerate(rows):
        row['deployment_memory']['pss_bytes']+=index*100*1024**2
        row['memory_containment']['worker']['events']['high']=index
    path.write_text(''.join(json.dumps(row)+'\n' for row in rows))
    report=acceptance.audit(evidence)
    assert report['status']=='review_required' and report['approved_records'] is None
    assert report['jobs'][0]['pss_growth']['status']=='review'
