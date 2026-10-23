"""Bounded syntax-cache A/B diagnostic; parameter binding remains in every call."""
import argparse
import importlib
import json
from pathlib import Path
import statistics
import sys
import time

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'NexoraDB/src'))
from nexoradb_admin.query_runner import _load_nexoraql_package


def main():
    command=argparse.ArgumentParser()
    command.add_argument('--output',type=Path,required=True)
    args=command.parse_args()
    if args.output.exists():
        command.error('Output exists; preserve previous results')
    package=_load_nexoraql_package()
    parser=importlib.import_module(package.__name__+'.parser')
    query='SELECT * FROM users WHERE age >= $minimum AND country = $country LIMIT 10;'
    parser.configure_parser_cache(False,clear=True)
    package.parse(query,{'minimum':18,'country':'IR'})  # Exclude one-time Lark construction.
    rows=[]
    for repetition in range(3):
        for enabled in ((False,True) if repetition%2==0 else (True,False)):
            parser.configure_parser_cache(enabled,clear=True)
            started=time.perf_counter()
            for i in range(200):
                statement=package.parse(query,{'minimum':i,'country':'IR'})[0]
                assert statement.where.subs[0].value==i
            elapsed=time.perf_counter()-started
            rows.append(dict(enabled=enabled,repetition=repetition,calls=200,
                             seconds=elapsed,calls_per_second=200/elapsed,
                             cache=parser.parser_cache_stats()))
    parser.configure_parser_cache(True,clear=True)
    result=dict(scope='syntax parsing/binding only, not database throughput',rows=rows,
                median_calls_per_second={str(enabled):statistics.median(row['calls_per_second'] for row in rows
                                            if row['enabled']==enabled) for enabled in (False,True)})
    args.output.parent.mkdir(parents=True,exist_ok=True)
    with args.output.open('x') as output:
        json.dump(result,output,indent=2)
    print(json.dumps(result['median_calls_per_second'],indent=2))


if __name__=='__main__':
    main()
