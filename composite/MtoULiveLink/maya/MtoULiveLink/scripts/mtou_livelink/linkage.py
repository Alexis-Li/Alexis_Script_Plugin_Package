"""Linkage: follow the Unreal camera and time, answer with complete pose groups.

The session owns the exclusive time-control state on the Maya side. It never
touches Maya directly: the host adapter passed to it applies the time and
camera, samples the objects and restores the captured state, so the state
machine and its scheduling stay testable without a host.

Scheduling follows the frozen v11 contract (protocol/v11.md): one target is
being applied and one newer target may wait; an advancing desired target never
cancels the work in flight, because Unreal shows that complete result and marks
it as lagging instead of losing the only sample that was going to arrive.

A group answers one target for the whole object set: the primary character and
the optional second object are sampled at the same Maya time, and while the
second object is being removed no group is sent, so Unreal never receives a
group of a set it is closing.
"""

import re
import threading
import time

from .config import (
    CHARACTER_OBJECT_ID,
    CONTROL_MAYA,
    CONTROL_UNREAL,
    LINKAGE_CONVERGENCE_SECONDS,
    LINKAGE_MODE,
    LINKAGE_MODE_CONFIRMATION_SECONDS,
    MAX_LINKAGE_GROUP_BYTES,
    MAX_PENDING_LINKAGE_TARGETS,
    REALTIME_MODE,
)
from .protocol import (
    exact_int,
    finite_float,
    group_wire_size,
    make_group_message,
    make_leave_linkage_message,
    make_mode_message,
    validate_reply,
)

# Unreal refuses a pose group a mode change overtook with MODE_CONFLICT and
# names the group's revision in ``details`` (MtoULinkageSession's staged-group
# gate). The code alone also covers real conflicts, so only these two forms are
# scheduling: a group that arrived during the transition, and a group of an
# older revision. A paired upgrade guarantees both hosts share these strings,
# as ``note_refused`` already relies on for EVAL_STALE.
_GROUP_DURING_TRANSITION = re.compile(r"^target mode revision -?\d+$")
_GROUP_REVISION = re.compile(r"^group mode revision (-?\d+), current (-?\d+)$")


STALE_REFERENCE_MESSAGE = "场景参考是为目标的另一摆放导出的，不能用于当前镜头联动"


def message_anchor(message):
    """The placement a linkage ``mode_state`` names (validated by the protocol)."""
    return message["anchor"]


def is_scheduling_conflict(details):
    """True when a MODE_CONFLICT answers a group that a mode change overtook."""
    text = str(details or "").strip()
    if _GROUP_DURING_TRANSITION.match(text):
        return True
    match = _GROUP_REVISION.match(text)
    return bool(match) and int(match.group(1)) < int(match.group(2))


class LinkageError(RuntimeError):
    """A refusal with a stable protocol code.

    ``stale`` marks a message that was correct when Unreal sent it and was
    overtaken by a newer mode transition in flight; the panel counts it
    instead of reporting it.
    """

    def __init__(self, code, message="", details="", stale=False):
        RuntimeError.__init__(self, message or code)
        self.code = code
        self.message = message or code
        self.details = details
        self.stale = stale


class LinkageEvent(object):
    """One state change the panel displays."""

    __slots__ = ("kind", "mode", "control", "mode_revision", "detail", "diagnostic")

    def __init__(self, kind, mode=None, control=None, mode_revision=None,
                 detail="", diagnostic=None):
        self.kind = kind
        self.mode = mode
        self.control = control
        self.mode_revision = mode_revision
        self.detail = detail
        self.diagnostic = diagnostic


class LinkageTarget(object):
    """One `camera_time` target, validated and ready to apply."""

    __slots__ = ("eval_id", "eval_serial", "transport_serial", "frame_serial",
                 "time", "camera", "gate", "cut", "received")

    def __init__(self, eval_id, eval_serial, transport_serial, frame_serial,
                 time_payload, camera, gate, cut, received):
        self.eval_id = eval_id
        self.eval_serial = eval_serial
        self.transport_serial = transport_serial
        self.frame_serial = frame_serial
        self.time = time_payload
        self.camera = camera
        self.gate = gate
        self.cut = cut
        self.received = received

    def display_frame(self):
        value = finite_float(self.time.get("display_frame"))
        return value

    def summary(self):
        return "{0} @ {1}".format(self.eval_id, self.display_frame())


def target_from_message(message, received=None):
    """Validate one `camera_time` message into a LinkageTarget.

    Raises LinkageError with the frozen rejection code so the caller can report
    the exact reason instead of guessing.
    """
    if not isinstance(message, dict):
        raise LinkageError("INVALID_MESSAGE", "camera_time must be an object")
    eval_id = message.get("eval_id")
    if not isinstance(eval_id, str) or not eval_id:
        raise LinkageError(
            "INVALID_MESSAGE", "camera_time requires a non-empty eval_id")
    serials = {}
    for name in ("eval_serial", "transport_serial", "frame_serial"):
        value = exact_int(message.get(name))
        if value is None or value < 0:
            raise LinkageError(
                "INVALID_MESSAGE",
                "camera_time field '{0}' must be a non-negative integer".format(name))
        serials[name] = value
    time_payload = message.get("time")
    if not isinstance(time_payload, dict):
        raise LinkageError("INVALID_MESSAGE", "camera_time requires a time object")
    display_frame = finite_float(time_payload.get("display_frame"))
    if display_frame is None:
        raise LinkageError(
            "INVALID_MESSAGE",
            "camera_time time.display_frame must be a finite number")
    for rate_name in ("display_rate", "tick_resolution"):
        rate = time_payload.get(rate_name)
        if not isinstance(rate, dict):
            raise LinkageError(
                "INVALID_MESSAGE",
                "camera_time time.{0} must be an object".format(rate_name))
        numerator = exact_int(rate.get("numerator"))
        denominator = exact_int(rate.get("denominator"))
        if numerator is None or numerator <= 0 or denominator is None or denominator <= 0:
            raise LinkageError(
                "INVALID_MESSAGE",
                "camera_time time.{0} must be positive integers".format(rate_name))
    playback_range = time_payload.get("playback_range")
    if not isinstance(playback_range, dict):
        raise LinkageError(
            "INVALID_MESSAGE", "camera_time requires time.playback_range")
    for name in ("start", "end"):
        if exact_int(playback_range.get(name)) is None:
            raise LinkageError(
                "INVALID_MESSAGE",
                "camera_time time.playback_range.{0} must be an integer".format(name))
    camera = message.get("camera")
    if not isinstance(camera, dict):
        raise LinkageError("INVALID_MESSAGE", "camera_time requires a camera object")
    gate = message.get("gate")
    if not isinstance(gate, dict):
        raise LinkageError("INVALID_MESSAGE", "camera_time requires a gate object")
    cut = message.get("cut")
    if cut is not None and not isinstance(cut, dict):
        raise LinkageError("INVALID_MESSAGE", "camera_time cut must be an object")
    return LinkageTarget(
        eval_id, serials["eval_serial"], serials["transport_serial"],
        serials["frame_serial"], time_payload, camera, gate, cut,
        time.monotonic() if received is None else received)


class LinkageStatus(object):
    """Immutable snapshot the panel renders."""

    __slots__ = ("mode", "control", "mode_revision", "set_revision",
                 "space_revision", "session_id", "target", "target_display_frame",
                 "issued", "applied_eval_id", "applied_display_frame",
                 "applied_sample_serial", "applied_age", "lag_seconds",
                 "pending", "applied_count", "refused_count", "converged",
                 "recovery", "detail", "object_ids", "removing", "apply_failure")

    def __init__(self, **values):
        for name in self.__slots__:
            setattr(self, name, values.get(name))


class LinkageSession(object):
    """One Maya-side linkage state over an established streaming session."""

    def __init__(self, host, session, on_event=None, object_id=CHARACTER_OBJECT_ID,
                 space_revision=1, set_revision=1, clock=time.monotonic,
                 mode_revision=0, object_ids=None):
        self._host = host
        self._session = session
        self._on_event = on_event
        self._object_id = object_id
        # Every object of the current set, primary first; a group carries all.
        self._object_ids = tuple(object_ids or (object_id,))
        # True while the second object is being removed: no group of the old
        # set is sent, and the waiting target is answered by the new set.
        self._removing = False
        self._space_revision = int(space_revision)
        self._set_revision = int(set_revision)
        self._clock = clock
        self._lock = threading.Lock()
        self._mode = REALTIME_MODE
        self._control = CONTROL_MAYA
        # Unreal keeps mode revisions for the whole connection, so a session
        # created for a later entry continues from the last revision seen.
        self._mode_revision = max(0, exact_int(mode_revision) or 0)
        self._session_id = None
        self._state = None
        self._pending = None
        self._issued = None
        self._sample_serial = 0
        self._issued_sample_serial = 0
        # The target Unreal currently asks for and the first sample Maya sent
        # for it: only a completed group from that sample on answers it. A
        # resample after a refusal keeps both; a new target or set starts over.
        self._request = None
        self._request_first_sample = 0
        # The latest complete application; a refused group never replaces it.
        self._applied = None
        self._applied_at = None
        # Unreal's reason for the latest group it could not complete, until a
        # later group completes.
        self._apply_failure = None
        self._applied_count = 0
        self._refused_count = 0
        self._recovery = []
        self._detail = ""
        self._closed = False
        # True once this session entered linkage; 结束预览 stays available
        # after 返回实时 because Unreal may still hold the original writers.
        self._entered = False
        # A transition Maya started waits for Unreal's matching mode_state;
        # without it Maya rolls back instead of following a peer that never
        # took over.
        self._awaiting_confirmation = False
        self._entered_at = None

    # ------------------------------------------------------------------ state
    @property
    def is_active(self):
        with self._lock:
            return self._mode == LINKAGE_MODE

    def mode(self):
        with self._lock:
            return self._mode

    def mode_revision(self):
        with self._lock:
            return self._mode_revision

    def space_revision(self):
        with self._lock:
            return self._space_revision

    def set_revision(self):
        with self._lock:
            return self._set_revision

    def object_ids(self):
        with self._lock:
            return self._object_ids

    def begin_set_removal(self):
        """Close the old set: hold groups until Unreal confirms the new one."""
        with self._lock:
            self._removing = True
            self._detail = "正在移除副对象，等待 Unreal 确认"

    def adopt_set(self, set_revision, object_ids):
        """Adopt the set Unreal confirmed; the answer in flight belongs to the old set.

        Unreal cancels the old set's work and issues the current target again,
        so the issued slot is released and the next target is answered by the
        new set.
        """
        revision = exact_int(set_revision)
        if revision is None or revision < 1:
            raise LinkageError("INVALID_MESSAGE", "set revision must be a positive integer")
        with self._lock:
            if revision < self._set_revision:
                return False
            self._set_revision = revision
            self._object_ids = tuple(object_ids)
            self._removing = False
            self._issued = None
            # The old set's results answer nothing the new set is asked for;
            # the latest complete application stays what Unreal last showed.
            self._request = None
            self._apply_failure = None
            self._detail = "对象集合已更新（{0} 个对象）".format(len(self._object_ids))
        self._emit(LinkageEvent(
            "set", mode=self.mode(), control=self._control,
            mode_revision=self.mode_revision(), detail=self._detail))
        return True

    def takeover_held(self):
        """True after 返回实时 while Unreal may still hold the original writers."""
        with self._lock:
            return self._entered and self._mode != LINKAGE_MODE

    def set_session_id(self, session_id):
        value = exact_int(session_id)
        if value is None or value < 1:
            raise LinkageError("INVALID_MESSAGE", "session id must be a positive integer")
        with self._lock:
            self._session_id = value

    def status(self):
        with self._lock:
            applied = self._applied
            applied_at = self._applied_at
            return LinkageStatus(
                mode=self._mode,
                control=self._control,
                mode_revision=self._mode_revision,
                set_revision=self._set_revision,
                space_revision=self._space_revision,
                session_id=self._session_id,
                target=self._issued.eval_id if self._issued else None,
                target_display_frame=(
                    self._issued.display_frame() if self._issued else None),
                issued=1 if self._issued else 0,
                applied_eval_id=applied.get("eval_id") if applied else None,
                applied_display_frame=(
                    applied.get("applied_display_frame") if applied else None),
                applied_sample_serial=(
                    applied.get("sample_serial") if applied else None),
                applied_age=(
                    None if applied_at is None else self._clock() - applied_at),
                lag_seconds=applied.get("lag_seconds") if applied else None,
                pending=1 if self._pending else 0,
                applied_count=self._applied_count,
                refused_count=self._refused_count,
                converged=self._converged_locked(),
                recovery=tuple(self._recovery),
                detail=self._detail,
                object_ids=self._object_ids,
                removing=self._removing,
                apply_failure=self._apply_failure,
            )

    def _converged_locked(self):
        if self._mode != LINKAGE_MODE:
            return False
        if self._issued is not None:
            return False
        if self._pending is not None:
            return False
        applied = self._applied
        if applied is None:
            return False
        return applied.get("applied") is True

    # ------------------------------------------------------------- transitions
    def enter(self, mode_revision, remote=False, anchor=None):
        """Take linkage control: capture Maya state and announce the new mode.

        ``remote`` marks a transition Unreal started: Maya's ``mode`` message is
        then the answer Unreal waits for (Unreal echoes it with ``mode_state``),
        and a refusal is announced as a failed transition so Unreal hands its
        borrowed writers back at once. ``anchor`` is the placement that
        ``mode_state`` named; a scene reference placed for another one refuses
        the entry before Maya changes anything.
        """
        revision = exact_int(mode_revision)
        if revision is None or revision < 1:
            raise LinkageError("INVALID_MESSAGE", "mode revision must be a positive integer")
        with self._lock:
            if self._closed:
                raise LinkageError("INVALID_MESSAGE", "linkage session is closed")
            if self._mode == LINKAGE_MODE:
                raise LinkageError(
                    "MODE_CONFLICT", "linkage is already active")
            if revision <= self._mode_revision:
                raise LinkageError(
                    "MODE_CONFLICT",
                    "mode revision {0} is not newer than {1}".format(
                        revision, self._mode_revision))
        problem = self._host.stale_reference(anchor) if anchor is not None else None
        if problem:
            if remote:
                self._announce_failed(revision, problem)
            self._emit(LinkageEvent(
                "mode", mode=REALTIME_MODE, control=CONTROL_MAYA,
                mode_revision=self._mode_revision, detail=problem,
                diagnostic=self._diagnostic("REFERENCE_STALE", problem)))
            raise LinkageError("REFERENCE_STALE", STALE_REFERENCE_MESSAGE, problem)
        try:
            state = self._host.capture_state()
        except Exception as exc:  # host refusal keeps the previous mode
            # The revision only advances once the host accepted the transition,
            # so a refused entry cannot make the next attempt look stale.
            self._record_recovery("capture", exc)
            if remote:
                self._announce_failed(revision, exc)
            self._emit(LinkageEvent(
                "mode", mode=REALTIME_MODE, control=CONTROL_MAYA,
                mode_revision=self._mode_revision, detail=str(exc),
                diagnostic=self._diagnostic("LINKAGE_FAILED", exc)))
            raise
        with self._lock:
            self._mode_revision = revision
            self._state = state
            self._mode = LINKAGE_MODE
            self._entered = True
            self._control = CONTROL_UNREAL
            self._pending = None
            self._issued = None
            self._applied = None
            self._applied_at = None
            self._apply_failure = None
            # Either way Unreal answers with mode_state linkage at this
            # revision; until then a same or older realtime statement is the
            # refusal or failure of this transition.
            self._awaiting_confirmation = True
            self._entered_at = self._clock()
            self._detail = "镜头联动已开始"
        self._session.submit_linkage(make_mode_message(
            self._session_id, revision, LINKAGE_MODE, CONTROL_UNREAL, "ready",
            "Maya follows the Unreal sequence time"))
        self._emit(LinkageEvent(
            "mode", mode=LINKAGE_MODE, control=CONTROL_UNREAL,
            mode_revision=revision, detail="镜头联动已开始"))
        return self._mode_revision

    def leave(self, target, mode_revision):
        """Return to realtime (keeping the Unreal takeover) or end the preview."""
        if target not in ("realtime", "end"):
            raise LinkageError(
                "INVALID_MESSAGE", "linkage exit target must be 'realtime' or 'end'")
        revision = exact_int(mode_revision)
        if revision is None or revision < 1:
            raise LinkageError("INVALID_MESSAGE", "mode revision must be a positive integer")
        with self._lock:
            if self._mode != LINKAGE_MODE:
                # 结束预览 after 返回实时 has nothing left to restore here, but
                # Unreal may still hold the original writers: the exit is still
                # the statement that hands them back, so it is accepted.
                if target != "end" or not self._entered:
                    raise LinkageError("LINKAGE_NOT_ACTIVE", "linkage is not active")
                self._mode_revision = max(revision, self._mode_revision)
                revision = self._mode_revision
                self._pending = None
                self._issued = None
                self._entered = False
                self._session.submit_linkage(make_leave_linkage_message(
                    self._session_id, revision, target, ""))
                self._emit(LinkageEvent(
                    "mode", mode=REALTIME_MODE, control=CONTROL_MAYA,
                    mode_revision=revision,
                    detail="已结束预览并恢复原状态"))
                return ()
            if revision <= self._mode_revision:
                raise LinkageError(
                    "MODE_CONFLICT",
                    "mode revision {0} is not newer than {1}".format(
                        revision, self._mode_revision))
            self._mode_revision = revision
            state = self._state
            self._pending = None
            self._issued = None
            self._awaiting_confirmation = False
        residual = self._restore(state)
        with self._lock:
            self._state = None
            self._mode = REALTIME_MODE
            self._control = CONTROL_MAYA
            if target == "end":
                self._entered = False
            self._detail = (
                "已返回实时预览（Maya 保持暂停）" if target == "realtime"
                else "已结束预览并恢复原状态")
        self._session.submit_linkage(make_leave_linkage_message(
            self._session_id, revision, target, "; ".join(residual)))
        self._emit(LinkageEvent(
            "mode", mode=REALTIME_MODE, control=CONTROL_MAYA,
            mode_revision=revision, detail=self._detail))
        return tuple(residual)

    def _restore(self, state):
        if state is None:
            return []
        try:
            residual = self._host.restore_state(state) or ()
        except Exception as exc:
            self._record_recovery("restore", exc)
            return ["{0}: {1}".format(type(exc).__name__, exc)]
        for entry in residual:
            self._record_recovery("residual", entry)
        return list(residual)

    # ---------------------------------------------------------------- inbound
    def offer_target(self, message):
        """Accept one `camera_time` target (worker thread); never blocks on Maya."""
        with self._lock:
            if self._closed:
                raise LinkageError("INVALID_MESSAGE", "linkage session is closed")
            if self._mode != LINKAGE_MODE:
                raise LinkageError("LINKAGE_NOT_ACTIVE", "linkage is not active")
            self._check_identity_locked(message)
        target = target_from_message(message, self._clock())
        with self._lock:
            pending = self._pending
            if pending is not None and pending.eval_serial > target.eval_serial:
                raise LinkageError(
                    "EVAL_STALE",
                    "target {0} is older than the waiting target {1}".format(
                        target.eval_id, pending.eval_id))
            if self._issued is not None and target.eval_serial < self._issued.eval_serial:
                raise LinkageError(
                    "EVAL_STALE",
                    "target {0} is older than the issued target {1}".format(
                        target.eval_id, self._issued.eval_id))
            self._pending = target
            replaced = pending is not None
        if not replaced:
            self._emit(LinkageEvent(
                "target", mode=LINKAGE_MODE, control=CONTROL_UNREAL,
                mode_revision=self._mode_revision, detail=target.summary()))
        return True

    def note_applied(self, message):
        """Record Unreal's actual component completion evidence."""
        validate_reply(message)
        with self._lock:
            self._check_identity_locked(message)
            sample_serial = exact_int(message.get("sample_serial"))
            if sample_serial is None:
                raise LinkageError(
                    "INVALID_MESSAGE", "applied requires an integer sample_serial")
            answers_issued = (self._issued is not None
                              and self._issued.eval_id == message.get("eval_id")
                              and sample_serial == self._issued_sample_serial)
            if message.get("applied"):
                self._applied = dict(message)
                self._applied_at = self._clock()
                self._apply_failure = None
                self._applied_count += 1
                # The issued work is finished once Unreal confirms the actual
                # components of that sample; a newer waiting target stays.
                if answers_issued:
                    self._issued = None
            else:
                # A refused group leaves the last complete application as it
                # was; the refusal is reported until a later group completes.
                self._apply_failure = str(message.get("reason") or "")
                if answers_issued:
                    # Unreal keeps its target issued, and nothing else would
                    # answer it: the target is sampled again unless a newer
                    # one already waits.
                    if self._pending is None:
                        self._pending = self._issued
                    self._issued = None
            self._detail = (
                "已应用 {0}".format(message.get("eval_id"))
                if message.get("applied")
                else "应用失败：{0}".format(message.get("reason") or ""))
        self._emit(LinkageEvent(
            "applied", mode=LINKAGE_MODE, control=CONTROL_UNREAL,
            mode_revision=self._mode_revision, detail=self._detail))
        return True

    def note_mode_state(self, message):
        """Record Unreal's authority statement and follow its transitions.

        Runs on Maya's main thread because a statement that ends linkage
        restores Maya's own state. Returns what Maya did:

        - ``"confirmed"``: Unreal confirmed the linkage this session holds;
        - ``"enter"``: Unreal started linkage; the caller prepares the session
          and calls :meth:`enter` with ``remote=True``;
        - ``"left"``: Unreal left linkage, refused or failed the transition and
          Maya restored itself (any residual is in the recovery records);
        - ``"refused"``: Unreal confirmed a placement the scene reference was
          not exported for; Maya failed the transition and restored itself;
        - ``"noted"``: nothing to do (an echo, or a stale statement).
        """
        validate_reply(message)
        revision = exact_int(message.get("mode_revision"))
        if revision is None:
            raise LinkageError(
                "INVALID_MESSAGE", "mode_state requires an integer mode_revision")
        mode = message.get("mode")
        detail = str(message.get("detail") or "")
        with self._lock:
            if self._session_id is not None \
                    and exact_int(message.get("session_id")) != self._session_id:
                raise LinkageError(
                    "INVALID_MESSAGE",
                    "mode_state session {0!r} does not match {1!r}".format(
                        message.get("session_id"), self._session_id))
            active = self._mode == LINKAGE_MODE
            current = self._mode_revision
            confirming = False
            if mode == LINKAGE_MODE:
                if active and revision == current:
                    confirming = self._awaiting_confirmation
                    self._awaiting_confirmation = False
                    self._detail = detail or self._detail
                    outcome = "confirmed"
                elif not active and revision > current and not self._closed:
                    outcome = "enter"
                else:
                    # An echo of a linkage this session already left.
                    outcome = "noted"
            elif active:
                if revision > current:
                    # Unreal's own 返回实时, 结束预览 or lost takeover.
                    outcome = "left"
                elif self._awaiting_confirmation:
                    # Unreal refused (older revision) or failed (same revision)
                    # the transition this session has not seen confirmed.
                    outcome = "left"
                else:
                    # A statement older than the confirmed linkage: it can no
                    # longer describe the current mode.
                    outcome = "noted"
            else:
                if revision > current:
                    self._mode_revision = revision
                self._detail = detail or self._detail
                outcome = "noted"
        if outcome == "left":
            return self._follow_remote_exit(revision, current, detail)
        if confirming:
            problem = self._host.stale_reference(message_anchor(message))
            if problem:
                self._fail_entered(revision, problem, "REFERENCE_STALE")
                return "refused"
        if outcome != "enter":
            self._emit(LinkageEvent(
                "mode", mode=self.mode(), control=self._control,
                mode_revision=self.mode_revision(), detail=self._detail))
        return outcome

    def _follow_remote_exit(self, revision, current, detail):
        """Unreal no longer holds linkage: restore Maya and stay paused.

        A newer revision is Unreal's own 返回实时 or 结束预览; the same revision
        is a transition Unreal failed; an older one refused Maya's entry. In
        the last two cases Unreal holds no writers, so 结束预览 has nothing
        left to hand back.
        """
        with self._lock:
            state = self._state
            self._state = None
            self._pending = None
            self._issued = None
            self._awaiting_confirmation = False
            self._mode = REALTIME_MODE
            self._control = CONTROL_MAYA
            self._mode_revision = revision
            if revision <= current:
                self._entered = False
        residual = self._restore(state)
        with self._lock:
            if revision > current:
                summary = "Unreal 已结束镜头联动，Maya 已恢复并保持暂停"
            else:
                summary = "Unreal 未接管镜头联动，Maya 已恢复原状态"
            self._detail = summary + ("：" + detail if detail else "")
            if residual:
                self._detail += "（恢复残留：{0}）".format("；".join(residual))
        self._emit(LinkageEvent(
            "mode", mode=REALTIME_MODE, control=CONTROL_MAYA,
            mode_revision=revision, detail=self._detail,
            diagnostic=(self._diagnostic("RECOVERY_REFUSED", "; ".join(residual))
                        if residual else None)))
        return "left"

    def confirmation_overdue(self):
        """True when Unreal never confirmed the linkage Maya started."""
        with self._lock:
            if not self._awaiting_confirmation or self._entered_at is None:
                return False
            return self._clock() - self._entered_at > LINKAGE_MODE_CONFIRMATION_SECONDS

    def expire_unconfirmed(self):
        """Roll back a Maya-started linkage Unreal never confirmed.

        Maya restores itself and announces the failed transition, so an Unreal
        that took over late hands its writers back instead of driving a target
        nobody answers. Returns the residual, or None when nothing waited.
        """
        with self._lock:
            if self._mode != LINKAGE_MODE or not self._awaiting_confirmation:
                return None
            revision = self._mode_revision
        reason = "Unreal 未在 {0:g} 秒内确认镜头联动".format(
            LINKAGE_MODE_CONFIRMATION_SECONDS)
        return self._fail_entered(revision, reason, "MODE_CONFLICT")

    def _fail_entered(self, revision, reason, code):
        """Roll back an entered linkage Maya cannot keep and tell Unreal.

        Maya restores itself and announces the failed transition, so Unreal
        hands its writers back instead of driving a target nobody answers.
        Returns the residual.
        """
        with self._lock:
            state = self._state
            self._state = None
            self._pending = None
            self._issued = None
            self._awaiting_confirmation = False
            self._entered = False
            self._mode = REALTIME_MODE
            self._control = CONTROL_MAYA
        residual = self._restore(state)
        self._announce_failed(revision, reason)
        with self._lock:
            self._detail = reason + "，Maya 已恢复原状态"
            if residual:
                self._detail += "（恢复残留：{0}）".format("；".join(residual))
        self._emit(LinkageEvent(
            "failed", mode=REALTIME_MODE, control=CONTROL_MAYA,
            mode_revision=revision, detail=self._detail,
            diagnostic=self._diagnostic(code, reason)))
        return tuple(residual)

    def _announce_failed(self, revision, error):
        """Tell Unreal that transition ``revision`` failed on the Maya side."""
        if self._session_id is None or revision is None or revision < 1:
            return
        try:
            self._session.submit_linkage(make_mode_message(
                self._session_id, revision, REALTIME_MODE, CONTROL_MAYA,
                "failed", str(error)))
        except Exception as exc:  # the transport may already be gone
            self._record_recovery("announce", exc)

    def note_refused(self, detail=""):
        """Count one answer Unreal refused because its target was cancelled.

        When the refusal names the issued sample, that sample will never be
        applied, so the issued slot is free for the newer target Unreal sent.
        """
        with self._lock:
            self._refused_count += 1
            if self._issued is not None and detail \
                    and "group eval {0},".format(self._issued.eval_id) in detail:
                self._issued = None
            if detail:
                self._detail = "已丢弃过期结果：{0}".format(detail)

    def note_reference_offer(self, message):
        """Accept the handoff descriptor the panel will offer to the user."""
        validate_reply(message)
        with self._lock:
            self._check_identity_locked(message)
        self._emit(LinkageEvent(
            "reference", mode=self._mode, control=self._control,
            mode_revision=self._mode_revision,
            detail=str(message.get("transfer_id") or "")))
        return True

    def _check_identity_locked(self, message):
        session_id = exact_int(message.get("session_id"))
        if self._session_id is None:
            raise LinkageError(
                "INVALID_MESSAGE", "no session identity is established yet")
        if session_id != self._session_id:
            raise LinkageError(
                "INVALID_MESSAGE",
                "message session {0!r} does not match {1!r}".format(
                    session_id, self._session_id))
        revision = exact_int(message.get("mode_revision"))
        if revision is not None and revision != self._mode_revision:
            # Unreal's messages of the previous mode are still on the wire
            # when Maya completes its own transition.
            raise LinkageError(
                "MODE_CONFLICT",
                "message mode revision {0} is not the current {1}".format(
                    revision, self._mode_revision),
                stale=revision < self._mode_revision)
        set_revision = exact_int(message.get("set_revision"))
        if set_revision is not None and set_revision != self._set_revision:
            # A target of the set a removal just closed is still on the wire
            # when Maya adopts the new set; it is counted, not reported.
            raise LinkageError(
                "SET_REVISION_STALE",
                "message set revision {0} is not the current {1}".format(
                    set_revision, self._set_revision),
                stale=set_revision < self._set_revision)
        space_revision = exact_int(message.get("space_revision"))
        if space_revision is not None and space_revision != self._space_revision:
            raise LinkageError(
                "SPACE_REVISION_STALE",
                "message space revision {0} is not the current {1}".format(
                    space_revision, self._space_revision))

    # -------------------------------------------------------------------- pump
    def pump(self):
        """Apply the waiting target on the Maya main thread and answer it.

        Returns the number of groups submitted. A group is always sent for the
        target that was actually applied, even when a newer target is already
        waiting: Unreal shows that complete result and marks it as lagging.
        """
        with self._lock:
            if self._closed or self._mode != LINKAGE_MODE or self._removing:
                return 0
            target = self._pending
            self._pending = None
            object_ids = list(self._object_ids)
            set_revision = self._set_revision
        if target is None:
            return 0
        try:
            applied_time, readback = self._host.apply_target(
                target.time, target.camera, target.gate)
        except LinkageError:
            raise
        except Exception as exc:
            self._record_recovery("apply", exc)
            self._emit(LinkageEvent(
                "failed", mode=LINKAGE_MODE, control=CONTROL_UNREAL,
                mode_revision=self._mode_revision, detail=str(exc),
                diagnostic=self._diagnostic("LINKAGE_APPLY_FAILED", exc)))
            return 0
        objects = self._host.sample_group(object_ids)
        with self._lock:
            if self._set_revision != set_revision or self._removing:
                # The set changed while Maya sampled: the target is answered
                # again by the current set instead of by the closed one.
                if self._pending is None:
                    self._pending = target
                return 0
            self._sample_serial += 1
            sample_serial = self._sample_serial
            self._issued = target
            self._issued_sample_serial = sample_serial
            if target is not self._request:
                self._request = target
                self._request_first_sample = sample_serial
            self._detail = "已采样 {0}".format(target.eval_id)
        message = make_group_message(
            self._session_id, set_revision, self._mode_revision,
            self._space_revision, target.eval_id, target.transport_serial,
            sample_serial, objects, readback, applied_time)
        size = group_wire_size(message)
        if size > MAX_LINKAGE_GROUP_BYTES:
            with self._lock:
                self._issued = None
                self._detail = "整组姿势超过冻结的消息上限"
            self._emit(LinkageEvent(
                "failed", mode=LINKAGE_MODE, control=CONTROL_UNREAL,
                mode_revision=self._mode_revision,
                detail="group of {0} bytes exceeds {1}".format(
                    size, MAX_LINKAGE_GROUP_BYTES),
                diagnostic=self._diagnostic(
                    "RESOURCE_LIMIT",
                    "linkage group of {0} bytes exceeds the frozen {1} byte budget".format(
                        size, MAX_LINKAGE_GROUP_BYTES))))
            raise LinkageError(
                "RESOURCE_LIMIT",
                "linkage group exceeds the frozen message budget",
                details="group {0} bytes, budget {1} bytes".format(
                    size, MAX_LINKAGE_GROUP_BYTES))
        self._session.submit_linkage(message)
        self._emit(LinkageEvent(
            "group", mode=LINKAGE_MODE, control=CONTROL_UNREAL,
            mode_revision=self._mode_revision,
            detail="{0} sample {1}".format(target.eval_id, sample_serial)))
        return 1

    def settled(self):
        """True when nothing is in flight and the last result was applied."""
        with self._lock:
            return self._converged_locked()

    def wait_seconds(self):
        """Seconds the current target has been waiting for an applied result.

        A target Unreal refused waits to be sampled again and keeps the time it
        was received, so its retries count toward convergence. Only a group
        completed for this request answers it: an earlier success of the same
        evaluation, for example by the set before a removal, does not.
        """
        with self._lock:
            target = self._issued
            if target is None and self._apply_failure is not None:
                target = self._pending
            if target is None:
                return 0.0
            applied = self._applied
            if applied is not None and target is self._request \
                    and applied.get("eval_id") == target.eval_id \
                    and (exact_int(applied.get("sample_serial")) or 0) \
                    >= self._request_first_sample:
                return 0.0
            return self._clock() - target.received

    def convergence_overdue(self):
        return self.wait_seconds() > LINKAGE_CONVERGENCE_SECONDS

    # ------------------------------------------------------------------ close
    def close(self):
        """Release linkage resources; safe to call twice."""
        with self._lock:
            if self._closed:
                return ()
            self._closed = True
            state = self._state
            self._state = None
            self._pending = None
            self._issued = None
            self._mode = REALTIME_MODE
            self._control = CONTROL_MAYA
        residual = self._restore(state)
        self._detail = "已断开连接并恢复预览接管"
        return tuple(residual)

    # ----------------------------------------------------------------- detail
    def _record_recovery(self, stage, error):
        with self._lock:
            self._recovery.append("{0}: {1}".format(stage, error))

    def recovery_records(self):
        with self._lock:
            return tuple(self._recovery)

    def _diagnostic(self, code, error):
        return {"code": code, "message": str(error), "details": str(error)}

    def _emit(self, event):
        listener = self._on_event
        if listener is None:
            return
        try:
            listener(event)
        except Exception:
            pass

    def pending_count(self):
        with self._lock:
            return (1 if self._pending else 0) + (1 if self._issued else 0)

    def max_pending(self):
        return MAX_PENDING_LINKAGE_TARGETS
