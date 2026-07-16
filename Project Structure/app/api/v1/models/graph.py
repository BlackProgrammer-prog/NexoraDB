from typing import Optional, List, Dict, Any
from enum import Enum
from pydantic import BaseModel, Field


class GraphKind(str, Enum):
    LIVE = "Live"
    STATIC = "Static"


class Direction(str, Enum):
    OUT = "Out"
    IN = "In"
    BOTH = "Both"


class NodeView(BaseModel):
    dense_id: int
    external_id: str
    type_name: str
    flags: int
    out_degree: int
    in_degree: int


class EdgeView(BaseModel):
    edge_id: int
    src: int
    dst: int
    src_ext: str
    dst_ext: str
    type_name: str
    flags: int


class GraphStats(BaseModel):
    active_nodes: int
    active_edges: int
    deleted_nodes: int
    deleted_edges: int
    implicit_nodes: int
    heavy_nodes: int
    version: int


class AlgorithmType(str, Enum):
    LOCK = "LOCK"   # Lightweight, on LiveGraph
    JOB = "JOB"     # Heavy, on StaticGraphView


class AlgorithmParams(BaseModel):
    name: str
    params: Dict[str, Any] = Field(default_factory=dict)


class JobStatus(str, Enum):
    PENDING = "pending"
    RUNNING = "running"
    COMPLETED = "completed"
    FAILED = "failed"
    CANCELLED = "cancelled"


class JobInfo(BaseModel):
    job_id: str
    algorithm: str
    status: JobStatus
    started_at: str
    completed_at: Optional[str] = None
    progress: float = 0.0
    result: Optional[Any] = None
    error: Optional[str] = None