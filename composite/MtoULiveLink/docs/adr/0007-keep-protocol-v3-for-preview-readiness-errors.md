---
status: superseded by ADR-0009
---

# Keep protocol v3 for preview readiness errors

MtoU_LiveLink 0.4.0 keeps protocol v3 when adding `PREVIEW_NOT_READY` and
`PREVIEW_BUILD_FAILED`. These are new values of the existing extensible error
code string and do not change framing, message shape, ordering, or streaming
semantics. Both adapters add user-facing diagnostics and conformance coverage,
but an otherwise compatible Maya adapter does not need a new wire protocol to
receive a structurally valid Preview mode rejection.
