from __future__ import annotations

import json
import time
import uuid
from typing import Any, Protocol

from fastapi import HTTPException, status
from pydantic import BaseModel, Field


class DBResultLike(Protocol):
    success: bool
    data: str
    error_msg: str


class DocumentStoreEngine(Protocol):
    def create_collection(self, collection: str) -> DBResultLike: ...

    def drop_collection(self, collection: str) -> DBResultLike: ...

    def list_collections(self) -> list[str]: ...

    def collection_exists(self, collection: str) -> bool: ...

    def insert_one(self, collection: str, document_json: str) -> DBResultLike: ...

    def find_by_id(self, collection: str, document_id: str) -> DBResultLike: ...

    def find_many(self, collection: str) -> DBResultLike: ...

    def count(self, collection: str) -> DBResultLike: ...

    def delete_by_id(self, collection: str, document_id: str) -> DBResultLike: ...


class CreateCollectionRequest(BaseModel):
    name: str = Field(min_length=1, max_length=128)


class UpdateCollectionRequest(BaseModel):
    name: str = Field(min_length=1, max_length=128)


class DocumentRequest(BaseModel):
    data: dict[str, Any]


def _now_iso() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def _result_error(error_msg: str) -> HTTPException:
    lower = error_msg.lower()
    if "not found" in lower or "does not exist" in lower:
        return HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail={"message": error_msg})
    if "already exists" in lower:
        return HTTPException(status_code=status.HTTP_409_CONFLICT, detail={"message": error_msg})
    if "reserved collection name" in lower:
        return HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail={"message": error_msg})
    return HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail={"message": error_msg})


def _require_success(result: DBResultLike) -> DBResultLike:
    if not result.success:
        raise _result_error(result.error_msg)
    return result


def _parse_document(raw_json: str) -> dict[str, Any]:
    try:
        value = json.loads(raw_json)
    except json.JSONDecodeError as exc:
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            detail={"message": "stored document is invalid JSON"},
        ) from exc
    if not isinstance(value, dict):
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            detail={"message": "stored document is not a JSON object"},
        )
    return value


def _document_timestamp(document: dict[str, Any], key: str) -> str:
    camel_key = "".join(
        part if index == 0 else part.capitalize()
        for index, part in enumerate(key.split("_"))
    )
    value = document.get(key) or document.get(camel_key)
    if isinstance(value, str):
        return value
    if isinstance(value, int | float):
        seconds = value / 1000 if value > 10_000_000_000 else value
        return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(seconds))
    return _now_iso()


def _document_response(collection_name: str, document: dict[str, Any]) -> dict[str, Any]:
    document_id = str(document.get("_id") or "")
    return {
        "id": document_id,
        "collectionName": collection_name,
        "data": document,
        "createdAt": _document_timestamp(document, "created_at"),
        "updatedAt": _document_timestamp(document, "updated_at"),
    }


def _collection_response(engine: DocumentStoreEngine, collection_name: str) -> dict[str, Any]:
    count_result = _require_success(engine.count(collection_name))
    documents = list_documents(engine, collection_name)
    size_bytes = sum(len(json.dumps(document["data"], separators=(",", ":"))) for document in documents)
    timestamp = _now_iso()
    return {
        "name": collection_name,
        "documentCount": int(count_result.data or 0),
        "sizeBytes": size_bytes,
        "createdAt": timestamp,
        "updatedAt": timestamp,
    }


def list_collections(engine: DocumentStoreEngine) -> list[dict[str, Any]]:
    return [_collection_response(engine, name) for name in engine.list_collections()]


def create_collection(engine: DocumentStoreEngine, payload: CreateCollectionRequest) -> dict[str, Any]:
    collection_name = payload.name.strip()
    _require_success(engine.create_collection(collection_name))
    return _collection_response(engine, collection_name)


def rename_collection(
    engine: DocumentStoreEngine,
    collection_name: str,
    payload: UpdateCollectionRequest,
) -> dict[str, Any]:
    new_name = payload.name.strip()
    if new_name == collection_name:
        return _collection_response(engine, collection_name)
    if not engine.collection_exists(collection_name):
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail={"message": f"Collection '{collection_name}' does not exist"},
        )
    if engine.collection_exists(new_name):
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail={"message": f"Collection '{new_name}' already exists"},
        )

    existing_documents = [_parse_document(item) for item in _find_many(engine, collection_name)]
    _require_success(engine.create_collection(new_name))
    try:
        for document in existing_documents:
            _require_success(engine.insert_one(new_name, json.dumps(document, separators=(",", ":"))))
        _require_success(engine.drop_collection(collection_name))
    except Exception:
        engine.drop_collection(new_name)
        raise
    return _collection_response(engine, new_name)


def delete_collection(engine: DocumentStoreEngine, collection_name: str) -> None:
    _require_success(engine.drop_collection(collection_name))


def _find_many(engine: DocumentStoreEngine, collection_name: str) -> list[str]:
    result = _require_success(engine.find_many(collection_name))
    try:
        documents = json.loads(result.data or "[]")
    except json.JSONDecodeError as exc:
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            detail={"message": "stored document list is invalid JSON"},
        ) from exc
    if not isinstance(documents, list):
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            detail={"message": "stored document list is not an array"},
        )
    return [json.dumps(document) if isinstance(document, dict) else str(document) for document in documents]


def list_documents(engine: DocumentStoreEngine, collection_name: str) -> list[dict[str, Any]]:
    return [
        _document_response(collection_name, _parse_document(raw_document))
        for raw_document in _find_many(engine, collection_name)
    ]


def get_document(
    engine: DocumentStoreEngine,
    collection_name: str,
    document_id: str,
) -> dict[str, Any]:
    result = _require_success(engine.find_by_id(collection_name, document_id))
    return _document_response(collection_name, _parse_document(result.data))


def create_document(
    engine: DocumentStoreEngine,
    collection_name: str,
    payload: DocumentRequest,
) -> dict[str, Any]:
    document = dict(payload.data)
    document.setdefault("_id", uuid.uuid4().hex)
    document_id = str(document["_id"])
    result = _require_success(engine.insert_one(collection_name, json.dumps(document, separators=(",", ":"))))
    return get_document(engine, collection_name, result.data or document_id)


def replace_document(
    engine: DocumentStoreEngine,
    collection_name: str,
    document_id: str,
    payload: DocumentRequest,
) -> dict[str, Any]:
    old_document = _parse_document(_require_success(engine.find_by_id(collection_name, document_id)).data)
    next_document = dict(payload.data)
    next_document.setdefault("_id", document_id)
    if str(next_document["_id"]) != document_id:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail={"message": "_id cannot be changed"},
        )

    _require_success(engine.delete_by_id(collection_name, document_id))
    insert_result = engine.insert_one(collection_name, json.dumps(next_document, separators=(",", ":")))
    if not insert_result.success:
        engine.insert_one(collection_name, json.dumps(old_document, separators=(",", ":")))
        raise _result_error(insert_result.error_msg)
    return get_document(engine, collection_name, document_id)


def delete_document(engine: DocumentStoreEngine, collection_name: str, document_id: str) -> None:
    result = _require_success(engine.delete_by_id(collection_name, document_id))
    if result.data == "0":
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail={"message": f"Document '{document_id}' not found in '{collection_name}'"},
        )
