from importlib.resources import files

from .binding import language

HIGHLIGHTS_QUERY: str = (
    files(__package__).joinpath("queries/highlights.scm").read_text("utf-8")
)

__all__ = ["HIGHLIGHTS_QUERY", "language"]
