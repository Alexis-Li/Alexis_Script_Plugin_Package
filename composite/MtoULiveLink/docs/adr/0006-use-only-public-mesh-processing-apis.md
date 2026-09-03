# Use only public mesh-processing APIs in Unreal 5.7.4

Model preview depends on the public `GeometryScriptingCore` and
`DynamicMesh` modules because it is an editor-only feature pinned and tested
against stock Unreal Editor 5.7.4. Direct use of public `FTransferBoneWeights`
is allowed when the higher-level wrapper omits required diagnostics. The
implementation does not include private headers, copy private engine code, or
patch the engine to make transient generation work. A host whose public APIs
cannot satisfy the transient asset boundary is unsupported.
