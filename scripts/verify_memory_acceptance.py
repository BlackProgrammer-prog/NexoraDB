"""Read-only MEM-007 evidence audit. Completion is necessary, never sufficient."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics

POLICY = {
    'schema': 1, 'records': 10_000_000, 'repetitions': 2, 'cells_per_repetition': 12,
    'validation': 'full-stream-every-field', 'max_close_seconds': 30,
    'allowed_event_deltas': {'max': 0, 'oom': 0, 'oom_kill': 0, 'oom_group_kill': 0},
    'pressure_review': 'any high event, swap or nonzero full-memory PSI requires an explicit explanation; never auto-accept',
    'growth_review': 'last six completed native cursor boundaries: PSS or charged anon+swap grows >256MiB or >1MiB/min requires review',
    'scope': 'this 10M profile only; not 100M, leak freedom or a performance comparison',
    'release_prerequisite': 'RES-004 acceptance remains an independent gate',
}


def read(path): return json.loads(Path(path).read_text())


def lines(path):
    with path.open() as stream:
        for number, line in enumerate(stream, 1):
            try: yield json.loads(line)
            except json.JSONDecodeError as error: raise ValueError(f'{path}:{number}: corrupt telemetry') from error


def psi_full(text):
    for line in text.splitlines():
        fields=line.split()
        if fields and fields[0]=='full':
            return int(dict(field.split('=',1) for field in fields[1:])['total'])
    return None


def growth(points):
    points = points[-6:]
    if len(points) < 3: return {'status':'insufficient', 'samples':len(points)}
    times, values = zip(*points)
    slopes = [(values[j]-values[i])/((times[j]-times[i])/60)
              for i in range(len(points)) for j in range(i+1,len(points)) if times[j]>times[i]]
    slope = statistics.median(slopes) if slopes else 0
    increase = values[-1]-values[0]
    return dict(status='review' if increase>256*1024**2 or slope>1024**2 else 'bounded_observed',
                samples=len(points), increase_bytes=increase, theil_sen_bytes_per_minute=slope,
                caveat='sampled finite window; includes cache and allocator retention, not proof of live growth/leak')


def audit(directory):
    manifest = read(directory/'manifest.json')
    failures, reviews, jobs = [], [], []
    config = manifest['config']
    if hashlib.sha256(json.dumps(config,sort_keys=True).encode()).hexdigest()!=manifest.get('config_sha256'):
        failures.append('manifest config hash mismatch')
    source_digest=hashlib.sha256()
    for path in sorted((directory/'source').rglob('*')):
        if path.is_file():
            source_digest.update(str(path.relative_to(directory/'source')).encode()+b'\0'+hashlib.sha256(path.read_bytes()).digest())
    if source_digest.hexdigest()!=manifest.get('source_sha256'): failures.append('source snapshot hash mismatch')
    for key, expected in (('records',POLICY['records']),('repetitions',2),('validation_mode','full'),
                          ('databases',['nexoradb']),('contain_memory',True)):
        if config.get(key)!=expected: failures.append(f'config.{key} mismatch')
    if manifest.get('status')!='completed': failures.append('run not completed')
    schedule = manifest.get('schedule',[])
    if len(schedule)!=2: failures.append('requires two independent jobs')
    if sorted(j.get('repetition',0) for j in schedule)!=[1,2]: failures.append('repetition identities mismatch')
    hashes = set()
    for job in schedule:
        attempts = sorted((directory/('job-'+job['id'])).glob('attempt-*'))
        if not attempts:
            failures.append(job['id']+': missing attempt');continue
        # Never hide a failed attempt by selecting a successful retry.
        if len(attempts)!=1: failures.append(job['id']+': multiple attempts require separate review')
        attempt = attempts[-1]
        checkpoint = read(attempt/'checkpoint.json')
        if checkpoint.get('status')!='completed' or not checkpoint.get('private_store_removed'):
            failures.append(job['id']+': incomplete job/cleanup')
        if len(job.get('cells',[]))!=12: failures.append(job['id']+': missing workload cells')
        cells={(c['workload'],c['clients'],c['working_set']) for c in job.get('cells',[])}
        expected_cells={(w,c,1.0) for w in ('ycsb-a','ycsb-b','ycsb-c','indexed-read') for c in (1,10,50)}
        if cells!=expected_cells: failures.append(job['id']+': workload matrix mismatch')
        ingestion = read(attempt/'ingestion.json') if (attempt/'ingestion.json').exists() else {}
        baseline = ingestion.get('baseline_digest')
        if not isinstance(baseline,dict) or baseline.get('algorithm')!='sha256-sum256-xor256-v1' or baseline.get('records')!=10_000_000:
            failures.append(job['id']+': missing/invalid baseline digest')
        def validate(value, label, restored=False):
            if (value.get('mode')!='full-stream-every-field' or value.get('verified_records')!=10_000_000
                    or value.get('baseline')!=baseline): failures.append(job['id']+': invalid '+label)
            if restored and value.get('actual')!=baseline: failures.append(job['id']+': baseline restoration digest mismatch')
        validate(ingestion.get('validation',{}),'load validation',True)
        for index in range(len(job.get('cells',[]))):
            path = attempt/f'cell-{index:04d}'/'measurement.json'
            if not path.exists(): failures.append(job['id']+f': missing cell {index}');continue
            cell = read(path)
            if cell.get('status')!='verified-and-restored' or cell.get('counts',{}).get('failed',0) or not cell.get('counts',{}).get('attempted',0):
                failures.append(job['id']+f': failed cell {index}')
            validate(cell.get('post_validation',{}),f'cell {index} post validation')
            validate(cell.get('restored_validation',{}),f'cell {index} restored validation',True)
        engine = read(attempt/'engine.json') if (attempt/'engine.json').exists() else {}
        digest = engine.get('native_binary_sha256');hashes.add(digest)
        binary = Path(engine.get('native_binary_artifact','/missing'))
        if not binary.is_file() or hashlib.sha256(binary.read_bytes()).hexdigest()!=digest:
            failures.append(job['id']+': missing/mismatched native binary evidence')
        if engine.get('native_build',{}).get('build_type')!='Release': failures.append(job['id']+': not Release')
        if engine.get('allocator_policy',{}).get('native_trim') is not False:
            reviews.append(job['id']+': allocator intervention differs from observation-only policy')
        close = read(attempt/'close.json') if (attempt/'close.json').exists() else {}
        if close.get('status')!='closed' or close.get('total_seconds',float('inf'))>30:
            failures.append(job['id']+': close failed/unmeasured/exceeds 30 seconds')
        final = close.get('after_prepare_native',{})
        if final.get('page_live_snapshots')!=0 or final.get('active_query_executions')!=0:
            failures.append(job['id']+': readers/snapshots not drained')
        budget = final.get('execution_budget',{})
        if budget.get('reserved_bytes')!=0: failures.append(job['id']+': execution credits not released')
        telemetry = attempt/'job-resources.jsonl'
        start_events = end_events = None
        first_full=last_full=None
        peaks = dict(pss_bytes=0,rss_bytes=0,charged_bytes=0,anon_bytes=0,swap_bytes=0,snapshots=0)
        pss_points, anon_points = [], []
        last_phase = None
        # Stage timestamps align the stream without retaining every telemetry row.
        stages = list(lines(attempt/'stage-memory.jsonl')) if (attempt/'stage-memory.jsonl').exists() else []
        if telemetry.exists():
            samples = 0
            for row in lines(telemetry):
                samples+=1
                memory = row.get('deployment_memory',{})
                peaks['pss_bytes']=max(peaks['pss_bytes'],memory.get('pss_bytes',0))
                peaks['rss_bytes']=max(peaks['rss_bytes'],memory.get('rss_bytes',0))
                group = row.get('memory_containment',{}).get('worker')
                if group:
                    events = group['events']
                    if start_events is None: start_events=events
                    end_events=events
                    full=psi_full(group.get('pressure',''))
                    if full is not None:
                        if first_full is None: first_full=full
                        last_full=full
                    for output, source in (('charged_bytes','current_bytes'),('anon_bytes','anon_bytes'),('swap_bytes','swap_current_bytes')):
                        peaks[output]=max(peaks[output],group.get(source,0))
                    if group.get('max_bytes')!=str(8*1024**3): failures.append(job['id']+': hard memory limit changed');break
                native = row.get('native') or {}
                peaks['snapshots']=max(peaks['snapshots'],native.get('page_live_snapshots',0))
                # Native snapshots returning to zero identify completed validation
                # boundaries. Native telemetry is not added to PSS/RSS.
                phase = native.get('page_live_snapshots',0)
                if last_phase and not phase and group:
                    pss_points.append((row['monotonic_seconds'],memory.get('pss_bytes',0)))
                    anon_points.append((row['monotonic_seconds'],group.get('anon_bytes',0)+group.get('swap_current_bytes',0)))
                    pss_points=pss_points[-6:];anon_points=anon_points[-6:]
                last_phase=phase
            if samples<2: failures.append(job['id']+': insufficient whole-job telemetry')
        else: failures.append(job['id']+': whole-job telemetry missing')
        delta = {k:end_events.get(k,0)-start_events.get(k,0) for k in set(start_events or {})|set(end_events or {})} if start_events and end_events else {}
        if not delta: failures.append(job['id']+': cgroup event evidence missing')
        for key in POLICY['allowed_event_deltas']:
            if delta.get(key,0)>0: failures.append(job['id']+': memory event '+key)
        if delta.get('high',0)>0 or peaks['swap_bytes']>0: reviews.append(job['id']+': high/reclaim/swap needs explanation')
        full_delta=last_full-first_full if first_full is not None and last_full is not None else None
        if full_delta is None: failures.append(job['id']+': full memory PSI missing')
        elif full_delta>0: reviews.append(job['id']+': full memory PSI requires explanation')
        pss_growth, charged_growth = growth(pss_points),growth(anon_points)
        if any(g['status']!='bounded_observed' for g in (pss_growth,charged_growth)):
            reviews.append(job['id']+': growth trend requires review')
        jobs.append(dict(id=job['id'],peaks=peaks,event_deltas=delta,close=close,
                         pss_growth=pss_growth,charged_anon_swap_growth=charged_growth,
                         stage_samples=len(stages),full_memory_stall_microseconds=full_delta))
    if len(hashes)!=1 or None in hashes: failures.append('native hashes differ or are missing between repetitions')
    return dict(schema=1,policy=POLICY,status='rejected' if failures else 'review_required' if reviews else 'accepted_10m_limited',
                failures=failures,reviews=reviews,jobs=jobs,approved_records=10_000_000 if not failures and not reviews else None,
                release_ready=False,release_reason='Independent RES-004 gate must be satisfied; memory acceptance alone is insufficient')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory',type=Path)
    parser.add_argument('--policy',action='store_true')
    parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    result=POLICY if args.policy else audit(args.directory.resolve())
    encoded=json.dumps(result,indent=2,allow_nan=False)+'\n'
    if args.output:
        with args.output.open('x') as stream: stream.write(encoded)
    else: print(encoded,end='')
