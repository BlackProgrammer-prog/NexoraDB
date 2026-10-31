"""Independent oracle for bounded, completion-ordered point-operation histories.

Not a checker for arbitrary overlapping invocation intervals, range predicates,
distributed consistency or power-loss durability. Values are JSON scalars.
"""
from copy import deepcopy
from itertools import permutations


def check_read_committed(initial, events):
    committed, active, seen = deepcopy(initial), {}, set()
    for position, event in enumerate(events):
        tx, op = event['tx'], event['op']
        if op == 'begin':
            if tx in seen:
                raise ValueError(f'duplicate begin at {position}')
            seen.add(tx)
            active[tx] = {}
            continue
        if tx not in active:
            raise ValueError(f'inactive transaction at {position}')
        staged = active[tx]
        if op == 'read':
            expected = staged.get(event['key'], committed.get(event['key']))
            if expected != event['value']:
                return {'valid': False, 'position': position, 'expected': expected, 'observed': event['value']}
        elif op == 'write':
            staged[event['key']] = deepcopy(event['value'])
        elif op == 'commit':
            committed.update(staged)
            del active[tx]
        elif op == 'abort':
            del active[tx]
        else:
            raise ValueError(f'unknown operation: {op}')
    if active:
        raise ValueError('history contains unfinished transactions')
    return {'valid': True, 'state': committed}


def check_strict_serializable(initial, events):
    # Exhaustive independent oracle; deliberately bounded to avoid factorial
    # runtime accidentally becoming an unbounded benchmark.
    check_read_committed(initial, events)  # Validate event grammar first.
    begin, end, operations = {}, {}, {}
    for position, event in enumerate(events):
        tx = event['tx']
        if event['op'] == 'begin':
            if tx in begin:
                raise ValueError('transaction IDs must be unique')
            begin[tx], operations[tx] = position, []
        elif event['op'] == 'commit':
            end[tx] = position
        elif event['op'] in ('read', 'write'):
            operations[tx].append(event)
    if len(end) > 8:
        raise ValueError('serial oracle limited to eight committed transactions')
    for order in permutations(end):
        rank = {tx: i for i, tx in enumerate(order)}
        if any(end[a] < begin[b] and rank[a] > rank[b] for a in end for b in end):
            continue
        state, valid = deepcopy(initial), True
        for tx in order:
            for event in operations[tx]:
                if event['op'] == 'read' and state.get(event['key']) != event['value']:
                    valid = False
                    break
                if event['op'] == 'write':
                    state[event['key']] = deepcopy(event['value'])
            if not valid:
                break
        if valid:
            return {'valid': True, 'serial_order': order, 'state': state}
    return {'valid': False, 'reason': 'no legal serial order for committed transactions'}


if __name__=='__main__':
    import argparse
    import json
    from pathlib import Path
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--history',type=Path,required=True,help='JSON with initial and events keys')
    parser.add_argument('--isolation',choices=['read-committed','strict-serializable'],default='read-committed')
    args=parser.parse_args()
    history=json.loads(args.history.read_text(encoding='utf-8'))
    oracle=check_read_committed if args.isolation=='read-committed' else check_strict_serializable
    result=oracle(history['initial'],history['events'])
    print(json.dumps(result,ensure_ascii=False))
    raise SystemExit(0 if result['valid'] else 1)
