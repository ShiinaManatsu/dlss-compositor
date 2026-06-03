from __future__ import annotations

bl_info = {
    "name": "DLSS Compositor",
    "blender": (5, 1, 0),
    "category": "Render",
    "version": (2, 0, 0),
}

# Blender requires bl_info before imports — E402 is expected here
import logging  # noqa: E402
import sys      # noqa: E402
from pathlib import Path  # noqa: E402

from . import properties, panel, operators  # noqa: E402
from .handlers import register_handlers, unregister_handlers  # noqa: E402

# ---------------------------------------------------------------------------
# Debug log — written to the extension's parent directory so it survives
# hot-reloads and is easy to find.  Cleared on every register() call.
# ---------------------------------------------------------------------------
_LOG_PATH = Path(__file__).parent.parent / "dlss_compositor_debug.log"
_LOG_NAME = "dlss_compositor"


def _setup_logging() -> None:
    """Configure root logger for dlss_compositor; clear the log file first."""
    logger = logging.getLogger(_LOG_NAME)
    logger.setLevel(logging.DEBUG)

    # Remove any handlers left over from a previous hot-reload
    for h in list(logger.handlers):
        logger.removeHandler(h)
        h.close()

    # File handler — truncate on open so each reload starts fresh
    fh = logging.FileHandler(str(_LOG_PATH), mode="w", encoding="utf-8")
    fh.setLevel(logging.DEBUG)
    fh.setFormatter(
        logging.Formatter("%(asctime)s [%(levelname)s] %(name)s: %(message)s")
    )
    logger.addHandler(fh)

    # Mirror to Blender's system console (stderr)
    sh = logging.StreamHandler(sys.stderr)
    sh.setLevel(logging.DEBUG)
    sh.setFormatter(logging.Formatter("[DLSS] %(levelname)s %(name)s: %(message)s"))
    logger.addHandler(sh)

    logger.info("=" * 60)
    logger.info("DLSS Compositor registered — log: %s", _LOG_PATH)
    logger.info("=" * 60)


def register() -> None:
    _setup_logging()
    properties.register()
    panel.register()
    operators.register()
    register_handlers()


def unregister() -> None:
    unregister_handlers()
    operators.unregister()
    panel.unregister()
    properties.unregister()
    logging.getLogger(_LOG_NAME).info("DLSS Compositor unregistered")
