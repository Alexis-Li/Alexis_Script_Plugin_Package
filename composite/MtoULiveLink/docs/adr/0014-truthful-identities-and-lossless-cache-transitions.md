# Give cached playback truthful identities and lossless transitions

Review of the protocol-v5 upload-then-play implementation (commits `00fae5a`
and `2892fb0`) found that completion could still be claimed from a loop
counter, that late replies from older attempts could complete newer ones, and
that returning to Real-time Preview could strand queued controls when the
sender switched back to Latest mode. ADR 0013's decision — Maya uploads a
complete temporary cache, Unreal validates it atomically, and Unreal alone
drives local playback — stays frozen; this decision hardens it.

## Identity and authoritative revision (protocol v6)

Protocol advances to v6 because v5 is frozen. `init` now carries the character
snapshot revision established at connection negotiation; the `ready` outcome
echoes it, and every cache upload declares it for validation against the
negotiated value, so client messages can never invent compatibility. Each
upload carries a monotonically increasing `upload_id`; each play attempt a
monotonically increasing `play_id`. `cache_ready`, bounded `cache_progress`,
`cache_complete` (applied count plus elapsed duration), `cache_stopped`, and
`cache_cleared` echo these identities, and Maya ignores any well-formed
outcome whose identity does not match the current operation while continuing
to wait. Wrong counts or revisions in Ready or completion are stable
invalid-outcome failures, never success.

## Truthful application evidence

Unreal applies at most one cached pose per source-frame position per game-thread
update. Before publishing the next pose it checks the monotonic clock against
that position's valid window; a missed window stops playback with
`CACHED_PLAYBACK_PERFORMANCE` before any overdue catch-up burst can collapse
several poses into one visible tick. The publication callback reports
acceptance, and applied evidence advances only on acceptance. Successful
completion requires every expected pose accepted exactly once in order, the
final pose held, and elapsed duration within the captured-rate bound.

## Bounded resources

The framing boundary meters actual encoded bytes of every cached frame with
overflow-safe accumulation; exceeding the client-declared size or the frozen
64 MiB limit rejects the whole upload with `CACHE_PAYLOAD_TOO_LARGE` while
keeping the session open. `cache_begin` also preflights the predicted parsed
transient allocation from negotiated transform/curve counts against a fixed
256 MiB budget before any allocation.

## Lossless mode transitions

The sender migrates queued ordered packets into a carry-over queue when it
returns to Latest mode instead of discarding them, so `cache_clear` keeps its
place ahead of the first resumed live pose and Real-time Preview recovers
without stranding work.
