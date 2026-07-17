from typing import Optional, List
from pydantic_settings import BaseSettings, SettingsConfigDict
from pydantic import Field, field_validator


class Settings(BaseSettings):
    APP_NAME: str = "NexoraDB API"
    APP_VERSION: str = "0.1.0"
    DEBUG: bool = True
    ENVIRONMENT: str = "development"
    HOST: str = "0.0.0.0"
    PORT: int = 8000
    
    DB_PATH: str = "/var/data/nexoradb"
    ROCKSDB_PATH: str = "/var/data/nexoradb/rocksdb"
    GRAPH_PATH: str = "/var/data/nexoradb/graph"
    
    API_TOKEN_SECRET: str = Field(default="change-this-secret-in-production", min_length=32)
    TOKEN_EXPIRY_SECONDS: int = 86400
    
    ALLOWED_ORIGINS: List[str] = Field(default=["http://localhost:3000", "http://localhost:5173"])
    
    LOG_LEVEL: str = "INFO"
    ENABLE_QUERY_LOGGING: bool = True
    
    NATIVE_MODULE_PATH: Optional[str] = None
    GRAPH_ENABLED: bool = True
    
    @field_validator("API_TOKEN_SECRET")
    @classmethod
    def validate_secret(cls, v: str) -> str:
        if len(v) < 32:
            raise ValueError("API_TOKEN_SECRET must be at least 32 characters")
        return v
    
    @field_validator("ENVIRONMENT")
    @classmethod
    def validate_environment(cls, v: str) -> str:
        allowed = {"development", "staging", "production"}
        if v not in allowed:
            raise ValueError(f"ENVIRONMENT must be one of {allowed}")
        return v
    
    def validate_for_startup(self) -> None:
        if self.ENVIRONMENT == "production" and self.API_TOKEN_SECRET == "change-this-secret-in-production":
            raise ValueError("API_TOKEN_SECRET must be changed in production")
        if self.ENVIRONMENT == "production" and self.DEBUG:
            raise ValueError("DEBUG must be False in production")
    
    model_config = SettingsConfigDict(
        env_file=".env",
        case_sensitive=True,
        extra="ignore"
    )


settings = Settings()