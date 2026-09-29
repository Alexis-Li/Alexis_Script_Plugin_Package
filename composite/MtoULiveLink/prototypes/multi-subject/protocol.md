# Two-object realtime verification contract (Issue #54)

This is a disposable, editor-only experiment. It does not extend product protocol
v9, change a Binding Actor, turn independent rigs into Additional Parts, bake an
animation, or transfer an animation asset. Exactly two explicit pairs are
supported: `character` + `prop`, then `character` + `arms` on a newly negotiated
connection. Each pair contains a full Maya DAG root path and a distinct Unreal
Skeletal Mesh/target Actor; no heuristic target lookup or skeleton retargeting.

## Identity and evaluated time

The loopback TCP prototype uses newline-delimited UTF-8 JSON, version 1, with
a 1 MiB maximum line length. One `init` declares an fps and exactly two
subjects; each includes an `id`, `root`, parent-first `bones` with normalized
name and parent index, `curves` (Morph names), and `bind` local transforms.
The receiver accepts neither missing nor extra target IDs, duplicate IDs,
shared targets, ambiguous bone/curve names within one subject, nor a bone
whose parent/name does not match the explicitly selected target skeleton.
Identical names *across* subjects are valid and must remain isolated by ID.

Only after both targets accept `init` does the receiver return `ready` with a
session identity. Maya evaluates its referenced rigs at one scene time and
sends one `frame` containing a serial, source frame time, and complete poses
and curves for both IDs. The receiver admits a frame only for its current
session and complete current target set; the `applied` outcome echoes the
serial, source time and each subject's result. A late/partial/invalid frame
must not display a mixture of two source times. A `remove` for one ID removes
only that preview; closing the socket, switching the target skeleton, changing
the source scene, or ending the editor world terminates the old session. A
subsequent full-body/arms selection needs a new `init`, not a hot swap of an
old skeleton description. These are prototype messages, not v9 additions.

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
