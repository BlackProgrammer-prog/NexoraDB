"""
nexoraql.errors
───────────────
سلسله‌مراتب خطاهای NexoraQL.

NexoraQLError            ← ریشه همه خطاها
├── NexoraQLParseError   ← خطای گرامری (از Lark)
├── NexoraQLSemanticError← خطای معنایی (قبل از اجرا)
└── NexoraQLExecutionError← خطای زمان اجرا (از DocEngine/GraphManager)
"""

from __future__ import annotations


class NexoraQLError(Exception):
    """ریشه تمام خطاهای NexoraQL."""

    def __init__(self, message: str, line: int | None = None,
                 column: int | None = None, context: str | None = None):
        self.message = message
        self.line = line
        self.column = column
        self.context = context
        super().__init__(self._format())

    def _format(self) -> str:
        parts = [self.message]
        if self.line is not None:
            loc = f"line {self.line}"
            if self.column is not None:
                loc += f", col {self.column}"
            parts.append(f"({loc})")
        if self.context:
            parts.append(f"\n  near: {self.context}")
        return " ".join(parts)

    def to_dict(self) -> dict:
        """برای برگشت JSON به FastAPI."""
        return {
            "error": type(self).__name__,
            "message": self.message,
            "line": self.line,
            "column": self.column,
            "context": self.context,
        }


class NexoraQLParseError(NexoraQLError):
    """خطای گرامری — کوئری قابل parse نیست."""
    pass


class NexoraQLSemanticError(NexoraQLError):
    """خطای معنایی — کوئری parse شد اما معتبر نیست.

    مثال‌ها:
      - MAP NODE بدون USE GRAPH قبلی
      - الگوریتم LOCK با RUN JOB صدا زده شود
      - UPDATE بدون WHERE
    """
    pass


class NexoraQLExecutionError(NexoraQLError):
    """خطای اجرا — DocEngine یا GraphManager خطا برگرداند."""
    pass


class NexoraQLUnsupportedError(NexoraQLError):
    """دستور معتبر است اما در MVP فعلی پشتیبانی نمی‌شود."""
    pass