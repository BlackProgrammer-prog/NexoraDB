import pytest
from app.api.v1.models.query import Condition, Op, LogicOp, ValueType


class TestCondition:
    def test_leaf_condition(self):
        cond = Condition.leaf("age", Op.GT, "18", ValueType.INT64)
        assert cond.is_leaf()
        assert not cond.is_composite()
        assert not cond.is_empty()
        assert cond.field == "age"
        assert cond.op == Op.GT
    
    def test_empty_condition(self):
        cond = Condition()
        assert cond.is_empty()
    
    def test_composite_and(self):
        c1 = Condition.leaf("age", Op.GT, "18")
        c2 = Condition.leaf("active", Op.EQ, "1")
        cond = Condition.and_condition(c1, c2)
        assert cond.is_composite()
        assert cond.logic == LogicOp.AND
        assert len(cond.sub_conditions) == 2
    
    def test_in_condition(self):
        cond = Condition.in_condition("status", ["active", "verified"])
        assert cond.op == Op.IN
        assert cond.values == ["active", "verified"]
    
    def test_nin_condition(self):
        cond = Condition.in_condition("status", ["banned"], negate=True)
        assert cond.op == Op.NIN