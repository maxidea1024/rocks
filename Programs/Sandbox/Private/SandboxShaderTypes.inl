// Sandbox 셰이더 타입 목록이다(SandboxShaders.h·SandboxShaders.cpp가 매크로를 정해 두 번 펼친다).
// SANDBOX_<단계>(변수, 원문 파일, 엔트리), SANDBOX_<단계>_IF(…, 거르기)는 그 형식에서 컴파일하지
// 않는 타입이다(형식 상한 — GLES31_PC의 VS·PS storage block 0, render target 4).

SANDBOX_VERTEX(FullScreenVS, "FullScreen.rsf", "VSMain")

SANDBOX_VERTEX(SampleVS, "Sample.rsf", "VSMain")
SANDBOX_PIXEL(SamplePS, "Sample.rsf", "PSMain")

SANDBOX_VERTEX(CullingFacingVS, "Culling.rsf", "VSMain")
SANDBOX_PIXEL(CullingFacingPS, "Culling.rsf", "PSMain")

SANDBOX_VERTEX(MsaaCoverVS, "Msaa.rsf", "CoverVS")
SANDBOX_PIXEL(MsaaCoverPS, "Msaa.rsf", "CoverPS")
SANDBOX_PIXEL(MsaaLoadPS, "Msaa.rsf", "LoadPS")

SANDBOX_VERTEX(BufferBlackVS, "Buffer.rsf", "BlackVS")
SANDBOX_PIXEL(BufferColorPS, "Buffer.rsf", "ColorPS")
SANDBOX_PIXEL(BufferTypedPS, "Buffer.rsf", "TypedPS")
SANDBOX_VERTEX(BufferTypedVS, "Buffer.rsf", "TypedVS")
SANDBOX_PIXEL_IF(BufferStructuredPS, "Buffer.rsf", "StructuredPS", NoGraphicsStorageBlocks)
SANDBOX_VERTEX_IF(BufferRawVS, "Buffer.rsf", "RawVS", NoGraphicsStorageBlocks)
SANDBOX_PIXEL(BufferOutsidePS, "Buffer.rsf", "OutsidePS")

SANDBOX_VERTEX(CommandFillVS, "Command.rsf", "FillVS")
SANDBOX_PIXEL(CommandFillPS, "Command.rsf", "FillPS")
SANDBOX_COMPUTE(CommandWriterCS, "Command.rsf", "WriterCS")
SANDBOX_VERTEX(CommandReaderVS, "Command.rsf", "ReaderVS")
SANDBOX_PIXEL_IF(CommandReaderPS, "Command.rsf", "ReaderPS", NoGraphicsStorageBlocks)
SANDBOX_COMPUTE(CommandArgumentsCS, "Command.rsf", "ArgumentsCS")
SANDBOX_COMPUTE(CommandMarkerCS, "Command.rsf", "MarkerCS")
SANDBOX_COMPUTE(CommandOutsideCS, "Command.rsf", "OutsideCS")

SANDBOX_COMPUTE(GraphSequenceCS, "GraphCompute.rsf", "SequenceCS")
SANDBOX_COMPUTE(GraphArgumentsCS, "GraphCompute.rsf", "ArgumentsCS")
SANDBOX_COMPUTE(GraphPatternCS, "GraphCompute.rsf", "PatternCS")
SANDBOX_PIXEL_IF(CommandMrtPS, "Command.rsf", "MrtPS", EightRenderTargets)

SANDBOX_PIXEL(TintPS, "Tint.rsf", "TintPS")

SANDBOX_PIXEL(FormatFloatColorPS, "Format.rsf", "FloatColorPS")
SANDBOX_PIXEL(FormatIntColorPS, "Format.rsf", "IntColorPS")
SANDBOX_PIXEL(FormatUintColorPS, "Format.rsf", "UintColorPS")
SANDBOX_PIXEL(FormatLoadFloatPS, "Format.rsf", "LoadFloatPS")
SANDBOX_PIXEL(FormatLoadStencilPS, "Format.rsf", "LoadStencilPS")

SANDBOX_PIXEL(SliceWhitePS, "SliceAttachment.rsf", "WhitePS")
SANDBOX_PIXEL(SliceHalfPS, "SliceAttachment.rsf", "HalfPS")
SANDBOX_PIXEL(SliceMsaaLayerPS, "SliceAttachment.rsf", "MsaaLayerPS")

SANDBOX_COMPUTE(StorageColumnsCS, "StorageTarget.rsf", "ColumnsCS")
SANDBOX_COMPUTE(StorageFillCubeCS, "StorageTarget.rsf", "FillCubeCS")
SANDBOX_COMPUTE(StorageFillCubeArrayCS, "StorageTarget.rsf", "FillCubeArrayCS")
SANDBOX_COMPUTE(StorageFillFaceLayerCS, "StorageTarget.rsf", "FillFaceLayerCS")
SANDBOX_COMPUTE(StorageFillFaceCS, "StorageTarget.rsf", "FillFaceCS")
SANDBOX_PIXEL(StorageDrawPS, "StorageTarget.rsf", "DrawPS")

SANDBOX_PIXEL(TextureArrayPS, "Texture.rsf", "ArrayPS")
SANDBOX_PIXEL(TextureVolumePS, "Texture.rsf", "VolumePS")
SANDBOX_PIXEL(TextureCubePS, "Texture.rsf", "CubePS")
SANDBOX_PIXEL(TextureCubeArrayPS, "Texture.rsf", "CubeArrayPS")
SANDBOX_PIXEL(TextureLoadPS, "Texture.rsf", "LoadPS")
SANDBOX_PIXEL(TextureAddressWPS, "Texture.rsf", "AddressWPS")
SANDBOX_PIXEL(TextureComparePS, "Texture.rsf", "ComparePS")
SANDBOX_PIXEL(TextureCompareLinearPS, "Texture.rsf", "CompareLinearPS")

SANDBOX_COMPUTE(TextureViewFillCS, "TextureView.rsf", "FillCS")
SANDBOX_COMPUTE(TextureViewReduceCS, "TextureView.rsf", "ReduceCS")
SANDBOX_PIXEL(TextureViewLevelPS, "TextureView.rsf", "LevelPS")
SANDBOX_PIXEL(TextureViewLayerPS, "TextureView.rsf", "LayerPS")

SANDBOX_VERTEX(VertexFloatVS, "Vertex.rsf", "FloatVS")
SANDBOX_VERTEX(VertexIntVS, "Vertex.rsf", "IntVS")
SANDBOX_VERTEX(VertexUintVS, "Vertex.rsf", "UintVS")
SANDBOX_VERTEX(VertexInstanceVS, "Vertex.rsf", "InstanceVS")
SANDBOX_VERTEX(VertexNoInputVS, "Vertex.rsf", "NoInputVS")
SANDBOX_PIXEL(VertexColorPS, "Vertex.rsf", "ColorPS")

SANDBOX_PIXEL(ViewRead2DPS, "ViewDimension.rsf", "Read2DPS")
SANDBOX_PIXEL(ViewReadLayer0PS, "ViewDimension.rsf", "ReadLayer0PS")
SANDBOX_PIXEL(ViewReadLayer4PS, "ViewDimension.rsf", "ReadLayer4PS")
SANDBOX_PIXEL(ViewReadCubeArrayPS, "ViewDimension.rsf", "ReadCubeArrayPS")
SANDBOX_COMPUTE(ViewFillFaceCS, "ViewDimension.rsf", "FillFaceCS")
SANDBOX_COMPUTE(ViewFillCubeCS, "ViewDimension.rsf", "FillCubeCS")
