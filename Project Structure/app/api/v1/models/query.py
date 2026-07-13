from typing import Optional, List, Union
from enum import Enum
from pydantic import BaseModel, Field, model_validator


class Op(str, Enum):
    EQ = "EQ"
    NEQ = "NEQ"
    GT = "GT"
    GTE = "GTE"
    LT = "LT"
    LTE = "LTE"
    IN = "IN"
    NIN = "NIN"
    EXISTS = "EXISTS"
    REGEX = "REGEX"
    STARTS = "STARTS"
    CONTAINS = "CONTAINS"


class LogicOp(str, Enum):
    AND = "AND"
    OR = "OR"
    NOR = "NOR"
    NOT = "NOT"


class ValueType(str, Enum):
    STRING = "String"
    INT64 = "Int64"
    FLOAT64 = "Float64"
    BOOL = "Bool"
    NULL = "Null"


class SortOrder(str, Enum):
    ASCENDING = "Ascending"
    DESCENDING = "Descending"


class Condition(BaseModel):
    field: Optional[str] = None
    op: Optional[Op] = None
    value: Optional[str] = None
    value_type: ValueType = ValueType.STRING
    values: Optional[List[str]] = None
    logic: Optional[LogicOp] = None
    sub_conditions: Optional[List['Condition']] = None

    @model_validator(mode='after')
    def validate_condition(self):
        is_leaf = self.field is not None and self.op is not None
        is_composite = self.logic is not None and self.sub_conditions is not None
        
        if not is_leaf and not is_composite:
            if not self.field and not self.op and not self.logic and not self.sub_conditions:
                return self
            raise ValueError("Condition must be either Leaf or Composite")
        
        if is_leaf and is_composite:
            raise ValueError("Condition cannot be both Leaf and Composite")
        
        return self
    
    def is_leaf(self) -> bool:
        return self.field is not None and self.op is not None
    
    def is_empty(self) -> bool:
        return not self.field and not self.op and not self.logic and not self.sub_conditions
    
    def is_composite(self) -> bool:
        return self.logic is not None and self.sub_conditions is not None
    
    @classmethod
    def leaf(cls, field: str, op: Op, value: str, value_type: ValueType = ValueType.STRING):
        return cls(field=field, op=op, value=value, value_type=value_type)
    
    @classmethod
    def in_condition(cls, field: str, values: List[str], negate: bool = False):
        return cls(field=field, op=Op.NIN if negate else Op.IN, values=values)
    
    @classmethod
    def and_condition(cls, *conditions: 'Condition'):
        return cls(logic=LogicOp.AND, sub_conditions=list(conditions))
    
    @classmethod
    def or_condition(cls, *conditions: 'Condition'):
        return cls(logic=LogicOp.OR, sub_conditions=list(conditions))


class SortField(BaseModel):
    field: str
    order: SortOrder = SortOrder.ASCENDING


class QueryOptions(BaseModel):
    limit: int = 0
    skip: int = 0
    sort: Optional[List[SortField]] = None
    projection: Optional[List[str]] = None
    projection_exclude: bool = False


Condition.model_rebuild()