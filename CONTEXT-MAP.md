# Context Map

## Contexts

- [StoUTextureSync](https://github.com/Alexis-Li/StoUTextureSync/blob/main/CONTEXT.md) — publishes Painter outputs for existing UE Texture assets; independently versioned.
- [MtoU Live Link](https://github.com/Alexis-Li/MtoULiveLink/blob/main/CONTEXT.md) — negotiates and streams a Maya character into an Unreal skeletal mesh; independently versioned.

## Relationships

- **Painter → StoUTextureSync → Unreal**: Painter outputs become local source
  images associated with existing UE Textures. The independent product owns
  this contract.
- **Maya → MtoU Live Link → Unreal**: Maya supplies evaluated character animation; Unreal supplies the target skeletal mesh and receives the negotiated Live Link stream. The independent product owns this contract.
