import pytest
from app.api.v1.models.security import (
    create_app_token,
    decode_app_token,
    AppTokenError,
)


class TestAppToken:
    def test_create_and_decode_token(self):
        token_response = create_app_token(
            app_id="test-app",
            app_name="Test Application",
            scopes=["query:execute"],
            secret="test-secret-32-chars-long-enough!",
            expires_in_seconds=3600,
        )
        
        claims = decode_app_token(token_response.token, "test-secret-32-chars-long-enough!")
        
        assert claims.app_id == "test-app"
        assert claims.app_name == "Test Application"
        assert "query:execute" in claims.scopes
        assert claims.expires_at is not None
    
    def test_expired_token(self):
        token_response = create_app_token(
            app_id="test-app",
            app_name="Test",
            scopes=["query:execute"],
            secret="test-secret-32-chars-long-enough!",
            expires_in_seconds=0,
        )
        
        with pytest.raises(AppTokenError, match="application token expired"):
            decode_app_token(token_response.token, "test-secret-32-chars-long-enough!")
    
    def test_invalid_signature(self):
        token_response = create_app_token(
            app_id="test-app",
            app_name="Test",
            scopes=["query:execute"],
            secret="test-secret-32-chars-long-enough!",
        )
        
        tampered = token_response.token[:-5] + "xxxxx"
        
        with pytest.raises(AppTokenError, match="invalid application token signature"):
            decode_app_token(tampered, "test-secret-32-chars-long-enough!")