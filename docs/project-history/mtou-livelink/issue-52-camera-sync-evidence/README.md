# Issue #52 acceptance evidence

The [acceptance record](../issue-52-camera-sync-acceptance.md) owns the final
scope and limits. Independent review-02 accepted implementation
`25336faee2e03076e7e22e829a9793509b1cfbca` on 2026-10-08 using Maya 2024 and
stock UE 5.7.4. These are preserved observations, not new host verification.

| File | Why retained |
| --- | --- |
| [maya-host.json](maya-host.json) | Final 145-check Maya/Arnold result, including numeric projection and safety evidence. |
| [review-reproduction.json](review-reproduction.json) | Independent R-007–R-009 failure-input re-verification results. |
| [ue-automation.json](ue-automation.json) | Final UE Automation scope and 11-check pass/skip/warning status. |
| [maya-peer.json](maya-peer.json) | Maya-side real-peer observations supporting cross-host convergence. |
| [ue-peer.json](ue-peer.json) | UE-side real-peer convergence and projection results. |
| [editor-loop-maya.json](editor-loop-maya.json) | Maya-side application and restoration across editor-loop connections. |
| [editor-loop-ue.json](editor-loop-ue.json) | UE-side 13-step editor-loop, fractional-time and recovery results. |
| [review-02-reproduction.py](review-02-reproduction.py) | Exact independent failure inputs associated with the retained review result; preserves reviewer provenance separately from implementation-owned safety tests. |

The seven JSON files are byte-for-byte copies of review-02 results at
[`12d3acf`](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/tree/12d3acfd4fc64d737cd1eea3edc03cf4e1031059/docs/project-history/mtou-livelink/issue-52-camera-sync-evidence/issue52-review02-20261008).
Historical paths inside those results identify original runs, not current file
locations. Expected stale/replay refusals are negative cases, not failed acceptance.

Use maintained [prototype instructions](../../../../composite/MtoULiveLink/prototypes/camera-sync/README.md)
for general checks. The supplementary script runs with Maya 2024 `mayapy` and
accepts an optional output JSON path; its default uses scratch beside the repo.
The preserved script was syntax-checked during archival, not rerun as a new review.
Render images are not retained; numeric projection observations remain in JSON.

The [source-comment archive](../issue-52-comment-archive.json) identifies original
review reports by ID and links to earlier snapshots. Superseded development runs,
old failure inputs and the former disposal manifest are recoverable from
[Git history](https://github.com/Alexis-Li/Alexis_Script_Plugin_Package/tree/12d3acfd4fc64d737cd1eea3edc03cf4e1031059/docs/project-history/mtou-livelink/issue-52-camera-sync-evidence).
