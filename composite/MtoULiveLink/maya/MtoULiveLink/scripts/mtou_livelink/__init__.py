"""MtoU_LiveLink Maya adapter package.

The entry module `MtoULiveLink.py` re-exports the product entry points;
each module here owns one lifecycle: configuration, protocol, scene,
timeline, session, cache, linkage and the panel controller.
"""

from .config import PROTOCOL_VERSION, __version__

__all__ = ["PROTOCOL_VERSION", "__version__"]
