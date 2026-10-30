"""MtoU_LiveLink Maya adapter package.

The entry module `MtoULiveLink.py` re-exports the product entry points;
each module here owns one lifecycle: configuration, protocol, scene,
timeline, session, cache, linkage and the panel controller.
"""

import os as _os

from .config import PROTOCOL_VERSION, __version__
from .install import content_digest as _content_digest

# Digest of the code this session loaded. The installer compares it with what
# it installs to decide whether Maya must restart before the new code runs.
LOADED_DIGEST = _content_digest(_os.path.dirname(_os.path.dirname(_os.path.abspath(__file__))))

__all__ = ["PROTOCOL_VERSION", "__version__"]
