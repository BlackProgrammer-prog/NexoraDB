from typing import Optional, Any


class NexoraDBException(Exception):
    def __init__(self, message: str, status_code: int = 500, details: Optional[Any] = None):
        self.message = message
        self.status_code = status_code
        self.details = details
        super().__init__(message)


class DocumentNotFoundError(NexoraDBException):
    def __init__(self, doc_id: str, collection: str):
        super().__init__(
            message=f"Document '{doc_id}' not found in collection '{collection}'",
            status_code=404
        )


class CollectionNotFoundError(NexoraDBException):
    def __init__(self, collection: str):
        super().__init__(f"Collection '{collection}' not found", status_code=404)


class ValidationError(NexoraDBException):
    def __init__(self, message: str, details: Optional[Any] = None):
        super().__init__(message=message, status_code=400, details=details)


class ForeignKeyViolationError(NexoraDBException):
    def __init__(self, message: str):
        super().__init__(message=message, status_code=409)