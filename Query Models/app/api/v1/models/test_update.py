import pytest
from app.api.v1.models.update import UpdateSpec, UpdateOp
from app.api.v1.models.query import ValueType


class TestUpdateSpec:
    def test_builder_pattern(self):
        spec = (UpdateSpec()
                .set_field("bio", "Hello")
                .inc("likes", "1")
                .push("tags", "sports"))
        
        assert len(spec.operations) == 3
        assert spec.operations[0].op == UpdateOp.SET
        assert spec.operations[0].field == "bio"
        assert spec.operations[1].op == UpdateOp.INC
        assert spec.operations[2].op == UpdateOp.PUSH
    
    def test_unset(self):
        spec = UpdateSpec().unset("temp")
        assert spec.operations[0].op == UpdateOp.UNSET
        assert spec.operations[0].field == "temp"
    
    def test_rename(self):
        spec = UpdateSpec().rename("old", "new")
        assert spec.operations[0].op == UpdateOp.RENAME
        assert spec.operations[0].field == "old"
        assert spec.operations[0].value == "new"