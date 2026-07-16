from typing import Optional, List
from enum import Enum
from pydantic import BaseModel, Field


class FieldType(str, Enum):
    STRING = "String"
    INT32 = "Int32"
    INT64 = "Int64"
    FLOAT64 = "Float64"
    BOOL = "Bool"
    ARRAY = "Array"
    OBJECT = "Object"


class SchemaField(BaseModel):
    name: str
    type: FieldType
    required: bool = False
    unique: bool = False
    default_val: Optional[str] = None


class SchemaDefinition(BaseModel):
    fields: List[SchemaField] = Field(default_factory=list)
    strict: bool = False


class IndexType(str, Enum):
    SINGLE_FIELD = "SingleField"
    COMPOUND = "Compound"
    UNIQUE = "Unique"


class IndexDefinition(BaseModel):
    index_name: str
    fields: List[str]
    type: IndexType = IndexType.SINGLE_FIELD


class ForeignKeyDefinition(BaseModel):
    fk_name: str
    local_field: str
    ref_collection: str
    ref_field: str = "_id"