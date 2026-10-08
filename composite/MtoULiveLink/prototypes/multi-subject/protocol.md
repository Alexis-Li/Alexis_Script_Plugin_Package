# Two-object realtime verification contract (Issue #54)

This is a disposable, editor-only experiment. It does not extend product protocol
v9, change a Binding Actor, turn independent rigs into Additional Parts, bake an
animation, or transfer an animation asset. Exactly two explicit pairs are
supported: `character` + `prop`, then `character` + `arms` on a newly negotiated
connection. Each pair contains a full Maya DAG root path and a distinct Unreal
Skeletal Mesh/target Actor; no heuristic target lookup or skeleton retargeting.

## Identity and evaluated time

Contract summary (version 1):

* exactly two subjects, each with an explicit `id`, Maya `root` path, parent-first
  `bones`, `curves` (Morph names) and one `bind` row per bone;
* the receiver accepts neither missing nor extra target IDs, duplicate IDs,
  shared targets, ambiguous bone/curve names within one subject, nor a declared
  bone the selected target does not have. Identical names *across* subjects are
  valid and stay isolated by ID;
* `ready` returns a positive session id. Every later `frame` and `remove`
  repeats that session; a message naming another session is refused with
  `session_mismatch` and changes nothing;
* one `frame` carries a strictly increasing `serial` (the evaluation identity)
  and one Maya source `time`. The serial must increase inside the session; the
  time may move backwards (reverse scrub) or repeat (a re-edit of an already
  sent frame), and each applied frame records `first`/`forward`/`backward`/`hold`
  so the direction stays visible;
* the receiver admits a frame only for its current session and complete current
  target set; the `applied` outcome echoes the session, serial, source time and
  each subject's result. A late/partial/invalid frame must not display a mixture
  of two source times;
* `remove` for one ID removes only that preview; closing the socket, switching
  the target skeleton, changing the source scene, or ending the editor world
  terminates the old session. A subsequent full-body/arms selection needs a new
  `init`, not a hot swap of an old skeleton description. These are prototype
  messages, not v9 additions.

## Necessary bones, not whole-table equality

A declaration must name every bone it needs and nothing it cannot drive: each
declared bone has to exist in the target, carry the declared parent, and have
only declared bones above it, so no driven bone ever hangs under a bone nobody
drives. Bones the target owns but the declaration does not name stay at their
reference pose and are reported per subject as `undriven_bones`, so the evidence
never implies that the whole target skeleton was matched. This is the negotiation
shape #55 accepted for a real rig: a production C01 skeleton legitimately has
branches a simplified preview target does not drive. Declaring a bone the target
lacks, or dropping a bone a declared child needs, is still `skeleton_mismatch`.

Maya captures the *evaluated* DAG joint matrices and BlendShape values, not
controller or constraint wiring; its saved reference assets remain untouched.
Frame time uses Maya source-frame units (fractional values are valid), not
elapsed seconds or an Unreal wall clock. Maya is the time authority for live
poses. If a Sequencer time-follow mode is proposed later, there must instead
be one explicit time authority and a frame/session identity join; simultaneous
Maya playback and Sequencer-controlled time are not a valid test.

## Space and preview ownership

The prototype uses the product Maya→Unreal mapping: Maya Y-up centimeters to
Unreal Z-up centimeters, `(x,y,z) -> (x,z,y)` with the corresponding quaternion
conversion. The first root transform in each subject is the evaluated
**world** matrix of that Maya DAG root, including any reference parent motion;
child transforms are relative to their captured parent. A target Actor is a
single UE placement anchor. Apply the root pose once in that Actor's local
space, with the same anchor and axis mapping for character and prop. If Maya
already includes movement from the character's reference hierarchy, attaching
the prop target to a *moving* UE socket and then adding that movement again is
invalid: use a world-anchored target, or explicitly remove the shared parent
transform before using a socket. Do not silently choose one by name. Sequencer
placement should likewise be an anchor, not a second root-motion writer.

Explicit preview takeover means pausing/disabling an existing animation or
Sequencer track's write to each participating target, saving its former driver
and component state, and restoring them on exit and error. Sequencer can
continue driving unrelated tracks and its own time; the prototype never edits
an existing sequence or imported skeletal/animation asset. This is *not* a
claim that the existing product can share one target with Sequencer.

Drive ownership is reported, not inferred: each target names the driver it had
before the takeover (animation blueprint, single-node asset, or "none"), the
writers the preview muted with whether each belongs to a saved asset, and how
the target left (`own_driver_restored`, `reference_pose` or `preview_active`).
`MtoUMultiSubject.Ownership` prints it and the evidence file records it, because
`SetLocalEvalDisabled` is an editor-local mute that no Sequencer UI shows.

Restoring a component's previous animation state is not enough when it had no
driver: nothing would write the pose back, so the component would keep the
preview's last pose. On exit such a target is put back into its reference pose
explicitly, and the evidence records `restored_to_reference_pose`.

## Cache-only design; no cache implementation

A future cache would freeze a single session/subject set, common source-frame
range and fps, including both endpoints. Each subject must have exactly the
same sampled time lattice (or the upload fails with named subject, incompatible
range/fps, or missing frame); do not interpolate a mismatched rig silently.
The entire set becomes Ready atomically only after all uploads pass bounds and
identity checks. A failed/partially uploaded subject invalidates the *set*;
a disconnected/renegotiated subject clears old ownership; neither stale cached
poses nor an old session's playback commands may drive the new pair. UE alone
would play/seek/pause the Ready set on one clock with shared applied-frame
acknowledgement; start/seek/loop completion semantics must be revised together
with ADR 0013. No multi-object cache is sent by this prototype.

## Product cutover inventory (not performed here)

- Maya controller, `_CharacterScene` and `_StreamingSession` currently hold
  one root/snapshot, one sender and one `init`/`frame`; input revisions and
  lifecycle callbacks would need *per-object* ownership plus a set-wide
  common-frame transaction.
- UE `FMtoULiveLinkSource::HandleInitOnGameThread` requires one placed Binding
  Actor and one fixed `MtoU_Character` SubjectKey; it keeps one negotiated
  bone/curve index set, one pending latest frame and one cache owner. A product
  migration must use an explicit ID→root→Binding/target registry, independent
  subject keys, per-object cleanup and a bounded aggregate frame admission.
- Binding Actor `Destroyed`, `SetBinding`, preview refresh and the source
  publish gate currently terminate the one streaming session. They need
  target-scoped ending without leaving another subject or its Live Link/Anim
  instance attached. Static-data, Morph retarget and duplicate-name mappings
  must never be shared across object IDs.
- Product v9 `init`, `frame`, replies, conformance corpus, and ADR 0013 are
  single-character; simultaneous snapshot revisions, session-bound frame/time
  identity, ready/failure across all subjects and atomic cache ownership
  require a *new* versioned contract and both adapters' tests, not a local
  exception in the existing parser. UI must allow explicit target pairing and
  an exclusive preview/Sequencer drive state, without an implicit auto-retarget.

Minimal next implementation order: stabilize an explicit two-pair input and
strict negotiation; prove session/frame identity and per-pair teardown in both
hosts; add a reversible takeover control; only then specify a versioned set
cache. This note authorizes none of those product changes.
