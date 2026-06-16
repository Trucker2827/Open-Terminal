from __future__ import annotations

import functools
import json
import logging
from typing import Any, Callable


def setup_logging(level: str) -> None:
    logging.basicConfig(
        level=getattr(logging, level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)s %(name)s %(message)s",
    )


def json_safe(value: Any) -> Any:
    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    if isinstance(value, dict):
        return {str(k): json_safe(v) for k, v in value.items()}
    if isinstance(value, (list, tuple, set)):
        return [json_safe(v) for v in value]
    if hasattr(value, "model_dump"):
        return json_safe(value.model_dump())
    if hasattr(value, "to_dict"):  # coinbase-advanced-py response objects
        return json_safe(value.to_dict())
    if hasattr(value, "dict"):
        return json_safe(value.dict())
    return str(value)


def tool_error(fn: Callable[..., Any]) -> Callable[..., Any]:
    # functools.wraps sets __wrapped__, so inspect.signature() (used by FastMCP to
    # build each tool's input schema) follows through to fn's REAL parameters.
    # Without it the wrapper's (*args, **kwargs) signature leaks into the schema
    # and every tool becomes uncallable (clients must pass the named params).
    @functools.wraps(fn)
    def wrapper(*args: Any, **kwargs: Any) -> Any:
        try:
            return fn(*args, **kwargs)
        except Exception as exc:  # MCP tools should return structured errors, not tracebacks.
            logging.getLogger(fn.__module__).exception("tool failed: %s", fn.__name__)
            return {"ok": False, "error_type": exc.__class__.__name__, "error": str(exc)}
    return wrapper


def audit(event: str, payload: dict[str, Any]) -> None:
    logging.getLogger("audit").info("%s %s", event, json.dumps(json_safe(payload), sort_keys=True))
