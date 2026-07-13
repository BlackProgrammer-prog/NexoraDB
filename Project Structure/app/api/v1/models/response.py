from typing import Optional, Any, Generic, TypeVar, List
from pydantic import BaseModel

T = TypeVar('T')


class StandardResponse(BaseModel, Generic[T]):
    success: bool
    data: Optional[T] = None
    error_msg: Optional[str] = None
    
    @staticmethod
    def ok(payload: Any = None):
        return StandardResponse(success=True, data=payload)
    
    @staticmethod
    def err(msg: str, details: Any = None):
        return StandardResponse(success=False, error_msg=msg, data=details)


class PaginatedResponse(BaseModel, Generic[T]):
    items: List[T]
    total: int
    limit: int
    skip: int
    has_more: bool