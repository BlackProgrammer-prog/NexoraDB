"""Administrator-only performance control request schema."""
from pydantic import BaseModel, ConfigDict, StrictBool


class PerformanceUpdate(BaseModel):
    model_config = ConfigDict(extra='forbid')
    parser_cache: StrictBool | None = None
    clear_parser_cache: StrictBool | None = None
    read_cache_admission: StrictBool | None = None
    smart_indexes: StrictBool | None = None
    index_advisor: StrictBool | None = None
