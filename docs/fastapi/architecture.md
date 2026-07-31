# FastAPI Architecture - NexoraDB

## Overview
This module serves as the API layer for NexoraDB using FastAPI.

## Core Components

### 1. Models (Pydantic)
- **query.py**: Condition, QueryOptions (maps to C++ QueryLayer)
- **update.py**: UpdateSpec with builder pattern
- **security.py**: App token authentication (HMAC-SHA256)
- **schema.py**: Collection schema definitions
- **graph.py**: Graph operations models

### 2. Endpoints
- **query.py**: Main query execution endpoint
- **documents.py**: Document CRUD operations
- **collections.py**: Collection management
- **graph.py**: Graph operations
- **algorithms.py**: Algorithm execution
- **apps.py**: App token management
- **system.py**: Health and metrics

### 3. Core
- **config.py**: Settings with environment validation
- **dependencies.py**: Authentication dependencies
- **exceptions.py**: Custom exceptions
- **native.py**: C++ engine bridge

## Data Flow
1. HTTP Request → FastAPI
2. Authentication → App Token validation
3. Validation → Pydantic models
4. Execution → C++ Engine (via native module)
5. Response → StandardResponse

## Security
- All endpoints require Bearer token authentication
- Scope-based authorization (query:execute, documents:read, etc.)
- Tokens are HMAC-SHA256 signed
- Production mode enforces security settings