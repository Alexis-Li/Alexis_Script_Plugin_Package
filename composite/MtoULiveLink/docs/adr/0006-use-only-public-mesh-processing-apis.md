# Use only public mesh-processing APIs in Unreal 5.7.4

Preview Mesh V1 may depend on the public `GeometryScriptingCore` and
`DynamicMesh` modules because it is an editor-only feature pinned and tested
against stock Unreal Editor 5.7.4. Direct use of public `FTransferBoneWeights`
is allowed when the higher-level wrapper omits required diagnostics. The
implementation will not include private headers, copy private engine code, or
patch the engine to make transient generation work. If public APIs cannot
satisfy the transient asset boundary, the feasibility Spike fails and V1
pauses.
