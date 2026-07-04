"""
NexoraQL — زبان کوئری NexoraDB
══════════════════════════════

وابستگی: pip install lark   (تنها وابستگی خارجی)

سه سطح استفاده:

── سطح ۱: فقط parse (بدون دیتابیس — برای تست/lint) ──────────
    from nexoraql import parse
    stmts = parse("SELECT * FROM users WHERE age > 18;")

── سطح ۲: Executor دستی (کنترل کامل) ────────────────────────
    import nexoradb
    from nexoraql import Executor

    engine = nexoradb.DocEngine("/var/data/db")
    gm     = nexoradb.GraphManager(engine, "./graph_data")
    gm.startup()

    ex = Executor(engine, gm)
    results = ex.execute_text("COUNT FROM users;")

── سطح ۳: Session یکجا (ساده‌ترین — برای FastAPI) ────────────
    from nexoraql import NexoraQLSession

    session = NexoraQLSession("/var/data/db", "./graph_data")
    results = session.execute('''
        CREATE COLLECTION users;
        INSERT INTO users VALUES ('{"_id":"u1","username":"ali","age":28}');
        SELECT * FROM users WHERE age > 18;
    ''')

نمونه کوئری‌های گراف و الگوریتم:
    CREATE LIVE GRAPH social HETEROGENEOUS DIRECTED;
    USE GRAPH social;
    MAP NODE User FROM users KEY _id PROPERTIES username, age;
    MAP EDGE FOLLOWS FROM follows SOURCE from_id AS User TARGET to_id AS User;
    BUILD GRAPH social;
    TRAVERSE User('u1') OUT FOLLOWS DEPTH 2 LIMIT 50;
    EDGE EXISTS User('u1') -[FOLLOWS]-> User('u2');
    RUN LOCK MutualFriends ON social WITH user1='u1', user2='u2';
    RUN JOB CommunityDetection ON social WITH max_iterations=10, members=true;
    JOB RESULT 'job_1';
"""

from .parser import parse, parse_one
from .semantic import (
    Executor,
    Validator,
    ALGO_SPECS,
    algo_params_to_positional,
    build_condition,
    build_update_spec,
)
from .errors import (
    NexoraQLError,
    NexoraQLParseError,
    NexoraQLSemanticError,
    NexoraQLExecutionError,
    NexoraQLUnsupportedError,
)
from . import ast_nodes

__version__ = "1.0.0"

__all__ = [
    "parse", "parse_one",
    "Executor", "Validator",
    "NexoraQLSession",
    "ALGO_SPECS", "algo_params_to_positional",
    "build_condition", "build_update_spec",
    "NexoraQLError", "NexoraQLParseError", "NexoraQLSemanticError",
    "NexoraQLExecutionError", "NexoraQLUnsupportedError",
    "ast_nodes",
]


class NexoraQLSession:
    """Session کامل: DocEngine + GraphManager + Executor در یک کلاس.

    Args:
        db_path:   مسیر RocksDB (DocEngine)
        graph_dir: مسیر فایل‌های گراف (GraphManager) — None = بدون گراف

    Example:
        session = NexoraQLSession("/tmp/mydb", "/tmp/mygraphs")
        for r in session.execute("SHOW COLLECTIONS; SYSTEM STATUS;"):
            print(r)
        session.close()
    """

    def __init__(self, db_path: str, graph_dir: str | None = "./graph_data"):
        import nexoradb  # lazy — خطای واضح اگر .so نبود

        self.engine = nexoradb.DocEngine(db_path)
        if not self.engine.is_healthy():
            raise RuntimeError(f"DocEngine failed to open: {db_path}")

        self.gm = None
        if graph_dir is not None and getattr(nexoradb, "GRAPH_ENABLED", False):
            self.gm = nexoradb.GraphManager(self.engine, graph_dir)
            self.gm.startup()

        self.executor = Executor(self.engine, self.gm)

    def execute(self, text: str) -> list[dict]:
        """اجرای یک یا چند دستور NexoraQL → لیست نتایج."""
        return self.executor.execute_text(text)

    def execute_one(self, text: str) -> dict:
        """اجرای یک دستور واحد → یک نتیجه."""
        results = self.execute(
            text if text.rstrip().endswith(";") else text + ";")
        return results[0] if results else {"success": False, "error": "empty"}

    def register_algorithm(self, name: str, fn) -> None:
        """ثبت runner پایتونی برای الگوریتم (تا زمان binding مستقیم C++)."""
        self.executor.register_algorithm(name, fn)

    def close(self) -> None:
        if self.gm is not None:
            self.gm.shutdown()