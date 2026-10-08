# Issue #52 evidence archive

The [acceptance record](../issue-52-camera-sync-acceptance.md) owns the current
conclusion: review-02 accepted `25336faee2e03076e7e22e829a9793509b1cfbca`.
This 2026-10-08 closeout preserved existing evidence; it ran no new host tests.
Earlier/failed runs are historical and do not override final acceptance.

| Source directory | Evidence role |
| --- | --- |
| `mtou-issue52-camera-sync` | Initial prototype, transport probes, first host/peer results. |
| `mtou-camera-sync-2026-09-29`, `issue52-evaluation-review-20260929`, `issue52-content-identity-20260929` | Timing, convergence and camera-content identity development. |
| `issue52-natural-20260930` | Natural editor-loop/recovery development, including failing intermediate and final runs. |
| `issue52-review-20261008` | Independent review-01: normal checks and R-007–R-009 reproduced at `91413946466d8f0337104f4bf087b7a4962d3428`. |
| `issue52-fix-20261008` | Development self-tests; these alone do not grant acceptance. |
| `issue52-review02-20261008` | Final independent acceptance: 145 Maya/Arnold checks, 11 UE checks with real peers and supplementary failure-input verification. |

[manifest.json](manifest.json) lists original scratch-relative paths, sizes,
SHA-256 hashes, retained destinations and disposal reasons. Identical normalized
results share one retained file; use the manifest when a source filename has no
separate copy. Machine-specific path prefixes/host names are normalized; numeric
observations, outcomes and expected refusal records are preserved. Original and
archived hashes identify transformations.

[review-01-reproduction.py](review-01-reproduction.py) and
[review-02-reproduction.py](review-02-reproduction.py) preserve independent
failure inputs and respective assertions. Repository lookup derives from this
directory; an optional first argument specifies result JSON. Default output
uses the scratch directory beside the repository. Run with Maya 2024 `mayapy`;
review-01 is for its historical baseline, review-02 for the accepted one.
The archive adaptations were syntax-checked, not executed as a new host review.
General reproduction uses maintained prototype instructions and tests.

Comment originals live in the [unified archive](../issue-52-comment-archive.json).
Reports are historical observations, not guarantees for untested hosts. Raw
logs, render images, preferences, generated fixtures and publication scaffolding
are omitted after retaining meaningful numeric/report content. Render coordinates
and errors remain in host results; maintained tests regenerate images.
