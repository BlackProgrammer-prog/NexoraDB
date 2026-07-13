from typing import Optional, List
from enum import Enum
from pydantic import BaseModel, Field

from .query import ValueType


class UpdateOp(str, Enum):
    SET = "Set"
    UNSET = "Unset"
    INC = "Inc"
    MUL = "Mul"
    MIN = "Min"
    MAX = "Max"
    RENAME = "Rename"
    CURRENT_DATE = "CurrentDate"
    PUSH = "Push"
    PUSH_ALL = "PushAll"
    PULL = "Pull"
    PULL_ALL = "PullAll"
    ADD_TO_SET = "AddToSet"
    POP = "Pop"


class UpdateOperation(BaseModel):
    op: UpdateOp
    field: str
    value: Optional[str] = None
    values: Optional[List[str]] = None
    value_type: ValueType = ValueType.STRING


class UpdateSpec(BaseModel):
    operations: List[UpdateOperation] = Field(default_factory=list)
    upsert: bool = False
    
    def set_field(self, field: str, value: str, value_type: ValueType = ValueType.STRING):
        self.operations.append(UpdateOperation(op=UpdateOp.SET, field=field, value=value, value_type=value_type))
        return self
    
    def unset(self, field: str):
        self.operations.append(UpdateOperation(op=UpdateOp.UNSET, field=field, value=""))
        return self
    
    def inc(self, field: str, delta: str, value_type: ValueType = ValueType.INT64):
        self.operations.append(UpdateOperation(op=UpdateOp.INC, field=field, value=delta, value_type=value_type))
        return self
    
    def push(self, field: str, element: str, value_type: ValueType = ValueType.STRING):
        self.operations.append(UpdateOperation(op=UpdateOp.PUSH, field=field, value=element, value_type=value_type))
        return self
    
    def pull(self, field: str, element: str, value_type: ValueType = ValueType.STRING):
        self.operations.append(UpdateOperation(op=UpdateOp.PULL, field=field, value=element, value_type=value_type))
        return self
    
    def rename(self, old_field: str, new_field: str):
        self.operations.append(UpdateOperation(op=UpdateOp.RENAME, field=old_field, value=new_field))
        return self
    
    def touch_date(self, field: str):
        self.operations.append(UpdateOperation(op=UpdateOp.CURRENT_DATE, field=field, value=""))
        return self