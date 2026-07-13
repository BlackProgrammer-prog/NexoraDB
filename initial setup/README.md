# NexoraDB FastAPI Backend

API layer for NexoraDB - Document Store, Graph Engine, and Algorithms.

## Features

- ✅ Query Execution: Execute NexoraQL queries via `/api/v1/query`
- ✅ App Token Auth: Secure token-based authentication for external apps
- ✅ Document CRUD: Full document operations (insert, find, update, delete)
- ✅ Collection Management: Create, drop, list, and schema management
- ✅ Graph Operations: LiveGraph and StaticGraph management
- ✅ Algorithm Execution: Run Lock and Job algorithms
- ✅ Python Driver: Ready-to-use client library

## Quick Start

### Installation

```bash
# Create virtual environment
python3 -m venv venv
source venv/bin/activate

# Install dependencies
pip install -r requirements.txt

# Copy environment file
cp .env.example .env
# Edit .env with your settings