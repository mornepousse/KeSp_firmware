"""rf-scope host tool — trace frame codec (see codec.py and main/comm/rf/rf_trace.h)."""
from .codec import (  # noqa: F401
    TraceError, crc8, encode_frame, decode_frame, scan, StreamScanner,
    FOUND, PARTIAL, NONE,
)
