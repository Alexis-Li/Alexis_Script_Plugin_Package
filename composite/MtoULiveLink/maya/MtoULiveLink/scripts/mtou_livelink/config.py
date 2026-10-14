"""Constants shared by the Maya adapter: versions, wire limits and cache contracts."""


__version__ = "0.10.0"

PROTOCOL_VERSION = 10

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

# The third exclusive time-control state: Unreal drives the Level Sequence
# time and the evaluated camera while Maya follows and answers with complete
# pose groups. See protocol/v10.md.
LINKAGE_MODE = "linkage"

MODES = (REALTIME_MODE, CACHED_MODE, LINKAGE_MODE)

CONTROL_MAYA = "maya"

CONTROL_UNREAL = "unreal"

# Frozen v10 budgets. The linkage group is metered as one message, not per
# object, so the framing ceiling is also the group ceiling.
MAX_LINKAGE_GROUP_BYTES = MAX_MESSAGE_BYTES

# Undelivered control messages per direction; overflow is an error instead of
# an unbounded backlog.
MAX_CONTROL_QUEUE = 16

# At most one linkage target is in flight and one replaceable target waits.
MAX_PENDING_LINKAGE_TARGETS = 2

# Candidate stop-convergence budget from the parent issue: after the last
# desired target, the group must be applied within this many seconds.
LINKAGE_CONVERGENCE_SECONDS = 2.0

# Scene-reference handoff budgets, enforced by the Maya importer before any
# staging node is created.
MAX_REFERENCE_BYTES = 256 * 1024 * 1024

MAX_REFERENCE_OBJECTS = 20000

MAX_REFERENCE_TRIANGLES = 20000000

# Tool-owned scene-reference container and its ownership mark.
REFERENCE_CONTAINER = "MtoU_UE_SceneRef"

REFERENCE_MARK_ATTRIBUTE = "mtouSceneRefContainer"

REFERENCE_MARK_VALUE = "mtou-scene-ref-container/1"

REFERENCE_GRAY_SHADER = "MtoU_UE_SceneRef_Gray"

# Tool-owned temporary camera that follows the Unreal evaluated camera.
LINK_CAMERA_NAME = "MtoU_LinkCamera"

# Stable object identifier of the single character the product pairs. It is not
# a display name, a bone short name or a list position, so a reordered scene
# cannot silently re-target the wrong object.
CHARACTER_OBJECT_ID = "character"

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
