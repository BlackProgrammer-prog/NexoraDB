"""COR-006 independent, deliberately small JSON document/point-transaction model.

No engine import, index codec, parser, storage implementation or evaluator is
used here. Concurrency and FK histories belong to transaction_history.py/tests.
"""
from copy import deepcopy
from fractions import Fraction
import random

MISSING=object()
VALUES=[None,False,True,0,-0.0,1,1.0,-2,2.5,2**53,2**53+1,
        -(2**63),2**63-1,0.125,'','alpha','فارسی','a\x00b',[1,2],{'nested':True}]


def equal(left,right):
    if left is MISSING or right is MISSING: return False
    if left is None or right is None: return left is None and right is None
    if isinstance(left,bool) or isinstance(right,bool): return type(left) is bool and type(right) is bool and left==right
    if type(left) in (int,float) and type(right) in (int,float):
        return Fraction(left)==Fraction(right)
    return type(left) is str and type(right) is str and left==right


class Model:
    def __init__(self): self.rows={}

    def apply(self,operation):
        kind=operation['op']
        if kind=='insert':
            row=operation['document'];key=row['_id']
            if key in self.rows: return False
            self.rows[key]=deepcopy(row);return True
        if kind=='bulk':
            rows=operation['documents'];keys=[row['_id'] for row in rows]
            if not rows or len(set(keys))!=len(keys) or any(key in self.rows for key in keys): return False
            self.rows.update({row['_id']:deepcopy(row) for row in rows});return True
        if kind=='set':
            key=operation['id']
            if key not in self.rows: return False
            self.rows[key]['v']=deepcopy(operation['value']);return True
        if kind=='delete': return int(self.rows.pop(operation['id'],None) is not None)
        if kind=='query':
            return sorted(key for key,row in self.rows.items() if equal(row.get('v',MISSING),operation['value']))
        if kind=='tx':
            saved=deepcopy(self.rows)
            result=[self.apply(action) for action in operation['actions']]
            if not operation['commit']: self.rows=saved
            return result
        raise ValueError(f'unknown model operation {kind}')


def generate(seed,steps):
    rng=random.Random(seed)
    def action():
        kind=rng.choice(['insert','set','delete','query','bulk'])
        key=str(rng.randrange(16));value=deepcopy(rng.choice(VALUES))
        if kind=='insert':
            row={'_id':key,'payload':{'text':'✓\x00','array':[1,False,None]}}
            if rng.randrange(5): row['v']=value
            return {'op':kind,'document':row}
        if kind=='bulk':
            return {'op':kind,'documents':[{'_id':str(rng.randrange(16)),'v':deepcopy(rng.choice(VALUES))} for _ in range(3)]}
        if kind=='set': return {'op':kind,'id':key,'value':value}
        if kind=='delete': return {'op':kind,'id':key}
        # Scalar EQ is the supported query subset, not structural JSON equality.
        return {'op':kind,'value':deepcopy(rng.choice(VALUES[:-2]))}
    result=[]
    for _ in range(steps):
        if rng.randrange(5)==0:
            actions=[]
            for _ in range(rng.randrange(1,5)):
                item=action()
                if item['op'] in ('query','bulk'): item={'op':'delete','id':str(rng.randrange(16))}
                actions.append(item)
            result.append({'op':'tx','commit':bool(rng.randrange(2)),'actions':actions})
        else: result.append(action())
    return result


def shrink(sequence,fails):
    """Deletion-based 1-minimizer; caller preserves failure identity.

    Transaction groups are indivisible first, then their action lists shrink.
    Does not claim globally shortest or minimal scalar values.
    """
    result=deepcopy(sequence)
    changed=True
    while changed:
        changed=False
        for index in range(len(result)):
            candidate=result[:index]+result[index+1:]
            if fails(candidate): result=candidate;changed=True;break
    for index,item in enumerate(result):
        if item['op']=='tx':
            def inner(actions):
                candidate=deepcopy(result);candidate[index]['actions']=actions
                return fails(candidate)
            actions=deepcopy(item['actions'])
            changed=True
            while changed:
                changed=False
                for position in range(len(actions)):
                    candidate=actions[:position]+actions[position+1:]
                    if inner(candidate): actions=candidate;changed=True;break
            result[index]['actions']=actions
    return result
