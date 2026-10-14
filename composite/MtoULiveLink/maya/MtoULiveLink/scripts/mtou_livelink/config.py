"""Constants shared by the Maya adapter: versions, wire limits and cache contracts."""


__version__ = "0.9.0"

PROTOCOL_VERSION = 9

WORKFLOW_ANIMATION = "animation"

WORKFLOW_MODEL = "model"

WORKFLOWS = (WORKFLOW_ANIMATION, WORKFLOW_MODEL)

# Frozen per-message framing ceiling enforced by both adapters before any
# JSON parse or allocation; oversized messages close the connection. Pinned by
# limits.max_message_bytes in the conformance corpus (asserted in tests).
MAX_MESSAGE_BYTES = 32 * 1024 * 1024

HOST = "127.0.0.1"

PORT = 54321

SUBJECT_NAME = "MtoU_Character"

CURVE_VALUE_TOLERANCE = 1.0e-6

BIND_MATRIX_TOLERANCE = 1.0e-5

MIN_FRAME_RATE = 1.0

MAX_FRAME_RATE = 60.0

PLAYBACK_CAP_OPTION_VAR = "MtoULiveLinkPlaybackCap"

DEFAULT_PLAYBACK_CAP = "20 fps"

PLAYBACK_CAP_CHOICES = ("Follow Scene", "30 fps", "20 fps", "15 fps")

CACHE_OWNER = "MtoULiveLink"

CACHE_FORMAT_VERSION = 1

CACHE_FILE_PREFIX = "MtoULiveLinkPlayback-"

CACHE_METADATA_SUFFIX = ".metadata.json"

CACHE_FRAMES_SUFFIX = ".frames"

CACHE_PARTIAL_METADATA_SUFFIX = ".partial.json"

CACHE_PARTIAL_FRAMES_SUFFIX = ".partial.frames"

CACHE_CONFIRMATION_BYTES = 1 << 30

CACHE_STALE_SECONDS = 24 * 60 * 60

# Recalibrated with the first real-project capture (320 frames at 30 fps
# exceeding the previous 64 MiB estimate); still a frozen bound, not a
# user tuning knob.
MAX_CACHE_PAYLOAD_BYTES = 1024 * 1024 * 1024

MAX_CACHE_FRAME_COUNT = 20000

UPLOAD_CHUNK_FRAMES = 64

# Watchdog for a wedged-but-open connection during the Ready wait; the upload
# itself has no real-time deadline and is never paced by this value.
UPLOAD_READY_TIMEOUT_SECONDS = 60.0

REALTIME_MODE = "realtime"

CACHED_MODE = "cached"

TIME_UNIT_FPS = {
    "game": 15.0,
    "film": 24.0,
    "pal": 25.0,
    "ntsc": 30.0,
    "show": 48.0,
    "palf": 50.0,
    "ntscf": 60.0,
    "29.97df": 29.97,
}
