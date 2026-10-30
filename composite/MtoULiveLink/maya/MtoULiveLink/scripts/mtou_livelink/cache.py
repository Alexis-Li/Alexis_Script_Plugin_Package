"""Cached Playback capture, upload, retention and replay control."""

import json
import os
import queue
import re
import shutil
import tempfile
import time
import uuid

from .config import (
    CACHE_CONFIRMATION_BYTES,
    CACHE_FILE_PREFIX,
    CACHE_FORMAT_VERSION,
    CACHE_FRAMES_SUFFIX,
    CACHE_METADATA_SUFFIX,
    CACHE_OWNER,
    CACHE_PARTIAL_FRAMES_SUFFIX,
    CACHE_PARTIAL_METADATA_SUFFIX,
    CACHE_STALE_SECONDS,
    MAX_CACHE_FRAME_COUNT,
    MAX_CACHE_PAYLOAD_BYTES,
    MAX_MESSAGE_BYTES,
    UPLOAD_CHUNK_FRAMES,
    UPLOAD_READY_TIMEOUT_SECONDS,
)
from .diagnostics import DIAGNOSTICS, make_diagnostic
from .mayaapi import cmds, om
from .protocol import (
    cache_frame_wire_size,
    exact_bool,
    exact_int,
    finite_float,
    make_cache_begin_message,
    make_cache_clear_message,
    make_cache_end_message,
    make_cache_enter_message,
    make_cache_frame_message,
    make_frame_message,
)
from .scene import CharacterSceneError
from .session import StreamingSessionError, copy_session_payload
from .space import validate_frame_rate
from .timeline import MayaTimeline


class PlaybackCacheError(RuntimeError):
    """Raised when a temporary cached-playback file cannot be used."""


class PlaybackCache(object):
    """Incremental, atomically finalized storage for captured frame messages."""

    _TOKEN_PATTERN = re.compile(r"^[0-9a-f]{32}$")

    def __init__(self, metadata_path, frames_path, metadata, writer=None,
                 partial_metadata_path=None, partial_frames_path=None):
        self._metadata_path = os.fspath(metadata_path)
        self._frames_path = os.fspath(frames_path)
        self._metadata = dict(metadata)
        self._writer = writer
        base = self._metadata_path
        if base.endswith(CACHE_METADATA_SUFFIX):
            base = base[:-len(CACHE_METADATA_SUFFIX)]
        self._partial_metadata_path = os.fspath(
            partial_metadata_path or base + CACHE_PARTIAL_METADATA_SUFFIX)
        self._partial_frames_path = os.fspath(
            partial_frames_path or base + CACHE_PARTIAL_FRAMES_SUFFIX)
        self._deleted = False

    @classmethod
    def begin(cls, snapshot_revision, capture_start, capture_end, scene_fps,
              temp_dir=None, capture_time=None):
        directory = os.fspath(temp_dir or tempfile.gettempdir())
        os.makedirs(directory, exist_ok=True)
        cache_id = uuid.uuid4().hex
        if not cls._TOKEN_PATTERN.match(cache_id):
            raise PlaybackCacheError("generated cache id is invalid")
        base = os.path.join(directory, CACHE_FILE_PREFIX + cache_id)
        metadata_path = base + CACHE_METADATA_SUFFIX
        frames_path = base + CACHE_FRAMES_SUFFIX
        partial_metadata_path = base + CACHE_PARTIAL_METADATA_SUFFIX
        partial_frames_path = base + CACHE_PARTIAL_FRAMES_SUFFIX
        metadata = {
            "owner": CACHE_OWNER,
            "format_version": CACHE_FORMAT_VERSION,
            "cache_id": cache_id,
            "snapshot_revision": int(snapshot_revision),
            "revision": int(snapshot_revision),
            "capture_range": [int(capture_start), int(capture_end)],
            "capture_start": int(capture_start),
            "capture_end": int(capture_end),
            "frame_count": 0,
            "scene_fps": float(scene_fps),
            "captured_scene_rate": float(scene_fps),
            "capture_time": float(time.time() if capture_time is None else capture_time),
            "completion_state": "partial",
            "completed": False,
            "frames_file": os.path.basename(frames_path),
        }
        try:
            writer = open(partial_frames_path, "w", encoding="utf-8", newline="\n")
            with open(partial_metadata_path, "w", encoding="utf-8",
                      newline="\n") as stream:
                json.dump(
                    metadata, stream, ensure_ascii=False,
                    allow_nan=False, separators=(",", ":"))
                stream.write("\n")
        except (OSError, TypeError, ValueError) as exc:
            try:
                writer.close()
            except (UnboundLocalError, OSError):
                pass
            for path in (partial_metadata_path, partial_frames_path):
                try:
                    os.remove(path)
                except (FileNotFoundError, OSError):
                    pass
            raise PlaybackCacheError(str(exc))
        return cls(
            metadata_path, frames_path, metadata, writer,
            partial_metadata_path, partial_frames_path)

    @classmethod
    def load(cls, metadata_path):
        metadata_path = os.fspath(metadata_path)
        try:
            with open(metadata_path, "r", encoding="utf-8") as stream:
                metadata = json.load(stream)
        except (OSError, ValueError, TypeError) as exc:
            raise PlaybackCacheError("invalid cache metadata: {0}".format(exc))
        cache_id = metadata.get("cache_id") if isinstance(metadata, dict) else None
        expected_metadata_name = CACHE_FILE_PREFIX + str(cache_id) + CACHE_METADATA_SUFFIX
        if os.path.basename(metadata_path) != expected_metadata_name:
            raise PlaybackCacheError("cache metadata filename is not owned by MtoU")
        cls._validate_metadata(metadata, require_complete=True)
        frames_file = metadata.get("frames_file")
        if not frames_file:
            frames_file = os.path.basename(metadata_path).replace(
                CACHE_METADATA_SUFFIX, CACHE_FRAMES_SUFFIX)
        frames_path = os.path.join(os.path.dirname(metadata_path), frames_file)
        if not os.path.isfile(frames_path):
            raise PlaybackCacheError("completed cache frame file is missing")
        return cls(metadata_path, frames_path, metadata)

    @staticmethod
    def _validate_metadata(metadata, require_complete=False):
        if not isinstance(metadata, dict):
            raise PlaybackCacheError("cache metadata must be a JSON object")
        if metadata.get("owner") != CACHE_OWNER:
            raise PlaybackCacheError("cache metadata has an unknown owner")
        if int(metadata.get("format_version", -1)) != CACHE_FORMAT_VERSION:
            raise PlaybackCacheError("unsupported cache format")
        cache_id = metadata.get("cache_id")
        if not isinstance(cache_id, str) or not PlaybackCache._TOKEN_PATTERN.match(cache_id):
            raise PlaybackCacheError("cache metadata has an invalid cache id")
        expected_frames_name = CACHE_FILE_PREFIX + cache_id + CACHE_FRAMES_SUFFIX
        if metadata.get("frames_file", expected_frames_name) != expected_frames_name:
            raise PlaybackCacheError("cache frame filename is not owned by MtoU")
        if require_complete and metadata.get("completed") is not True:
            raise PlaybackCacheError("cache is not complete")
        if require_complete and metadata.get("completion_state") != "complete":
            raise PlaybackCacheError("cache completion state is invalid")
        for name in ("snapshot_revision", "capture_start", "capture_end",
                     "frame_count", "scene_fps", "capture_time"):
            if name not in metadata:
                raise PlaybackCacheError("cache metadata requires '{0}'".format(name))
        if int(metadata["capture_end"]) < int(metadata["capture_start"]):
            raise PlaybackCacheError("cache capture range is invalid")
        if int(metadata["frame_count"]) < 0:
            raise PlaybackCacheError("cache frame count is invalid")
        if (require_complete
                and int(metadata["frame_count"])
                != int(metadata["capture_end"]) - int(metadata["capture_start"]) + 1):
            raise PlaybackCacheError("cache frame count does not cover the capture range")

    @staticmethod
    def serialized_frame_size(frame_message):
        payload = json.dumps(
            frame_message, ensure_ascii=False, allow_nan=False,
            separators=(",", ":"),
        ).encode("utf-8")
        return len(payload) + 1

    @property
    def path(self):
        return self._metadata_path

    @property
    def metadata_path(self):
        return self._metadata_path

    @property
    def frames_path(self):
        return self._frames_path

    @property
    def metadata(self):
        return dict(self._metadata)

    @property
    def cache_id(self):
        return self._metadata.get("cache_id")

    @property
    def snapshot_revision(self):
        return int(self._metadata["snapshot_revision"])

    @property
    def revision(self):
        return self.snapshot_revision

    @property
    def capture_range(self):
        return (
            int(self._metadata["capture_start"]),
            int(self._metadata["capture_end"]),
        )

    @property
    def frame_count(self):
        return int(self._metadata["frame_count"])

    @property
    def scene_fps(self):
        return float(self._metadata["scene_fps"])

    @property
    def captured_scene_rate(self):
        return self.scene_fps

    @property
    def capture_time(self):
        return float(self._metadata["capture_time"])

    @property
    def completion_state(self):
        return self._metadata.get("completion_state", "partial")

    @property
    def completed(self):
        return bool(self._metadata.get("completed"))

    def append(self, frame_message):
        if self._writer is None or self._deleted:
            raise PlaybackCacheError("cache is not accepting frames")
        try:
            payload = json.dumps(
                frame_message, ensure_ascii=False, allow_nan=False,
                separators=(",", ":"),
            )
            self._writer.write(payload + "\n")
            self._writer.flush()
        except (OSError, TypeError, ValueError) as exc:
            raise PlaybackCacheError(str(exc))
        self._metadata["frame_count"] = int(self._metadata["frame_count"]) + 1

    append_frame = append

    def finalize(self):
        if self._deleted:
            raise PlaybackCacheError("cache has been deleted")
        if self._writer is None:
            if self.completed:
                return self
            raise PlaybackCacheError("cache is already finalized")
        expected_frames = (
            int(self._metadata["capture_end"])
            - int(self._metadata["capture_start"])
            + 1)
        if int(self._metadata["frame_count"]) != expected_frames:
            self.delete()
            raise PlaybackCacheError(
                "cache frame count does not cover the capture range")
        try:
            self._writer.flush()
            self._writer.close()
            self._writer = None
            self._metadata["completion_state"] = "complete"
            self._metadata["completed"] = True
            with open(self._partial_metadata_path, "w", encoding="utf-8",
                      newline="\n") as stream:
                json.dump(
                    self._metadata, stream, ensure_ascii=False,
                    allow_nan=False, separators=(",", ":"))
                stream.write("\n")
                stream.flush()
            os.replace(self._partial_frames_path, self._frames_path)
            os.replace(self._partial_metadata_path, self._metadata_path)
        except (OSError, TypeError, ValueError) as exc:
            self.delete()
            raise PlaybackCacheError(str(exc))
        return self

    def iter_frames(self):
        if not self.completed or self._deleted:
            raise PlaybackCacheError("only a completed cache can be replayed")
        expected_frames = self.frame_count

        def frames():
            seen_frames = 0
            try:
                stream = open(self._frames_path, "r", encoding="utf-8")
            except OSError as exc:
                raise PlaybackCacheError(str(exc))
            with stream:
                for line in stream:
                    if not line.strip():
                        continue
                    if seen_frames >= expected_frames:
                        raise PlaybackCacheError(
                            "cached frame file contains too many frames")
                    try:
                        frame = json.loads(line)
                    except (TypeError, ValueError) as exc:
                        raise PlaybackCacheError(
                            "invalid cached frame: {0}".format(exc))
                    if not isinstance(frame, dict) or frame.get("type") != "frame":
                        raise PlaybackCacheError("cached frame is not a captured frame message")
                    seen_frames += 1
                    yield frame
                if seen_frames != expected_frames:
                    raise PlaybackCacheError(
                        "cached frame file does not contain the complete sequence")

        return frames()

    def compatible_with(self, snapshot_revision):
        return int(snapshot_revision) == self.snapshot_revision

    def delete(self):
        if self._deleted:
            return
        self._deleted = True
        writer, self._writer = self._writer, None
        if writer is not None:
            try:
                writer.close()
            except OSError:
                pass
        paths = (
            self._metadata_path,
            self._frames_path,
            self._partial_metadata_path,
            self._partial_frames_path,
        )
        for path in paths:
            try:
                os.remove(path)
            except FileNotFoundError:
                pass
            except OSError:
                pass

    @classmethod
    def cleanup_stale(cls, temp_dir=None, now=None, age_seconds=CACHE_STALE_SECONDS):
        directory = os.fspath(temp_dir or tempfile.gettempdir())
        cutoff = float(time.time() if now is None else now) - float(age_seconds)
        if not os.path.isdir(directory):
            return []
        removed = []
        names = os.listdir(directory)
        metadata_names = [
            name for name in names
            if name.endswith(CACHE_METADATA_SUFFIX)
            or name.endswith(CACHE_PARTIAL_METADATA_SUFFIX)
        ]
        for name in metadata_names:
            path = os.path.join(directory, name)
            try:
                with open(path, "r", encoding="utf-8") as stream:
                    metadata = json.load(stream)
                cls._validate_metadata(metadata, require_complete=False)
            except (OSError, ValueError, TypeError, PlaybackCacheError):
                continue
            token = metadata.get("cache_id")
            if not isinstance(token, str) or not cls._TOKEN_PATTERN.match(token):
                continue
            if name not in (
                    CACHE_FILE_PREFIX + token + CACHE_METADATA_SUFFIX,
                    CACHE_FILE_PREFIX + token + CACHE_PARTIAL_METADATA_SUFFIX):
                continue
            base = os.path.join(directory, CACHE_FILE_PREFIX + token)
            owned_paths = [
                base + CACHE_METADATA_SUFFIX,
                base + CACHE_FRAMES_SUFFIX,
                base + CACHE_PARTIAL_METADATA_SUFFIX,
                base + CACHE_PARTIAL_FRAMES_SUFFIX,
            ]
            existing = [candidate for candidate in owned_paths if os.path.exists(candidate)]
            if not existing:
                continue
            try:
                newest = max(os.path.getmtime(candidate) for candidate in existing)
            except OSError:
                continue
            if newest >= cutoff:
                continue
            for candidate in existing:
                try:
                    os.remove(candidate)
                    removed.append(candidate)
                except OSError:
                    pass
        return removed


def capture_frame_range(playback_range, custom_range=None):
    """Freeze inclusive integer source frames without editing Maya's range."""
    values = playback_range if custom_range is None else custom_range
    try:
        if len(values) != 2:
            raise ValueError("capture range must have two endpoints")
        if custom_range is None:
            start, end = (int(round(float(value))) for value in values)
        else:
            if any(isinstance(value, bool) or not isinstance(value, int)
                   for value in values):
                raise ValueError("custom capture frames must be integers")
            start, end = values
    except (TypeError, ValueError, OverflowError) as exc:
        raise CachedPlaybackError(
            "CACHED_PLAYBACK_CAPTURE_RANGE", "Capture frames must be finite integers.",
            details=str(exc))
    if (start < -(1 << 31) or start > (1 << 31) - 1
            or end < -(1 << 31) or end > (1 << 31) - 1):
        raise CachedPlaybackError(
            "CACHED_PLAYBACK_CAPTURE_RANGE", "Capture frames exceed the supported 32-bit range.")
    if end < start:
        raise CachedPlaybackError(
            "CACHED_PLAYBACK_CAPTURE_RANGE", "Capture end must not precede start.")
    count = end - start + 1
    if count > MAX_CACHE_FRAME_COUNT:
        raise CachedPlaybackError(
            "CACHED_PLAYBACK_FRAME_LIMIT",
            "The capture range spans {0} frames; the fixed cache limit is {1} frames.".format(
                count, MAX_CACHE_FRAME_COUNT),
            details="start {0}, end {1}, limit {2}".format(start, end, MAX_CACHE_FRAME_COUNT))
    return start, end


class CachedPlaybackCacheSummary(object):
    __slots__ = ("_capture_start", "_capture_end", "_frame_count", "_scene_fps",
                 "_capture_time")

    def __init__(self, cache):
        self._capture_start, self._capture_end = cache.capture_range
        self._frame_count = cache.frame_count
        self._scene_fps = cache.scene_fps
        self._capture_time = cache.capture_time

    def __setattr__(self, name, value):
        if hasattr(self, name):
            raise AttributeError("Cached Playback cache summaries are immutable")
        object.__setattr__(self, name, value)

    capture_start = property(lambda self: self._capture_start)
    capture_end = property(lambda self: self._capture_end)
    capture_range = property(lambda self: (self._capture_start, self._capture_end))
    frame_count = property(lambda self: self._frame_count)
    scene_fps = property(lambda self: self._scene_fps)
    capture_time = property(lambda self: self._capture_time)

    def _values(self):
        return (self._capture_start, self._capture_end, self._frame_count,
                self._scene_fps, self._capture_time)

    def __eq__(self, other):
        return (isinstance(other, CachedPlaybackCacheSummary)
                and self._values() == other._values())

    def __ne__(self, other):
        return not self == other


class CachedPlaybackView(object):
    __slots__ = (
        "_state", "_current", "_total", "_cache_summary", "_diagnostic",
        "_can_capture", "_can_upload", "_can_cancel", "_can_leave",
        "_source_frame", "_loop_enabled", "_loop_round",
        "_segment_start", "_segment_end",
    )

    def __init__(self, state, current=0, total=0, cache_summary=None, diagnostic=None,
                 can_capture=False, can_upload=False, can_cancel=False,
                 can_leave=False, source_frame=None, loop_enabled=False,
                 loop_round=0, segment_start=None, segment_end=None):
        self._state = state
        self._current = int(current or 0)
        self._total = int(total or 0)
        self._cache_summary = cache_summary
        self._diagnostic = copy_session_payload(diagnostic) if diagnostic else None
        self._can_capture = bool(can_capture)
        self._can_upload = bool(can_upload)
        self._can_cancel = bool(can_cancel)
        self._can_leave = bool(can_leave)
        self._source_frame = None if source_frame is None else int(source_frame)
        self._loop_enabled = bool(loop_enabled)
        self._loop_round = int(loop_round or 0)
        self._segment_start = None if segment_start is None else int(segment_start)
        self._segment_end = None if segment_end is None else int(segment_end)

    def __setattr__(self, name, value):
        if hasattr(self, name):
            raise AttributeError("Cached Playback views are immutable")
        object.__setattr__(self, name, value)

    state = property(lambda self: self._state)
    current = property(lambda self: self._current)
    total = property(lambda self: self._total)
    cache_summary = property(lambda self: self._cache_summary)
    diagnostic = property(
        lambda self: copy_session_payload(self._diagnostic) if self._diagnostic else None)
    can_capture = property(lambda self: self._can_capture)
    can_upload = property(lambda self: self._can_upload)
    can_cancel = property(lambda self: self._can_cancel)
    can_leave = property(lambda self: self._can_leave)
    # Source frame the current attempt is positioned on (pause/seek), None
    # while Unreal has not applied a pose in this attempt.
    source_frame = property(lambda self: self._source_frame)
    loop_enabled = property(lambda self: self._loop_enabled)
    loop_round = property(lambda self: self._loop_round)
    # Bounds of the last completed playback segment; both are None while the
    # whole cache still plays as one unbroken segment.
    segment_start = property(lambda self: self._segment_start)
    segment_end = property(lambda self: self._segment_end)

    def _values(self):
        diagnostic = tuple(sorted((self._diagnostic or {}).items()))
        return (self._state, self._current, self._total, self._cache_summary,
                diagnostic, self._can_capture, self._can_upload,
                self._can_cancel, self._can_leave, self._source_frame,
                self._loop_enabled, self._loop_round, self._segment_start,
                self._segment_end)

    def __eq__(self, other):
        return isinstance(other, CachedPlaybackView) and self._values() == other._values()

    def __ne__(self, other):
        return not self == other


class CachedPlaybackError(RuntimeError):
    def __init__(self, code, message="", details=""):
        self._code = code
        self._message = message or code
        self._details = details or self._message
        super(CachedPlaybackError, self).__init__(self._message)

    @property
    def code(self):
        return self._code

    @property
    def message(self):
        return self._message

    @property
    def details(self):
        return self._details


class CachedPlaybackRetention(object):
    __slots__ = ("__cache",)

    def __init__(self, cache):
        self.__cache = cache if cache is not None and cache.completed else None

    def _consume(self, revision):
        cache, self.__cache = self.__cache, None
        if cache is None:
            return None
        try:
            if not cache.compatible_with(revision):
                cache.delete()
                return None
        except (PlaybackCacheError, AttributeError, TypeError, ValueError):
            cache.delete()
            return None
        return cache

    def _discard(self):
        cache, self.__cache = self.__cache, None
        if cache is not None:
            cache.delete()

    def _summary(self):
        return (CachedPlaybackCacheSummary(self.__cache)
                if self.__cache is not None else None)


class CacheUploadResult(object):
    """One bounded step of a single cache upload.

    ``processed`` is True when this call moved or measured frames.
    ``action`` is None for a plain step (optionally
    carrying ``progress`` as a ``(current, total)`` pair for the owner's view)
    or one of ``CacheUpload.CORRUPT`` / ``TRANSPORT`` / ``FAILED`` carrying
    the terminal ``detail`` for the owner to route.
    """

    __slots__ = ("processed", "action", "detail", "progress")

    def __init__(self, processed, action=None, detail=None, progress=None):
        self.processed = processed
        self.action = action
        self.detail = detail
        self.progress = progress


class CacheUpload(object):
    """Owns one upload pass inside the directly runnable Maya module.

    Single owner of the exact byte declaration, the bounded chunk pump, the
    frames-file iterator lifetime, the backpressure wait, and the
    post-drain Ready deadline. The owning ``CachedPlayback`` keeps cache
    retention, upload/play ID allocation, Ready identity verification, and
    the user-facing capture/replay/detach states; it only routes the
    returned ``CacheUploadResult`` to its existing terminal paths.
    """

    CORRUPT = "corrupt"
    TRANSPORT = "transport"
    FAILED = "failed"

    def __init__(self, submit, is_drained):
        self._submit = submit
        self._is_drained = is_drained
        self._cache = None
        self._upload_id = None
        self._revision = None
        self._declared_bytes = 0
        self._declaring = False
        self._frame_iter = None
        self._submitted = 0
        self._end_sent = False
        self._deadline = None

    @property
    def end_sent(self):
        return self._end_sent

    def start(self, cache, identity, declared_hint_bytes=0):
        """Begin one upload: declare bytes, send cache_begin, open the pass."""
        self._cache = cache
        self._upload_id, self._revision = identity
        declared = int(declared_hint_bytes or 0)
        if declared <= 0 and cache.frame_count:
            # Adopted cache from another session instance: derive the exact
            # encoded total inside bounded poller chunks so the declaring
            # pass never blocks Maya's thread on a whole-cache encode.
            self._declaring = True
            declared = 0
        else:
            self._declaring = False
        self._declared_bytes = declared
        self._submitted = 0
        self._end_sent = False
        self._deadline = None
        if not self._declaring:
            outcome = self._send_begin()
            if outcome is not None:
                return outcome
        opened = self._open_iter()
        if opened is not None:
            return opened
        return CacheUploadResult(False)

    def advance(self):
        """Push at most one bounded chunk; Maya's thread never stalls."""
        if (not self._declaring and self._end_sent
                and self._is_drained() and self._deadline is None):
            self._deadline = time.time() + UPLOAD_READY_TIMEOUT_SECONDS
        if self._deadline is not None and time.time() > self._deadline:
            return CacheUploadResult(False, self.TRANSPORT, make_diagnostic(
                "STREAM_INTERRUPTED",
                "Timed out waiting for Unreal to accept the uploaded cache."))
        if self._declaring:
            return self._pump_declaration()
        return self._pump_frames()

    def close(self):
        iterator, self._frame_iter = self._frame_iter, None
        if iterator is None:
            return
        close = getattr(iterator, "close", None)
        if close is not None:
            try:
                close()
            except (RuntimeError, TypeError):
                pass

    def _send_begin(self):
        cache = self._cache
        start_frame, end_frame = cache.capture_range
        try:
            begin_message = make_cache_begin_message(
                self._revision, start_frame, end_frame,
                cache.scene_fps, self._declared_bytes, self._upload_id)
        except ValueError as exc:
            return CacheUploadResult(False, self.FAILED, make_diagnostic(
                "CACHED_UPLOAD_FAILED",
                "The captured cache exceeds the Unreal transient cache limit.",
                details=str(exc)))
        try:
            self._submit(begin_message)
        except (CachedPlaybackError, StreamingSessionError) as error:
            return CacheUploadResult(False, self.TRANSPORT, error)
        return None

    def _open_iter(self):
        try:
            self._frame_iter = self._cache.iter_frames()
        except (PlaybackCacheError, OSError) as error:
            self.close()
            return CacheUploadResult(False, self.CORRUPT, error)
        return None

    def _pump_frames(self):
        """Push at most one bounded chunk of cached frames per call."""
        if self._frame_iter is None or not self._is_drained():
            return CacheUploadResult(False)  # previous chunk still draining
        total_frames = self._cache.frame_count
        submitted = 0
        progress = None
        while submitted < UPLOAD_CHUNK_FRAMES:
            try:
                frame = next(self._frame_iter)
            except StopIteration:
                # Release the frames-file handle before declaring wire done.
                self.close()
                if not self._end_sent:
                    try:
                        self._submit(make_cache_end_message())
                    except (CachedPlaybackError,
                            StreamingSessionError) as error:
                        return CacheUploadResult(False, self.TRANSPORT, error)
                    self._end_sent = True
                break
            except (PlaybackCacheError, UnicodeDecodeError, OSError,
                    TypeError, ValueError) as error:
                # Any decode or iteration failure is deterministic cache
                # corruption: invalidate the whole cache instead of
                # masquerading as a transport failure.
                self.close()
                return CacheUploadResult(submitted > 0, self.CORRUPT, error)
            try:
                message = make_cache_frame_message(
                    self._submitted,
                    frame["transforms"], frame["curves"])
            except (TypeError, ValueError) as error:
                # A cached frame that cannot be re-encoded is deterministic
                # cache corruption, never a transport failure.
                self.close()
                return CacheUploadResult(submitted > 0, self.CORRUPT, error)
            try:
                self._submit(message)
            except (CachedPlaybackError,
                    StreamingSessionError) as error:
                return CacheUploadResult(False, self.TRANSPORT, error)
            self._submitted += 1
            submitted += 1
            if (self._submitted % UPLOAD_CHUNK_FRAMES == 0
                    and self._submitted < total_frames):
                progress = (self._submitted, total_frames)
        return CacheUploadResult(submitted > 0, progress=progress)

    def _pump_declaration(self):
        """Measure at most one bounded chunk of cached frames per call.

        Adopted caches have no captured wire sizes, so the exact encoded
        declaration is derived here — in bounded poller chunks instead of one
        blocking whole-cache pass.
        """
        if self._frame_iter is None:
            return CacheUploadResult(False)
        measured = 0
        while measured < UPLOAD_CHUNK_FRAMES:
            try:
                frame = next(self._frame_iter)
            except StopIteration:
                self.close()
                self._declaring = False
                begin_outcome = self._send_begin()
                if begin_outcome is not None:
                    return CacheUploadResult(
                        measured > 0, begin_outcome.action, begin_outcome.detail)
                # Reopen the frames file for the pipelined submission pass;
                # indices and progress restart from zero for the transfer.
                reopened = self._open_iter()
                if reopened is not None:
                    return CacheUploadResult(
                        measured > 0, reopened.action, reopened.detail)
                self._end_sent = False
                self._submitted = 0
                break
            except (PlaybackCacheError, UnicodeDecodeError, OSError,
                    TypeError, ValueError) as error:
                self.close()
                return CacheUploadResult(measured > 0, self.CORRUPT, error)
            try:
                frame_bytes = cache_frame_wire_size(
                    self._submitted,
                    frame["transforms"], frame["curves"])
            except (TypeError, ValueError) as error:
                self.close()
                return CacheUploadResult(measured > 0, self.CORRUPT, error)
            # Overflow-safe accumulation against the frozen wire limit; an
            # adopted cache larger than the limit fails fast, exactly like a
            # captured one would at declaration time.
            if (frame_bytes < 0
                    or self._declared_bytes
                    > MAX_CACHE_PAYLOAD_BYTES - frame_bytes):
                self.close()
                self._declaring = False
                return CacheUploadResult(measured > 0, self.FAILED,
                    make_diagnostic(
                        "CACHED_UPLOAD_FAILED",
                        "The captured cache exceeds the Unreal transient cache"
                        " limit.",
                        details="declared {0} bytes plus frame {1} exceeds the"
                                " limit of {2} bytes".format(
                                    self._declared_bytes,
                                    self._submitted,
                                    MAX_CACHE_PAYLOAD_BYTES)))
            self._declared_bytes += frame_bytes
            self._submitted += 1
            measured += 1
        return CacheUploadResult(measured > 0)


class CachedPlayback(object):
    """Owns Maya capture and upload; observes Unreal's playback outcomes.

    Capture stores an inclusive frozen range in a temporary disk cache.
    Completion uploads the whole cache and waits for Unreal's Ready reply.
    Only Unreal initiates playback of the accepted cache.
    """

    REALTIME = "REALTIME"
    CACHED_IDLE = "CACHED_IDLE"
    CAPTURING = "CAPTURING"
    UPLOADING = "UPLOADING"
    READY = "READY"
    REPLAYING = "REPLAYING"
    PAUSED = "PAUSED"
    SEEKED = "SEEKED"
    STOPPING = "STOPPING"
    COMPLETED = "COMPLETED"
    STOPPED = "STOPPED"
    FAILED = "FAILED"
    DETACHED = "DETACHED"

    def __init__(self, scene, streaming_session, on_change=None, timeline=None,
                 cache_factory=None, temp_dir=None, disk_usage=None,
                 timer_api=None, retention=None):
        self._scene = scene
        self._streaming_session = streaming_session
        self._on_change = on_change
        self._timeline = timeline or MayaTimeline()
        self._cache_factory = cache_factory or PlaybackCache
        self._temp_dir = os.fspath(temp_dir or tempfile.gettempdir())
        self._disk_usage = disk_usage or shutil.disk_usage
        self._timer_api = timer_api if timer_api is not None else getattr(om, "MTimerMessage", None)
        self._cache = None
        self._phase = "idle"
        self._paused_streaming = False
        self._capture_cache = None
        self._capture_cancel_requested = False
        self._capture_generation = 0
        self._capture_timer_id = None
        self._capture_revision = None
        self._capture_start = None
        self._capture_end = None
        self._capture_next_frame = None
        self._capture_total_frames = None
        self._capture_original_frame = None
        self._capture_confirmation_callback = None
        self._capture_estimated_size = None
        self._capture_encoded_bytes = 0
        self._capture_large_cache_confirmed = False
        self._awaiting_clear = None
        self._clear_wait_deadline = None
        self._replies = queue.Queue()
        self._poller_generation = 0
        self._poller_timer_id = None
        self._uploaded_revision = None
        self._applied_frames = None
        self._source_frame = None
        self._loop_enabled = False
        self._loop_round = 0
        self._segment_start = None
        self._segment_end = None
        self._upload_id = 0
        self._play_id = 0
        self._clear_id = 0
        self._active_upload_id = None
        self._active_upload_revision = None
        self._active_play_id = None
        self._upload = None
        self._closed = False
        self._progress_current = 0
        self._progress_total = 0
        self._issued_retention = None
        revision = self._ensure_ready()
        if retention is not None:
            self._cache = retention._consume(revision)
        self._last_view = self._make_view()

    def _semantic_state(self):
        if self._closed or self._phase in ("closed", "detached", "transport_failed"):
            return self.DETACHED
        if self._phase == "capturing":
            return self.CAPTURING
        if self._phase in ("captured", "uploading"):
            return self.UPLOADING
        if self._phase == "ready_to_play":
            return self.READY
        if self._phase == "replaying":
            return self.REPLAYING
        if self._phase == "paused":
            return self.PAUSED
        if self._phase == "seeked":
            return self.SEEKED
        if self._phase == "stopping":
            return self.STOPPING
        if self._phase == "completed":
            return self.COMPLETED
        if self._phase == "stopped":
            return self.STOPPED
        if self._phase in ("failed", "upload_failed", "playback_failed", "cancelled"):
            # One truthful post-failure transition: a failure that already
            # resumed Real-time Preview and dropped Unreal cache ownership is
            # Real-time state carrying a diagnostic; only a failure that
            # remains cached renders as FAILED.
            return self.FAILED if self._paused_streaming else self.REALTIME
        return self.CACHED_IDLE if self._paused_streaming else self.REALTIME

    def _reset_attempt_state(self):
        """Drop the interactive state of a playback attempt Maya no longer owns.

        Pause/seek positions, the loop selection and the round counter all
        describe one attempt of one cache; a fresh capture, a clear, or a new
        attempt never inherits them.
        """
        self._source_frame = None
        self._loop_enabled = False
        self._loop_round = 0
        self._segment_start = None
        self._segment_end = None

    def _cache_summary(self):
        if self._cache is not None and self._cache.completed:
            return CachedPlaybackCacheSummary(self._cache)
        if self._issued_retention is not None:
            return self._issued_retention._summary()
        return None

    def _make_view(self, diagnostic=None):
        state = self._semantic_state()
        session = self._streaming_session
        attached = (not self._closed and session is not None and session.is_ready)
        entered = attached and self._paused_streaming
        summary = self._cache_summary()
        return CachedPlaybackView(
            state, self._progress_current, self._progress_total, summary, diagnostic,
            can_capture=(entered
                         and state not in (self.CAPTURING, self.STOPPING, self.DETACHED)),
            can_upload=(entered and self._cache is not None
                        and self._cache.completed
                        and self._uploaded_revision != self._cache.snapshot_revision
                        and state not in (self.CAPTURING, self.UPLOADING)),
            can_cancel=state == self.CAPTURING,
            can_leave=entered and state not in (self.CAPTURING, self.DETACHED),
            source_frame=self._source_frame,
            loop_enabled=self._loop_enabled,
            loop_round=self._loop_round,
            segment_start=self._segment_start,
            segment_end=self._segment_end,
        )

    @property
    def view(self):
        return self._make_view()

    def _publish(self, current=None, total=None, diagnostic=None):
        if current is not None:
            self._progress_current = current
        if total is not None:
            self._progress_total = total
        view = self._make_view(diagnostic)
        if view == self._last_view:
            return
        self._last_view = self._make_view()
        if self._on_change is None:
            return
        try:
            self._on_change(view)
        except Exception as exc:
            if cmds is not None:
                try:
                    cmds.warning("MtoU_LiveLink cached playback callback failed: {0}".format(exc))
                except Exception:
                    pass

    def _current_revision(self):
        try:
            return int(self._scene.snapshot().revision)
        except (AttributeError, RuntimeError, TypeError, ValueError):
            session = self._streaming_session
            revision = session.revision if session is not None else None
            return None if revision is None else int(revision)

    def _ensure_ready(self):
        session = self._streaming_session
        if self._closed or session is None or not session.is_ready:
            raise CachedPlaybackError(
                "CACHED_PLAYBACK_NOT_READY",
                "Cached Playback requires a ready negotiated connection.")
        expected = session.revision
        current = self._current_revision()
        if expected is not None and current is not None and expected != current:
            raise CachedPlaybackError(
                "CACHED_PLAYBACK_INCOMPATIBLE",
                "The character snapshot changed during cached playback.",
                "streaming revision {0}, current revision {1}".format(expected, current))
        return expected if expected is not None else current

    def reply_sink(self):
        """Where Unreal's replies go while this playback owns the connection."""
        return self._replies.put

    def _pause_streaming(self):
        if self._paused_streaming:
            return
        if self._streaming_session is not None:
            self._streaming_session.pause_for_cached(self._replies.put)
        self._paused_streaming = True

    def _resume_streaming(self):
        if not self._paused_streaming:
            return
        if self._streaming_session is not None:
            self._streaming_session.resume_from_cached()
        self._paused_streaming = False

    def enter(self):
        self._ensure_ready()
        if self._paused_streaming:
            return self
        self._pause_streaming()
        self._phase = "idle"
        self._send_enter_best_effort()
        # Every Cached Playback state that owns the negotiated session keeps
        # the lightweight transport observer running, so a disconnected
        # socket is detected while idle instead of surviving until the next
        # user action.
        self._add_playback_poller()
        self._publish(current=0, total=0)
        return self

    def _send_enter_best_effort(self):
        """Establish cached ownership in Unreal before any capture begins."""
        session = self._streaming_session
        if session is None or not session.is_ready:
            return
        try:
            session.submit_cached(make_cache_enter_message())
        except (CachedPlaybackError, StreamingSessionError,
                RuntimeError, TypeError, ValueError) as exc:
            if cmds is not None:
                try:
                    cmds.warning(
                        "MtoU_LiveLink could not send the cache entry"
                        " control: {0}".format(exc))
                except Exception:
                    pass

    def leave(self):
        if self._closed or not self._paused_streaming:
            return
        if self._phase == "capturing":
            self._capture_failure(CachedPlaybackError(
                "CACHED_PLAYBACK_CANCELLED", "Cached capture was cancelled."))
        # Queue the Unreal cache clear while ordered delivery is active, then
        # resume streaming so live frames follow the clear in order.
        self._teardown_cached_runtime(send_clear=True, resume_streaming=True)
        self._uploaded_revision = None
        self._applied_frames = None
        self._reset_attempt_state()
        self._phase = "idle"
        self._publish(current=0, total=0)

    def cancel_capture(self):
        if self._phase == "capturing":
            self._capture_cancel_requested = True

    def _disk_free_bytes(self):
        try:
            usage = self._disk_usage(self._temp_dir)
            if hasattr(usage, "free"):
                return int(usage.free)
            if isinstance(usage, (tuple, list)) and len(usage) >= 3:
                return int(usage[2])
            return int(usage)
        except (OSError, TypeError, ValueError) as exc:
            raise CachedPlaybackError(
                "CACHED_PLAYBACK_SPACE",
                "Unable to determine available temporary-disk space.",
                details=str(exc))

    @staticmethod
    def _invoke_confirmation(callback, estimated_size, total_frames):
        if callback is None:
            return True
        try:
            return bool(callback(estimated_size, total_frames))
        except TypeError:
            return bool(callback(estimated_size))

    def _make_cache(self, revision, capture_start, capture_end, scene_fps, capture_time):
        begin = getattr(self._cache_factory, "begin", None)
        if begin is None:
            raise PlaybackCacheError("cache factory must provide begin()")
        return begin(
            snapshot_revision=revision,
            capture_start=capture_start,
            capture_end=capture_end,
            scene_fps=scene_fps,
            temp_dir=self._temp_dir,
            capture_time=capture_time,
        )


    def _capture_timer_interval(self):
        return 0.001

    def _add_capture_timer(self):
        if self._timer_api is None or not hasattr(self._timer_api, "addTimerCallback"):
            return
        self._capture_generation += 1
        generation = self._capture_generation

        def on_timer(*args):
            del args
            if generation != self._capture_generation:
                return
            self._capture_step()

        self._capture_timer_id = self._timer_api.addTimerCallback(
            self._capture_timer_interval(), on_timer)

    def _remove_capture_timer(self):
        self._capture_generation += 1
        timer_id, self._capture_timer_id = self._capture_timer_id, None
        self._remove_timer_callback(timer_id)

    def _remove_timer_callback(self, timer_id):
        if timer_id is None:
            return
        remove = getattr(self._timer_api, "removeCallback", None)
        if remove is None:
            remove = getattr(getattr(om, "MMessage", None), "removeCallback", None)
        if remove is not None:
            try:
                remove(timer_id)
            except RuntimeError:
                pass

    def _restore_capture_frame(self):
        original_frame = self._capture_original_frame
        if original_frame is None:
            return
        try:
            self._timeline.set_frame(original_frame)
        except (AttributeError, RuntimeError, TypeError, ValueError):
            pass
        self._capture_original_frame = None

    def capture(self, scene_fps, confirm_large_cache=None, capture_range=None):
        if self._phase == "capturing":
            return self
        revision = self._ensure_ready()
        playback_range = (self._timeline.playback_range()
                          if capture_range is None else None)
        start_frame, end_frame = capture_frame_range(playback_range, capture_range)
        scene_fps = validate_frame_rate(scene_fps)
        had_cached_ownership = self._paused_streaming
        self.enter()
        if had_cached_ownership:
            # UE must acknowledge dropping the old cache and ending any active
            # playback before Maya samples the first frame of a new capture.
            self._awaiting_clear = self._send_clear_best_effort()
            self._clear_wait_deadline = time.monotonic() + UPLOAD_READY_TIMEOUT_SECONDS
            self._send_enter_best_effort()
        else:
            self._awaiting_clear = None
            self._clear_wait_deadline = None
        if self._phase == "uploading":
            # Abort the in-flight transfer; the fresh cache_begin below resets
            # Unreal's buffer coherently, so stale frames cannot mix.
            self._discard_upload()
        if self._cache is not None:
            self._cache.delete()
            self._cache = None
        self._uploaded_revision = None
        self._applied_frames = None
        self._reset_attempt_state()
        original_frame = self._timeline.current_frame()
        self._capture_original_frame = original_frame
        if self._timeline.is_playing():
            self._timeline.stop()
        total_frames = end_frame - start_frame + 1
        try:
            cache = self._make_cache(
                revision, start_frame, end_frame, scene_fps, time.time())
        except (PlaybackCacheError, OSError, TypeError, ValueError) as exc:
            error = CachedPlaybackError(
                "CACHED_PLAYBACK_SPACE", str(exc), details=str(exc))
            raise self._capture_failure(error)
        self._capture_cache = cache
        self._capture_revision = revision
        self._capture_start = start_frame
        self._capture_end = end_frame
        self._capture_next_frame = start_frame
        self._capture_total_frames = total_frames
        self._capture_original_frame = original_frame
        self._capture_confirmation_callback = confirm_large_cache
        self._capture_estimated_size = None
        self._capture_encoded_bytes = 0
        self._capture_large_cache_confirmed = False
        self._capture_cancel_requested = False
        self._phase = "capturing"
        self._add_playback_poller()
        self._publish(current=0, total=total_frames)
        try:
            self._add_capture_timer()
        except (RuntimeError, TypeError, ValueError) as exc:
            error = CachedPlaybackError(
                "INTERNAL_ERROR", str(exc), details=str(exc))
            failure = self._capture_failure(error)
            if failure is not None:
                raise failure
        return self

    def _teardown_cached_runtime(self, send_clear=False, resume_streaming=False):
        """Single terminal cleanup seam for every cached-mode exit.

        Removes both timers, closes any upload iterator, optionally queues an
        ordered ``cache_clear`` while ordered delivery is still active, and
        only then resumes live streaming so the clear stays ahead of the
        first resumed live pose.
        """
        self._remove_capture_timer()
        self._awaiting_clear = None
        self._clear_wait_deadline = None
        self._remove_playback_poller()
        self._discard_upload()
        if send_clear:
            self._send_clear_best_effort()
        if resume_streaming:
            self._resume_streaming()

    def _capture_failure(self, error, transport_lost=False):
        # Timers and iterators die first so no further capture work fires.
        self._teardown_cached_runtime()
        if self._capture_cache is not None:
            self._capture_cache.delete()
        self._capture_cache = None
        self._restore_capture_frame()
        diagnostic = make_diagnostic(
            error.code if error.code in DIAGNOSTICS else "INTERNAL_ERROR",
            error.message, details=error.details)
        if transport_lost:
            self._phase = "transport_failed"
            self._publish(current=0, total=0, diagnostic=diagnostic)
            if self._streaming_session is not None:
                try:
                    self._streaming_session.stop()
                except (RuntimeError, TypeError):
                    pass
            return error
        self._phase = "cancelled" if error.code == "CACHED_PLAYBACK_CANCELLED" else "failed"
        # A failed capture leaves Unreal owning cached entry state; drop it in
        # order before live poses flow again. Resuming Real-time Preview here
        # and clearing Unreal ownership is the complete, truthful transition:
        # the published view renders as REALTIME with this diagnostic.
        self._send_clear_best_effort()
        self._resume_streaming()
        self._publish(current=0, total=0, diagnostic=diagnostic)
        if error.code == "CACHED_PLAYBACK_CANCELLED":
            return None
        return error

    def _capture_step(self):
        if self._phase != "capturing":
            return False
        try:
            if self._capture_cancel_requested:
                error = CachedPlaybackError(
                    "CACHED_PLAYBACK_CANCELLED", "Cached capture was cancelled.")
                self._capture_failure(error)
                return True
            if self._awaiting_clear is not None:
                return False
            session = self._streaming_session
            if session is None or not session.is_ready:
                self._handle_transport_failure(CachedPlaybackError(
                    "STREAM_INTERRUPTED",
                    "The streaming connection ended during capture."))
                return True
            frame_number = self._capture_next_frame
            self._timeline.set_frame(frame_number)
            frame = self._scene.sample()
            if int(frame.revision) != int(self._capture_revision):
                raise CachedPlaybackError(
                    "CACHED_PLAYBACK_INCOMPATIBLE",
                    "The character snapshot changed during capture.",
                    "captured revision {0}, expected {1}".format(
                        frame.revision, self._capture_revision))
            frame_message = make_frame_message(
                list(frame.transforms), list(frame.curves))
            frame_size = PlaybackCache.serialized_frame_size(frame_message)
            # Exact wire size of this frame's cache_frame message (real index
            # width included, no file newline) accumulated for the upload
            # declaration so it matches what Unreal meters on the socket.
            encoded_size = cache_frame_wire_size(
                frame_number - self._capture_start,
                frame_message["transforms"],
                frame_message["curves"])
            if (encoded_size < 0
                    or encoded_size > MAX_MESSAGE_BYTES
                    or self._capture_encoded_bytes + encoded_size
                    > MAX_CACHE_PAYLOAD_BYTES):
                # Stop as soon as a single frame would cross the per-message
                # framing ceiling or the encoded total would cross the frozen
                # 1 GiB budget; the partial owned cache is deleted below.
                raise CachedPlaybackError(
                    "CACHED_PLAYBACK_PAYLOAD_LIMIT",
                    "Captured frames would exceed the fixed cache limits.",
                    details="frame {0} bytes, encoded total {1} + {2} bytes"
                            " against limits {3} per message and {4} per"
                            " cache".format(
                                encoded_size, self._capture_encoded_bytes,
                                encoded_size, MAX_MESSAGE_BYTES,
                                MAX_CACHE_PAYLOAD_BYTES))
            self._capture_encoded_bytes += encoded_size
            if self._capture_estimated_size is None:
                self._capture_estimated_size = frame_size
            else:
                self._capture_estimated_size = max(self._capture_estimated_size, frame_size)
            estimated_total = self._capture_estimated_size * self._capture_total_frames
            frames_written = frame_number - self._capture_start
            remaining_estimate = self._capture_estimated_size * (
                self._capture_total_frames - frames_written)
            free_bytes = self._disk_free_bytes()
            if remaining_estimate > free_bytes:
                raise CachedPlaybackError(
                    "CACHED_PLAYBACK_SPACE",
                    "The temporary directory does not have enough free space.",
                    "estimated remaining {0} bytes, free {1} bytes".format(
                        remaining_estimate, free_bytes))
            if (estimated_total > CACHE_CONFIRMATION_BYTES
                    and not self._capture_large_cache_confirmed):
                if not self._invoke_confirmation(
                        self._capture_confirmation_callback,
                        estimated_total, self._capture_total_frames):
                    raise CachedPlaybackError(
                        "CACHED_PLAYBACK_CANCELLED",
                        "Large cache confirmation was declined.")
                self._capture_large_cache_confirmed = True
            self._capture_cache.append(frame_message)
            offset = frame_number - self._capture_start + 1
            self._publish(current=offset, total=self._capture_total_frames)
            if self._capture_cancel_requested:
                error = CachedPlaybackError(
                    "CACHED_PLAYBACK_CANCELLED", "Cached capture was cancelled.")
                self._capture_failure(error)
                return True
            self._capture_next_frame += 1
            if frame_number < self._capture_end:
                return True
            cache = self._capture_cache
            cache.finalize()
            self._remove_capture_timer()
            self._capture_cache = None
            self._cache = cache
            self._restore_capture_frame()
            self._publish(current=self._capture_total_frames,
                          total=self._capture_total_frames)
            self._phase = "captured"
            self._begin_upload()
            return True
        except CachedPlaybackError as error:
            self._capture_failure(error)
            return True
        except (CharacterSceneError, PlaybackCacheError, RuntimeError, ValueError) as exc:
            error = CachedPlaybackError(
                "SAMPLING_FAILED", str(exc), details=str(exc))
            self._capture_failure(error)
            return True

    def _submit_cached(self, message):
        if self._streaming_session is None:
            raise CachedPlaybackError(
                "CACHED_PLAYBACK_NOT_READY",
                "The streaming session cannot submit cached messages.")
        self._streaming_session.submit_cached(message)

    def _delivery_drained(self):
        session = self._streaming_session
        return True if session is None else session.cached_delivery_drained()

    def _accept_cache_ready(self, reply):
        if self._phase != "uploading" or self._cache is None:
            return
        if exact_int(reply.get("upload_id")) != self._active_upload_id:
            return  # stale outcome from an older upload
        upload = self._upload
        if upload is None or not upload.end_sent or not self._delivery_drained():
            # Unreal cannot truthfully report Ready before cache_end was sent
            # and drained. Put it back and stop draining this round so the
            # same outcome is not re-consumed in a tight loop.
            self._replies.put(reply)
            self._stop_draining_this_round = True
            return
        total_frames = self._cache.frame_count
        echoed_count = exact_int(reply.get("frame_count"))
        echoed_revision = exact_int(reply.get("revision"))
        if (echoed_count != total_frames
                or echoed_revision != self._active_upload_revision):
            self._fail_upload(make_diagnostic(
                "CACHED_UPLOAD_FAILED",
                "Unreal reported inconsistent cache evidence.",
                details="ready frame_count={0} revision={1}, expected"
                        " frame_count={2} revision={3}".format(
                            echoed_count, echoed_revision,
                            total_frames, self._active_upload_revision)))
            return
        self._uploaded_revision = self._cache.snapshot_revision
        self._phase = "ready_to_play"
        self._publish(current=total_frames, total=total_frames)

    def _route_outcome(self, reply):
        """Identity-aware routing: late outcomes from older operations are
        dropped without touching current state."""
        reply_type = reply.get("type")
        if reply_type == "cache_cleared":
            if (self._phase == "capturing" and self._awaiting_clear is not None
                    and exact_int(reply.get("clear_id")) == self._awaiting_clear):
                self._awaiting_clear = None
                self._clear_wait_deadline = None
            return
        if reply_type == "cache_playing":
            play_id = exact_int(reply.get("play_id"))
            upload_id = exact_int(reply.get("upload_id"))
            if (self._phase in ("ready_to_play", "stopped", "completed", "playback_failed")
                    and self._cache is not None
                    and self._uploaded_revision == self._cache.snapshot_revision
                    and upload_id == self._active_upload_id
                    and play_id is not None and play_id > self._play_id):
                self._play_id = play_id
                self._active_play_id = play_id
                self._applied_frames = None
                # Every attempt starts as one whole-cache segment: no pose was
                # displayed yet, and the round counter owns the loop display.
                self._reset_attempt_state()
                self._loop_enabled = bool(exact_bool(reply.get("loop")))
                self._phase = "replaying"
                self._publish(current=0, total=self._cache.frame_count)
            return
        if reply_type == "cache_paused":
            play_id = exact_int(reply.get("play_id"))
            applied = exact_int(reply.get("applied_frames"))
            cache = self._cache
            if (cache is not None
                    and self._uploaded_revision == cache.snapshot_revision
                    and self._phase == "replaying"
                    and play_id == self._active_play_id
                    and applied is not None):
                # Unreal holds the pose: the applied count of the current
                # segment and the frame the character is frozen on are the
                # only evidence a pause carries.
                self._applied_frames = applied
                self._source_frame = exact_int(reply.get("source_frame"))
                self._phase = "paused"
                self._publish(current=applied, total=cache.frame_count)
            return
        if reply_type == "cache_resumed":
            play_id = exact_int(reply.get("play_id"))
            applied = exact_int(reply.get("applied_frames"))
            cache = self._cache
            if (cache is not None
                    and self._uploaded_revision == cache.snapshot_revision
                    and self._phase in ("paused", "seeked")
                    and play_id == self._active_play_id
                    and applied is not None):
                # Resuming rebuilds Unreal's timing baseline from the held
                # position: no frame of the stopped interval is replayed.
                self._applied_frames = applied
                self._source_frame = exact_int(reply.get("source_frame"))
                self._phase = "replaying"
                self._publish(current=applied, total=cache.frame_count)
            return
        if reply_type == "cache_seeked":
            play_id = exact_int(reply.get("play_id"))
            source_frame = exact_int(reply.get("source_frame"))
            applied = exact_int(reply.get("applied_frames"))
            cache = self._cache
            if cache is None or self._uploaded_revision != cache.snapshot_revision:
                return
            start_frame, end_frame = cache.capture_range
            if source_frame is None or not start_frame <= source_frame <= end_frame:
                # Out-of-range seek evidence is rejected outright, never
                # clamped onto a neighbouring captured frame the user did not
                # ask for.
                return
            if self._phase in ("ready_to_play", "stopped", "completed",
                               "playback_failed"):
                # Positioning outside an attempt opens one, exactly like a
                # fresh play, so every later outcome carries an identity.
                if play_id is None or play_id <= self._play_id:
                    return
                self._play_id = play_id
                self._active_play_id = play_id
            elif self._phase in ("replaying", "paused", "seeked"):
                if play_id != self._active_play_id:
                    return
            else:
                return
            if applied is None:
                return
            self._applied_frames = applied
            self._source_frame = source_frame
            self._phase = "seeked"
            self._publish(current=applied, total=cache.frame_count)
            return
        if reply_type == "cache_looped":
            play_id = exact_int(reply.get("play_id"))
            round_number = exact_int(reply.get("round"))
            cache = self._cache
            if (cache is not None
                    and self._uploaded_revision == cache.snapshot_revision
                    and self._phase == "replaying"
                    and play_id == self._active_play_id
                    and round_number is not None
                    and round_number > self._loop_round):
                # The round counter only ever grows within one attempt, and the
                # new round starts before its first pose is applied.
                self._loop_round = round_number
                self._publish(current=0, total=cache.frame_count)
            return
        if reply_type == "cache_loop_changed":
            play_id = exact_int(reply.get("play_id"))
            loop = exact_bool(reply.get("loop"))
            cache = self._cache
            if (cache is not None
                    and self._uploaded_revision == cache.snapshot_revision
                    and self._phase in ("replaying", "paused", "seeked")
                    and play_id == self._active_play_id
                    and loop is not None):
                self._loop_enabled = loop
                self._publish()
            return
        if reply_type == "error":
            play_id = exact_int(reply.get("play_id"))
            upload_id = exact_int(reply.get("upload_id"))
            if self._phase == "uploading":
                if upload_id == self._active_upload_id:
                    self._handle_error_reply(reply)
            elif self._phase in ("ready_to_play", "replaying", "stopping"):
                if play_id == self._active_play_id:
                    self._handle_error_reply(reply)
            return
        if reply_type == "cache_progress":
            play_id = exact_int(reply.get("play_id"))
            applied = exact_int(reply.get("applied"))
            if self._phase == "replaying" and play_id == self._active_play_id:
                # Informational only; never completion evidence. Throttled to
                # roughly 2% steps so the UI is not flooded at scene rate.
                total_frames = self._cache.frame_count if self._cache else None
                if applied is None or (
                        total_frames is not None and not 0 <= applied <= total_frames):
                    return
                step = max(1, round(total_frames / 50)) if total_frames else 1
                if total_frames is None or applied % step == 0 \
                        or applied >= total_frames:
                    self._publish(current=applied, total=total_frames)
            return
        if reply_type == "cache_complete":
            play_id = exact_int(reply.get("play_id"))
            if self._phase != "replaying" or play_id != self._active_play_id:
                return
            cache = self._cache
            total_frames = cache.frame_count if cache is not None else None
            applied = exact_int(reply.get("applied_frame_count"))
            elapsed = finite_float(reply.get("elapsed_seconds"))
            scope = reply.get("scope")
            start_frame = exact_int(reply.get("start_frame"))
            end_frame = exact_int(reply.get("end_frame"))
            segment = None
            if scope == "cache":
                # Only an unbroken whole-cache play may claim the old
                # one-attempt semantics: every cached frame applied once.
                consistent = (total_frames is not None and applied == total_frames)
            elif scope == "segment":
                # A seek or a finished loop round ends a segment: the segment's
                # own frame count must equal the range it reports, and that
                # range must address captured frames.
                consistent = (cache is not None
                              and start_frame is not None
                              and end_frame is not None
                              and applied is not None
                              and applied == end_frame - start_frame + 1)
                if consistent:
                    cache_start, cache_end = cache.capture_range
                    consistent = (cache_start <= start_frame <= end_frame
                                  <= cache_end)
                if consistent:
                    segment = (start_frame, end_frame)
            else:
                consistent = False
            if (not consistent or elapsed is None or elapsed < 0.0
                    or (applied is not None and applied > 1 and elapsed == 0.0)):
                self._phase = "failed"
                self._publish(diagnostic=make_diagnostic(
                    "INTERNAL_ERROR",
                    "Unreal reported inconsistent playback evidence.",
                    details="completion scope={0} applied_frame_count={1}"
                            " elapsed_seconds={2} start_frame={3} end_frame={4},"
                            " cached frames {5}".format(
                                scope, applied, elapsed, start_frame, end_frame,
                                total_frames)))
                return
            self._applied_frames = applied
            self._segment_start, self._segment_end = (
                segment if segment is not None else (None, None))
            self._phase = "completed"
            self._publish(current=applied, total=total_frames)
            return
        if reply_type == "cache_stopped":
            play_id = exact_int(reply.get("play_id"))
            if (self._phase in ("replaying", "paused", "seeked", "stopping")
                    and play_id == self._active_play_id):
                # Success is reported only once Unreal acknowledges this
                # exact attempt; a late ack for an older attempt is dropped.
                # The poller keeps running so completed and stopped states
                # still observe transport termination.
                self._phase = "stopped"
                self._publish()
            return
        if reply_type == "cache_ready":
            # A late duplicate Ready can only belong to an older upload;
            # the current upload consumes its Ready synchronously.
            if self._phase == "uploading":
                self._accept_cache_ready(reply)
            return
        # Unknown well-formed outcomes are informational.

    def _handle_error_reply(self, reply):
        code = str(reply.get("code") or "")
        message = str(reply.get("message") or code)
        details = str(reply.get("details") or message)
        if self._phase == "uploading":
            self._fail_upload(make_diagnostic(
                "CACHED_UPLOAD_FAILED", message, details=details))
            return
        if self._phase in ("replaying", "stopping"):
            # Runtime playback failures keep the negotiated cached session:
            # the poller stays running so this state also observes transport
            # termination, and leave/retry remain available capabilities.
            if code == "CACHED_PLAYBACK_PERFORMANCE":
                self._phase = "playback_failed"
                self._publish(diagnostic=make_diagnostic(
                    "CACHED_PLAYBACK_PERFORMANCE", message, details=details))
                return
            mapped = {
                "CACHE_NOT_READY": "CACHED_PLAYBACK_NO_CACHE",
                "CACHE_REVISION_MISMATCH": "CACHED_PLAYBACK_INCOMPATIBLE",
            }.get(code, "INTERNAL_ERROR")
            self._phase = "failed"
            self._publish(diagnostic=make_diagnostic(mapped, message, details=details))

    def _fail_upload(self, diagnostic):
        """Terminal path for an asynchronously rejected upload.

        Removes the poller and upload transfer, drops Unreal's cache
        ownership in order, and only then resumes the current live pose.
        """
        self._teardown_cached_runtime(send_clear=True, resume_streaming=True)
        self._phase = "upload_failed"
        self._publish(current=0, total=0, diagnostic=diagnostic)

    def _discard_upload(self):
        """Close the in-flight upload transfer and drop its ownership."""
        upload, self._upload = self._upload, None
        if upload is not None:
            upload.close()

    def _begin_upload(self):
        try:
            revision = self._ensure_ready()
        except CachedPlaybackError as error:
            if error.code == "CACHED_PLAYBACK_INCOMPATIBLE":
                self._invalidate_incompatible_cache(error)
                return
            raise
        cache = self._cache
        if revision is not None and not cache.compatible_with(revision):
            self._invalidate_incompatible_cache(CachedPlaybackError(
                "CACHED_PLAYBACK_INCOMPATIBLE",
                "The completed cache does not match the current character snapshot.",
                "streaming revision {0}, cache revision {1}".format(
                    revision, cache.snapshot_revision)))
            return
        self._pause_streaming()
        total_frames = cache.frame_count
        self._upload_id += 1
        self._active_upload_id = self._upload_id
        self._active_play_id = None
        self._phase = "uploading"
        self._publish(current=0, total=total_frames)
        # Ready arrives asynchronously: Unreal parses and buffers without any
        # real-time deadline while the poller drains the identity-matched
        # outcome, so Maya's thread never blocks on Unreal's cache parsing.
        upload = CacheUpload(
            submit=self._submit_cached, is_drained=self._delivery_drained)
        outcome = upload.start(
            cache, (self._active_upload_id, revision),
            self._capture_encoded_bytes)
        if outcome.action == CacheUpload.CORRUPT:
            self._invalidate_corrupt_cache(outcome.detail)
            return
        if outcome.action == CacheUpload.TRANSPORT:
            self._handle_transport_failure(outcome.detail)
            return
        if outcome.action == CacheUpload.FAILED:
            self._fail_upload(outcome.detail)
            return
        self._upload = upload
        self._active_upload_revision = revision
        self._add_playback_poller()

    def _playback_poll_interval(self):
        return 0.05

    def _add_playback_poller(self):
        if self._timer_api is None or not hasattr(self._timer_api, "addTimerCallback"):
            return
        if self._poller_timer_id is not None:
            return
        self._poller_generation += 1
        generation = self._poller_generation

        def on_timer(*args):
            del args
            if generation != self._poller_generation:
                return
            self._poll()

        self._poller_timer_id = self._timer_api.addTimerCallback(
            self._playback_poll_interval(), on_timer)

    def _remove_playback_poller(self):
        self._poller_generation += 1
        timer_id, self._poller_timer_id = self._poller_timer_id, None
        self._remove_timer_callback(timer_id)

    def _poll(self):
        """Drain outcomes and advance the pipelined cache upload.

        Each call pushes at most one bounded chunk of cached frames (or
        declaration measurements) toward Unreal; Maya's thread never stalls
        for the duration of an upload.
        """
        processed = False
        self._stop_draining_this_round = False
        if self._phase == "capturing" and self._capture_cancel_requested:
            self._capture_step()
            return processed
        if (self._awaiting_clear is not None and self._clear_wait_deadline is not None
                and time.monotonic() > self._clear_wait_deadline):
            self._capture_failure(CachedPlaybackError(
                "STREAM_INTERRUPTED", "Unreal did not acknowledge ending the old cache."))
            return processed
        if self._phase == "uploading":
            session = self._streaming_session
            if session is None or not session.is_ready:
                self._handle_transport_failure(make_diagnostic(
                    "STREAM_INTERRUPTED",
                    "The streaming connection ended during the cache upload."))
                return processed
            upload = self._upload
            if upload is not None:
                result = upload.advance()
                if result.action == CacheUpload.TRANSPORT:
                    self._handle_transport_failure(result.detail)
                    return processed
                if result.action == CacheUpload.CORRUPT:
                    self._invalidate_corrupt_cache(result.detail)
                elif result.action == CacheUpload.FAILED:
                    self._fail_upload(result.detail)
                elif result.progress is not None:
                    self._publish(current=result.progress[0],
                                  total=result.progress[1])
                processed = result.processed or processed
        while not self._stop_draining_this_round:
            try:
                reply = self._replies.get_nowait()
            except queue.Empty:
                break
            self._route_outcome(reply)
            processed = True
        self._stop_draining_this_round = False
        session = self._streaming_session
        if (self._paused_streaming and not self._closed
                and self._phase != "transport_failed"
                and (session is None or not session.is_ready)):
            # Continuous observation in every state that owns the negotiated
            # session: idle, captured, uploading, replaying, completed, or
            # stopped all surface transport termination immediately instead of
            # rendering a stale connected indication until the next action.
            self._handle_transport_failure(make_diagnostic(
                "STREAM_INTERRUPTED", "The streaming connection ended."))
            return processed
        return processed

    def _send_clear_best_effort(self):
        self._clear_id += 1
        clear_id = self._clear_id
        session = self._streaming_session
        if session is None or not session.is_ready:
            return clear_id
        try:
            session.submit_cached(make_cache_clear_message(clear_id))
        except (CachedPlaybackError, StreamingSessionError,
                RuntimeError, TypeError, ValueError):
            pass
        return clear_id

    def _invalidate_incompatible_cache(self, error):
        self._remove_playback_poller()
        self._discard_upload()
        if self._cache is not None:
            self._cache.delete()
        self._cache = None
        self._uploaded_revision = None
        self._phase = "failed"
        diagnostic = make_diagnostic(
            "CACHED_PLAYBACK_INCOMPATIBLE", error.message, details=error.details)
        # Unreal may still own entered or partially received state; drop it in
        # order before live poses resume.
        self._send_clear_best_effort()
        self._resume_streaming()
        self._publish(current=0, total=0, diagnostic=diagnostic)

    def _invalidate_corrupt_cache(self, error):
        self._remove_playback_poller()
        self._discard_upload()
        if self._cache is not None:
            self._cache.delete()
        self._cache = None
        self._uploaded_revision = None
        self._phase = "failed"
        diagnostic = make_diagnostic(
            "CACHED_PLAYBACK_NO_CACHE",
            "The cached frame file is incomplete or invalid.",
            details=str(error))
        # A mid-upload corruption leaves Unreal holding a partial buffer;
        # clear that ownership in order before streaming resumes.
        self._send_clear_best_effort()
        self._resume_streaming()
        self._publish(current=0, total=0, diagnostic=diagnostic)

    def _handle_transport_failure(self, error):
        if self._closed:
            return
        if not self._paused_streaming:
            return
        if self._phase == "capturing":
            # A capture interrupted by transport loss is a capture failure:
            # it owns the partial-cache cleanup and the clear-before-resume
            # ordering.
            self._capture_failure(CachedPlaybackError(
                "STREAM_INTERRUPTED",
                "The streaming connection ended during capture."),
                transport_lost=True)
            return
        self._remove_playback_poller()
        self._discard_upload()
        self._phase = "transport_failed"
        diagnostic = error if isinstance(error, dict) else make_diagnostic(
            "STREAM_INTERRUPTED", str(getattr(error, "message", error)),
            details=str(getattr(error, "details", error)))
        self._publish(diagnostic=diagnostic)
        if self._streaming_session is not None:
            try:
                self._streaming_session.stop()
            except (RuntimeError, TypeError):
                pass

    def upload_retained_cache(self):
        """Reupload a retained local cache after a new negotiated connection."""
        try:
            revision = self._ensure_ready()
        except CachedPlaybackError as error:
            if error.code == "CACHED_PLAYBACK_INCOMPATIBLE":
                self._invalidate_incompatible_cache(error)
            raise
        if self._cache is None or not self._cache.completed:
            raise CachedPlaybackError(
                "CACHED_PLAYBACK_NO_CACHE", "There is no completed cache to upload.")
        if revision is not None and not self._cache.compatible_with(revision):
            error = CachedPlaybackError(
                "CACHED_PLAYBACK_INCOMPATIBLE",
                "The completed cache does not match the current character snapshot.")
            self._invalidate_incompatible_cache(error)
            raise error
        self.enter()
        if self._uploaded_revision != self._cache.snapshot_revision:
            self._begin_upload()

    def detach(self, outcome):
        if self._closed:
            return self._issued_retention
        self._remove_capture_timer()
        self._remove_playback_poller()
        self._discard_upload()
        self._restore_capture_frame()
        if self._capture_cache is not None:
            self._capture_cache.delete()
            self._capture_cache = None
        kind = getattr(outcome, "kind", None)
        recapture_scene = bool(getattr(outcome, "recapture_scene", False))
        diagnostic = getattr(outcome, "diagnostic", None)
        retain = ((kind == "failed" and not recapture_scene)
                  or self._phase == "transport_failed")
        if retain and self._cache is not None and self._cache.completed:
            try:
                retain = self._cache.compatible_with(self._current_revision())
            except (PlaybackCacheError, AttributeError, TypeError, ValueError):
                retain = False
        else:
            retain = False
        if retain:
            self._issued_retention = CachedPlaybackRetention(self._cache)
        elif self._cache is not None:
            self._cache.delete()
        self._cache = None
        self._uploaded_revision = None
        self._streaming_session = None
        self._paused_streaming = False
        self._closed = True
        self._phase = "detached"
        self._publish(diagnostic=diagnostic)
        return self._issued_retention

    def discard(self):
        if not self._closed:
            self._remove_capture_timer()
            self._remove_playback_poller()
            self._discard_upload()
            self._restore_capture_frame()
            self._send_clear_best_effort()
            self._resume_streaming()
        for cache in (self._capture_cache, self._cache):
            if cache is not None:
                cache.delete()
        self._capture_cache = None
        self._cache = None
        if self._issued_retention is not None:
            self._issued_retention._discard()
        self._issued_retention = None
        self._uploaded_revision = None
        self._streaming_session = None
        self._paused_streaming = False
        self._closed = True
        self._phase = "detached"
        self._publish(current=0, total=0)
