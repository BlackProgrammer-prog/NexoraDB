"""Short COR-006 oracle runner. Only creates owned temporary native stores."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import tempfile
from copy import deepcopy

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'NexoraDB/src'))
from reference_model import Model,generate,shrink
from nexoradb_admin.config import AdminApiSettings
from nexoradb_admin.native import load_native_module


class Mismatch(AssertionError):
    def __init__(self,code,detail): self.code=code;super().__init__(f'{code}: {detail}')


def check(condition,code,detail):
    if not condition: raise Mismatch(code,detail)


def canonical(value):
    return json.dumps(value,ensure_ascii=False,sort_keys=True,separators=(',',':'),allow_nan=False)


def run_sequence(native,sequence,parent=None):
    with tempfile.TemporaryDirectory(prefix='nexora-reference-',dir=parent) as directory:
        engine=native.DocEngine(str(Path(directory)/'db'))
        handles=[]
        try:
            for collection in ('scan','indexed','single'):
                check(engine.create_collection(collection).success,'setup',collection)
            index=native.IndexDefinition();index.index_name='by_v';index.fields=['v']
            check(engine.create_index('indexed',index).success,'setup','index')
            model=Model();trace=[]
            def execute(collection,operation,tx=None):
                kind=operation['op']
                if kind=='tx':
                    tx=engine.begin_transaction();handles.append(tx)
                    try:
                        results=[execute(collection,item,tx) for item in operation['actions']]
                        closed=engine.commit_transaction(tx) if operation['commit'] else engine.rollback_transaction(tx)
                        check(closed.success,'transaction_close',closed.error_msg)
                        return results
                    finally:
                        if tx.is_valid(): engine.rollback_transaction(tx)
                if kind=='query':
                    value=operation['value']
                    if value is None: value_type=native.ValueType.Null;constant=''
                    elif type(value) is bool: value_type=native.ValueType.Bool;constant=str(value).lower()
                    elif type(value) is int: value_type=native.ValueType.Int64;constant=str(value)
                    elif type(value) is float: value_type=native.ValueType.Float64;constant=repr(value)
                    else: value_type=native.ValueType.String;constant=value
                    condition=native.Condition.leaf('v',native.Op.EQ,constant,value_type)
                    result=engine.find_many(collection,condition,1000)
                    check(result.success,'query_error',result.error_msg)
                    return sorted(row['_id'] for row in json.loads(result.data))
                if kind=='bulk':
                    if collection=='single':
                        # Separate single-write oracle only when conflict-free;
                        # otherwise use a transaction to preserve bulk atomicity.
                        tx=engine.begin_transaction();handles.append(tx)
                        try:
                            ok=all(engine.insert_one_tx(tx,collection,canonical(row)).success for row in operation['documents'])
                            closed=engine.commit_transaction(tx) if ok else engine.rollback_transaction(tx)
                            check(closed.success,'bulk_single_close',closed.error_msg)
                            return ok
                        finally:
                            if tx.is_valid(): engine.rollback_transaction(tx)
                    return engine.insert_many(collection,[canonical(row) for row in operation['documents']]).success
                if kind=='insert':
                    args=(collection,canonical(operation['document']))
                    result=engine.insert_one_tx(tx,*args) if tx else engine.insert_one(*args)
                elif kind=='set':
                    value=operation['value']
                    mapping={str:native.UpdateValueType.String,int:native.UpdateValueType.Int64,
                             float:native.UpdateValueType.Float64,bool:native.UpdateValueType.Bool,
                             list:native.UpdateValueType.Array,dict:native.UpdateValueType.Object,type(None):native.UpdateValueType.Null}
                    text=value if type(value) is str else canonical(value)
                    spec=native.UpdateSpec().set('v',text,mapping[type(value)])
                    args=(collection,operation['id'],spec)
                    result=engine.update_by_id_tx(tx,*args) if tx else engine.update_by_id(*args)
                elif kind=='delete':
                    args=(collection,operation['id'])
                    result=engine.delete_by_id_tx(tx,*args) if tx else engine.delete_by_id(*args)
                    check(result.success,'delete_error',result.error_msg)
                    return int(result.data)
                else: raise ValueError(kind)
                return result.success

            for position,operation in enumerate(sequence):
                expected=model.apply(operation)
                actual=[execute(collection,operation) for collection in ('scan','indexed','single')]
                check(all(value==expected for value in actual),'operation_result',{'position':position,'operation':operation,'expected':expected,'actual':actual})
                # Snapshot export also tests codec roundtrip, type/Unicode/NUL,
                # counters, rollback visibility and absence of partial writes.
                for collection in ('scan','indexed','single'):
                    with engine.iter_documents(collection,page_size=7) as cursor:
                        rows={row['_id']:row for row in cursor}
                    check(canonical(rows)==canonical(model.rows),'stored_state',{'position':position,'collection':collection,'expected':model.rows,'actual':rows})
                    check(engine.get_collection_size(collection)==len(model.rows),'counter',position)
                trace.append(deepcopy(expected))
            check(engine.integrity_check().success,'integrity','final index/document verification')
            return {'trace':trace,'state':deepcopy(model.rows)}
        finally:
            for tx in handles:
                if tx.is_valid(): engine.rollback_transaction(tx)
            engine.prepare_shutdown()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seed',type=int,default=505006)
    parser.add_argument('--seeds',type=int,default=12)
    parser.add_argument('--steps',type=int,default=80)
    parser.add_argument('--directory',type=Path,required=True)
    parser.add_argument('--replay',type=Path,help='Replay original.json/minimized.json corpus')
    args=parser.parse_args()
    if not (1<=args.seeds<=100 and 1<=args.steps<=1000): parser.error('bounded run requires seeds 1..100 and steps 1..1000')
    args.directory.mkdir(parents=True,exist_ok=False)
    native=load_native_module(AdminApiSettings())
    binary=Path(getattr(native,'__native_module_path__',native.__file__))
    report={'schema':1,'scope':'sequential JSON point mutations; EQ; atomic bulk vs transactional singles; explicit commit/rollback',
            'native_path':str(binary),'native_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
            'model_sha256':hashlib.sha256((ROOT/'scripts/reference_model.py').read_bytes()).hexdigest(),
            'runner_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'cases':[]}
    cases=[json.loads(args.replay.read_text())] if args.replay else [
        {'seed':seed,'operations':generate(seed,args.steps)} for seed in range(args.seed,args.seed+args.seeds)]
    try:
        for case in cases:
            sequence=case['operations']
            corpus_name=f"corpus-{len(report['cases']):04d}.json"
            (args.directory/corpus_name).write_text(canonical(case)+'\n',encoding='utf-8')
            try:
                first=run_sequence(native,sequence,args.directory)
                second=run_sequence(native,sequence,args.directory)
                check(canonical(first)==canonical(second),'replay','same corpus on fresh independent store')
            except Mismatch as error:
                (args.directory/'original.json').write_text(canonical(case)+'\n',encoding='utf-8')
                def fails(candidate):
                    try: run_sequence(native,candidate,args.directory)
                    except Mismatch as other: return other.code==error.code
                    return False
                minimized=shrink(sequence,fails)
                (args.directory/'minimized.json').write_text(canonical({'seed':case.get('seed'),'operations':minimized,'failure':error.code})+'\n',encoding='utf-8')
                raise
            report['cases'].append({'seed':case.get('seed'),'operations':len(sequence),'corpus':corpus_name,'replay_verified':True})
        report['status']='passed'
    except Exception as error:
        report['status']='failed';report['error']=str(error)
        raise
    finally:
        (args.directory/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report,ensure_ascii=False))


if __name__=='__main__': main()
