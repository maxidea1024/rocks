#include "D3D12DeviceImpl.h"

namespace rocks::graphics::d3d12 {

namespace {

D3D12_COMPARISON_FUNC CompareOf(CompareFunction function) noexcept {
    // D3D12_COMPARISON_FUNC는 NEVER(1)부터 ALWAYS(8)까지 Rocks 순서와 같다.
    return static_cast<D3D12_COMPARISON_FUNC>(static_cast<uint32>(function) + 1);
}

D3D12_STENCIL_OP StencilOf(StencilOperation operation) noexcept {
    switch (operation) {
    case StencilOperation::Keep:
        return D3D12_STENCIL_OP_KEEP;
    case StencilOperation::Zero:
        return D3D12_STENCIL_OP_ZERO;
    case StencilOperation::Replace:
        return D3D12_STENCIL_OP_REPLACE;
    case StencilOperation::IncrementSaturate:
        return D3D12_STENCIL_OP_INCR_SAT;
    case StencilOperation::DecrementSaturate:
        return D3D12_STENCIL_OP_DECR_SAT;
    case StencilOperation::Invert:
        return D3D12_STENCIL_OP_INVERT;
    case StencilOperation::IncrementWrap:
        return D3D12_STENCIL_OP_INCR;
    case StencilOperation::DecrementWrap:
        return D3D12_STENCIL_OP_DECR;
    }

    return D3D12_STENCIL_OP_KEEP;
}

D3D12_BLEND BlendOf(BlendFactor factor) noexcept {
    switch (factor) {
    case BlendFactor::Zero:
        return D3D12_BLEND_ZERO;
    case BlendFactor::One:
        return D3D12_BLEND_ONE;
    case BlendFactor::SourceColor:
        return D3D12_BLEND_SRC_COLOR;
    case BlendFactor::InverseSourceColor:
        return D3D12_BLEND_INV_SRC_COLOR;
    case BlendFactor::SourceAlpha:
        return D3D12_BLEND_SRC_ALPHA;
    case BlendFactor::InverseSourceAlpha:
        return D3D12_BLEND_INV_SRC_ALPHA;
    case BlendFactor::DestinationColor:
        return D3D12_BLEND_DEST_COLOR;
    case BlendFactor::InverseDestinationColor:
        return D3D12_BLEND_INV_DEST_COLOR;
    case BlendFactor::DestinationAlpha:
        return D3D12_BLEND_DEST_ALPHA;
    case BlendFactor::InverseDestinationAlpha:
        return D3D12_BLEND_INV_DEST_ALPHA;
    case BlendFactor::SourceAlphaSaturate:
        return D3D12_BLEND_SRC_ALPHA_SAT;
    }

    return D3D12_BLEND_ONE;
}

// 알파 계수 자리에는 색 계수를 알파 계수로 바꿔 쓴다(D3D12 알파 블렌드는 *_COLOR 계수를 받지
// 않는다).
D3D12_BLEND AlphaBlendOf(BlendFactor factor) noexcept {
    switch (factor) {
    case BlendFactor::SourceColor:
        return D3D12_BLEND_SRC_ALPHA;
    case BlendFactor::InverseSourceColor:
        return D3D12_BLEND_INV_SRC_ALPHA;
    case BlendFactor::DestinationColor:
        return D3D12_BLEND_DEST_ALPHA;
    case BlendFactor::InverseDestinationColor:
        return D3D12_BLEND_INV_DEST_ALPHA;
    default:
        return BlendOf(factor);
    }
}

D3D12_BLEND_OP BlendOperationOf(BlendOperation operation) noexcept {
    // D3D12_BLEND_OP는 ADD(1)·SUBTRACT·REV_SUBTRACT·MIN·MAX로 Rocks 순서와 같다.
    return static_cast<D3D12_BLEND_OP>(static_cast<uint32>(operation) + 1);
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE TopologyTypeOf(PrimitiveTopology topology) noexcept {
    switch (topology) {
    case PrimitiveTopology::LineList:
    case PrimitiveTopology::LineStrip:
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    case PrimitiveTopology::PointList:
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    default:
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
}

D3D_PRIMITIVE_TOPOLOGY TopologyOf(PrimitiveTopology topology) noexcept {
    switch (topology) {
    case PrimitiveTopology::TriangleStrip:
        return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case PrimitiveTopology::LineList:
        return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    case PrimitiveTopology::LineStrip:
        return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case PrimitiveTopology::PointList:
        return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    default:
        return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

D3D12_DEPTH_STENCILOP_DESC StencilFaceOf(const StencilFace& face) noexcept {
    D3D12_DEPTH_STENCILOP_DESC desc = {};
    desc.StencilFailOp = StencilOf(face.fail);
    desc.StencilDepthFailOp = StencilOf(face.depthFail);
    desc.StencilPassOp = StencilOf(face.pass);
    desc.StencilFunc = CompareOf(face.compare);
    return desc;
}

} // namespace

// 다른 스레드에서도 부를 수 있다(concurrentResourceCreation, 내부 잠금으로 직렬화한다).
Result<PipelineHandle> D3D12Device::CreatePipeline(const PipelineDesc& desc) noexcept {
    platform::LockGuard lock(gate_);
    if (desc.vertex.hlsl.ByteCount() == 0 || desc.pixel.hlsl.ByteCount() == 0 ||
        !ValidatePipelineDesc(desc)) {
        return FailCode<PipelineHandle>(ErrorCode::InvalidArgument,
                                        "pipeline description breaks the pipeline rules");
    }

    for (uint32 index = 0; index < desc.colorTargetCount; ++index) {
        const PixelFormatCaps required =
            desc.blend[index].enable ? PixelFormatCaps::RenderTarget | PixelFormatCaps::Blendable
                                     : PixelFormatCaps::RenderTarget;
        if (!HasPixelFormatCaps(FormatCaps(desc.colorFormats[index]), required)) {
            return UnsupportedFormat<PipelineHandle>(desc.colorFormats[index]);
        }
    }

    if (desc.depthFormat != PixelFormat::Unknown &&
        !HasPixelFormatCaps(FormatCaps(desc.depthFormat), PixelFormatCaps::DepthStencil)) {
        return UnsupportedFormat<PipelineHandle>(desc.depthFormat);
    }

    if (!CheckPipelineFeatures(caps_, desc)) {
        return FailCode<PipelineHandle>(ErrorCode::Unsupported,
                                        "pipeline needs more color targets or a raster feature "
                                        "than the device supports");
    }

    auto layoutChecked = ValidateVertexLayout(desc.layout);
    if (!layoutChecked) {
        auto failed = Fail("vertex layout is invalid (RenderBase 10.4 limits)");
        (void)failed;
        return Result<PipelineHandle>::Failure(ErrorCode::InvalidArgument);
    }

    ComRef<ID3DBlob> vertexCode;
    ComRef<ID3DBlob> pixelCode;
    auto vertex = Compile(desc.vertex.hlsl, "VSMain", "vs_5_1", vertexCode);
    if (!vertex) {
        return Result<PipelineHandle>::Failure(vertex.Error());
    }

    auto matched = MatchVertexInputs(vertexCode.Get(), desc.layout);
    if (!matched) {
        return Result<PipelineHandle>::Failure(matched.Error());
    }

    auto pixel = Compile(desc.pixel.hlsl, "PSMain", "ps_5_1", pixelCode);
    if (!pixel) {
        return Result<PipelineHandle>::Failure(pixel.Error());
    }

    DrawBufferSlots bufferSlots;
    auto vertexBindings =
        ReflectBindings(vertexCode.Get(), ShaderStage::Vertex, bufferSlots, nullptr);
    auto pixelBindings =
        vertexBindings ? ReflectBindings(pixelCode.Get(), ShaderStage::Pixel, bufferSlots, nullptr)
                       : Move(vertexBindings);
    if (!pixelBindings) {
        return Result<PipelineHandle>::Failure(pixelBindings.Error());
    }

    D3D12_INPUT_ELEMENT_DESC elements[MaxVertexAttributes] = {};
    for (usize index = 0; index < desc.layout.attributes.Size(); ++index) {
        const VertexAttribute& attribute = desc.layout.attributes[index];
        const VertexStream& stream = desc.layout.streams[attribute.stream];
        const bool instance = stream.stepRate == VertexStepRate::Instance;
        elements[index].SemanticName = "ATTRIBUTE";
        elements[index].SemanticIndex = attribute.location;
        elements[index].Format = AttributeFormat(attribute.format);
        elements[index].InputSlot = attribute.stream;
        elements[index].AlignedByteOffset = attribute.offset;
        elements[index].InputSlotClass = instance ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                                                  : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        elements[index].InstanceDataStepRate = instance ? 1u : 0u;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC state = {};
    state.pRootSignature = rootSignature_.Get();
    state.VS = {vertexCode->GetBufferPointer(), vertexCode->GetBufferSize()};
    state.PS = {pixelCode->GetBufferPointer(), pixelCode->GetBufferSize()};
    state.BlendState.AlphaToCoverageEnable = desc.alphaToCoverage ? TRUE : FALSE;
    state.BlendState.IndependentBlendEnable = TRUE;
    for (uint32 index = 0; index < desc.colorTargetCount; ++index) {
        const ColorTargetBlend& source = desc.blend[index];
        D3D12_RENDER_TARGET_BLEND_DESC& blend = state.BlendState.RenderTarget[index];
        blend.BlendEnable = source.enable ? TRUE : FALSE;
        blend.SrcBlend = BlendOf(source.sourceColor);
        blend.DestBlend = BlendOf(source.destinationColor);
        blend.BlendOp = BlendOperationOf(source.colorOperation);
        blend.SrcBlendAlpha = AlphaBlendOf(source.sourceAlpha);
        blend.DestBlendAlpha = AlphaBlendOf(source.destinationAlpha);
        blend.BlendOpAlpha = BlendOperationOf(source.alphaOperation);
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask = static_cast<UINT8>(source.writeMask);
        state.RTVFormats[index] = d3d12::MappingOf(desc.colorFormats[index]).rtv;
    }

    state.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
    state.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    state.RasterizerState.CullMode = RasterCullMode(desc.raster.cullMode);
    // 앞면은 화면 반시계다(Graphics CullMode 규약, UE D3D12State.cpp:353과 같다).
    state.RasterizerState.FrontCounterClockwise = TRUE;
    state.RasterizerState.DepthBias = desc.raster.depthBias;
    state.RasterizerState.SlopeScaledDepthBias = desc.raster.depthBiasSlope;
    state.RasterizerState.DepthBiasClamp = desc.raster.depthBiasClamp;
    state.RasterizerState.DepthClipEnable = desc.raster.depthClamp ? FALSE : TRUE;
    const DepthStencilState& depth = desc.depthStencil;
    state.DepthStencilState.DepthEnable = depth.depthTest || depth.depthWrite ? TRUE : FALSE;
    state.DepthStencilState.DepthWriteMask =
        depth.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    state.DepthStencilState.DepthFunc =
        depth.depthTest ? CompareOf(depth.depthCompare) : D3D12_COMPARISON_FUNC_ALWAYS;
    state.DepthStencilState.StencilEnable = depth.stencilTest ? TRUE : FALSE;
    state.DepthStencilState.StencilReadMask = depth.stencilReadMask;
    state.DepthStencilState.StencilWriteMask = depth.stencilWriteMask;
    state.DepthStencilState.FrontFace = StencilFaceOf(depth.front);
    state.DepthStencilState.BackFace = StencilFaceOf(depth.back);
    state.DSVFormat = desc.depthFormat != PixelFormat::Unknown
                          ? d3d12::MappingOf(desc.depthFormat).dsv
                          : DXGI_FORMAT_UNKNOWN;
    state.InputLayout = {desc.layout.attributes.Size() != 0 ? elements : nullptr,
                         static_cast<UINT>(desc.layout.attributes.Size())};
    state.PrimitiveTopologyType = TopologyTypeOf(desc.topology);
    state.NumRenderTargets = desc.colorTargetCount;
    state.SampleDesc.Count = 1;

    PipelineRecord record;
    record.bufferSlots = bufferSlots;
    record.topology = TopologyOf(desc.topology);
    record.writesDepthStencil = PipelineWritesDepthStencil(desc);
    for (usize index = 0; index < desc.layout.streams.Size(); ++index) {
        record.strides[index] = desc.layout.streams[index].stride;
        record.streamMask |= 1u << index;
    }

    if (FAILED(device_->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(record.state.Out())))) {
        return FailWith<PipelineHandle>("CreateGraphicsPipelineState failed");
    }

    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return Result<PipelineHandle>::Failure(reserved.Error());
    }

    auto added = pipelines_.TryAdd(Move(record));
    if (!added) {
        return Result<PipelineHandle>::Failure(added.Error());
    }

    return Result<PipelineHandle>::Success(PipelineHandle{added.Value()});
}

// 컴파일한 VS 입력 signature를 배치와 대조한다(RenderBase 10.4절 셰이더 입력 대조). 사용자
// 입력은 시맨틱 ATTRIBUTEn만 받고, n이 배치에 있어야 하며 성분 타입 계열이 형식의 셰이더 타입과
// 같아야 한다. 셰이더가 읽지 않는 속성은 허용한다.
Result<void> D3D12Device::MatchVertexInputs(ID3DBlob* code, const VertexLayout& layout) noexcept {
    ComRef<ID3D12ShaderReflection> reflection;
    if (FAILED(D3DReflect(code->GetBufferPointer(), code->GetBufferSize(),
                          IID_PPV_ARGS(reflection.Out())))) {
        return Fail("D3DReflect failed on the vertex shader");
    }

    D3D12_SHADER_DESC shader = {};
    if (FAILED(reflection->GetDesc(&shader))) {
        return Fail("vertex shader reflection description failed");
    }

    for (UINT index = 0; index < shader.InputParameters; ++index) {
        D3D12_SIGNATURE_PARAMETER_DESC input = {};
        if (FAILED(reflection->GetInputParameterDesc(index, &input))) {
            return Fail("vertex shader input description failed");
        }

        if (input.SystemValueType != D3D_NAME_UNDEFINED) {
            continue; // SV_VertexID·SV_InstanceID 등
        }

        Diagnostic message;
        if (input.SemanticName == nullptr || !IsAttributeSemantic(input.SemanticName) ||
            input.SemanticIndex >= MaxVertexAttributes) {
            message.Text("vertex shader input ")
                .Text(input.SemanticName != nullptr ? input.SemanticName : "?")
                .Number(input.SemanticIndex)
                .Text(" must use semantic ATTRIBUTE0-15");
            return Reject(message);
        }

        const VertexAttribute* attribute = nullptr;
        for (const VertexAttribute& candidate : layout.attributes) {
            if (candidate.location == input.SemanticIndex) {
                attribute = &candidate;
            }
        }

        if (attribute == nullptr) {
            message.Text("vertex shader reads location ")
                .Number(input.SemanticIndex)
                .Text(" which the vertex layout does not provide");
            return Reject(message);
        }

        VertexShaderType shaderType = VertexShaderType::Float;
        if (input.ComponentType == D3D_REGISTER_COMPONENT_SINT32) {
            shaderType = VertexShaderType::Int;
        } else if (input.ComponentType == D3D_REGISTER_COMPONENT_UINT32) {
            shaderType = VertexShaderType::Uint;
        }

        if (VertexFormatShaderType(attribute->format) != shaderType) {
            message.Text("vertex input location ")
                .Number(input.SemanticIndex)
                .Text(": format ")
                .Text(VertexFormatName(attribute->format))
                .Text(" feeds ")
                .Text(VertexShaderTypeName(VertexFormatShaderType(attribute->format)))
                .Text(" but the shader reads ")
                .Text(VertexShaderTypeName(shaderType));
            return Reject(message);
        }
    }

    return Result<void>::Success();
}

// 셰이더 자원이 고정 배치(PipelineDesc·ComputePipelineDesc) 안인지 보고 버퍼 view slot 배치를
// 더한다. b0 상수, PS t0 텍스처·s0 샘플러, t1–t4 버퍼 SRV, compute의 u0–u3 버퍼 UAV(writeSlots)만
// 받는다. 두 단계가 같은 slot을 다른 종류로 읽거나 배치 밖 자원이면 InvalidArgument다.
Result<void> D3D12Device::ReflectBindings(ID3DBlob* code, ShaderStage stage, DrawBufferSlots& slots,
                                          DrawBufferSlots* writeSlots) noexcept {
    ComRef<ID3D12ShaderReflection> reflection;
    D3D12_SHADER_DESC shader = {};
    if (FAILED(D3DReflect(code->GetBufferPointer(), code->GetBufferSize(),
                          IID_PPV_ARGS(reflection.Out()))) ||
        FAILED(reflection->GetDesc(&shader))) {
        return Fail("shader resource reflection failed");
    }

    const bool pixel = stage == ShaderStage::Pixel;
    for (UINT index = 0; index < shader.BoundResources; ++index) {
        D3D12_SHADER_INPUT_BIND_DESC resource = {};
        if (FAILED(reflection->GetResourceBindingDesc(index, &resource))) {
            return Fail("shader resource binding description failed");
        }

        const bool single = resource.BindCount == 1 && resource.Space == 0;
        const uint32 slot = resource.BindPoint - 1;
        const bool viewSlot = resource.BindPoint >= 1 && slot < MaxDrawBufferViews;
        const bool writeSlot = writeSlots != nullptr && resource.BindPoint < MaxDrawBufferViews;
        const PixelShaderType type =
            resource.ReturnType == D3D_RETURN_TYPE_SINT
                ? PixelShaderType::Int
                : (resource.ReturnType == D3D_RETURN_TYPE_UINT ? PixelShaderType::Uint
                                                               : PixelShaderType::Float);
        bool fits = false;
        switch (resource.Type) {
        case D3D_SIT_CBUFFER:
            fits = single && resource.BindPoint == 0;
            break;
        case D3D_SIT_SAMPLER:
            fits = single && pixel && resource.BindPoint == 0;
            break;
        case D3D_SIT_TEXTURE:
            if (resource.Dimension != D3D_SRV_DIMENSION_BUFFER) {
                fits = single && pixel && resource.BindPoint == 0;
                break;
            }

            fits =
                single && viewSlot && AddDrawBufferSlot(slots, slot, BufferViewKind::Typed, type);
            break;
        case D3D_SIT_STRUCTURED:
        case D3D_SIT_BYTEADDRESS:
            // 구조화 버퍼의 NumSamples는 원소 stride다(D3D12_SHADER_INPUT_BIND_DESC).
            fits =
                single && viewSlot &&
                AddDrawBufferSlot(slots, slot,
                                  resource.Type == D3D_SIT_STRUCTURED ? BufferViewKind::Structured
                                                                      : BufferViewKind::Raw,
                                  PixelShaderType::None,
                                  resource.Type == D3D_SIT_STRUCTURED ? resource.NumSamples : 0u);
            break;
        case D3D_SIT_UAV_RWTYPED:
            fits = single && writeSlot && resource.Dimension == D3D_SRV_DIMENSION_BUFFER &&
                   AddDrawBufferSlot(*writeSlots, resource.BindPoint, BufferViewKind::Typed, type);
            break;
        case D3D_SIT_UAV_RWSTRUCTURED:
        case D3D_SIT_UAV_RWBYTEADDRESS:
            fits = single && writeSlot &&
                   AddDrawBufferSlot(
                       *writeSlots, resource.BindPoint,
                       resource.Type == D3D_SIT_UAV_RWSTRUCTURED ? BufferViewKind::Structured
                                                                 : BufferViewKind::Raw,
                       PixelShaderType::None,
                       resource.Type == D3D_SIT_UAV_RWSTRUCTURED ? resource.NumSamples : 0u);
            break;
        default:
            break;
        }

        if (!fits) {
            Diagnostic message;
            message.Text(pixel ? "pixel" : (stage == ShaderStage::Vertex ? "vertex" : "compute"))
                .Text(" shader resource ")
                .Text(resource.Name != nullptr ? resource.Name : "?")
                .Text(" is outside the fixed layout (b0, pixel t0 and s0, buffer views t1-t4, "
                      "compute buffer UAVs u0-u3; no UAVs in graphics stages)");
            return Reject(message);
        }
    }

    return Result<void>::Success();
}

Result<void> D3D12Device::Reject(const Diagnostic& message) noexcept {
    auto assigned =
        lastError_.TryAssign(StringView(message.CStr(), platform::CStringLength(message.CStr())));
    (void)assigned;
    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

Result<void> D3D12Device::CreateRootSignature() noexcept {
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_DESCRIPTOR_RANGE samplerRange = {};
    samplerRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    samplerRange.NumDescriptors = 1;
    samplerRange.BaseShaderRegister = 0;
    samplerRange.OffsetInDescriptorsFromTableStart = 0;
    // 버퍼 view slot n은 register t(n + 1)이고 VS·PS 모두 읽는다(PipelineDesc 고정 배치).
    // slot마다 table 하나라 view의 descriptor 칸을 그대로 가리킨다(descriptor 복사 없음).
    D3D12_DESCRIPTOR_RANGE viewRanges[MaxDrawBufferViews] = {};
    D3D12_ROOT_PARAMETER parameters[RootBufferViews + MaxDrawBufferViews] = {};
    parameters[RootConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[RootConstants].Constants.ShaderRegister = 0;
    parameters[RootConstants].Constants.Num32BitValues = ConstantCount;
    parameters[RootConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[RootTexture].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[RootTexture].DescriptorTable.NumDescriptorRanges = 1;
    parameters[RootTexture].DescriptorTable.pDescriptorRanges = &range;
    parameters[RootTexture].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[RootSampler].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[RootSampler].DescriptorTable.NumDescriptorRanges = 1;
    parameters[RootSampler].DescriptorTable.pDescriptorRanges = &samplerRange;
    parameters[RootSampler].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    for (uint32 slot = 0; slot < MaxDrawBufferViews; ++slot) {
        viewRanges[slot].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        viewRanges[slot].NumDescriptors = 1;
        viewRanges[slot].BaseShaderRegister = slot + 1;
        D3D12_ROOT_PARAMETER& parameter = parameters[RootBufferViews + slot];
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable.NumDescriptorRanges = 1;
        parameter.DescriptorTable.pDescriptorRanges = &viewRanges[slot];
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = RootBufferViews + MaxDrawBufferViews;
    desc.pParameters = parameters;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComRef<ID3DBlob> blob;
    ComRef<ID3DBlob> errors;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, blob.Out(),
                                           errors.Out())) ||
        FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                            IID_PPV_ARGS(rootSignature_.Out())))) {
        return Fail("root signature creation failed");
    }

    return Result<void>::Success();
}

Result<void> D3D12Device::Compile(StringView source, const char* entry, const char* target,
                                  ComRef<ID3DBlob>& code) noexcept {
    ComRef<ID3DBlob> errors;
    // 행렬은 행 우선으로 담는다(DrawConstants 규약, ShaderSystem 7.2절 6항 FXC /Zpr). UE도 같은
    // 플래그로 컴파일한다(D3DShaderCompiler.cpp:1337–1339).
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_PACK_MATRIX_ROW_MAJOR;
    if (validation_) {
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
    } else {
        flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
    }

    const HRESULT compiled = D3DCompile(source.Data(), source.ByteCount(), "RocksShader", nullptr,
                                        nullptr, entry, target, flags, 0, code.Out(), errors.Out());
    if (FAILED(compiled)) {
        // 파이프라인을 만드는 호출자가 LastError로 기록한다(LoggingSystem 13절). 진단은 앞쪽
        // MaxShaderDiagnosticLogBytes까지 싣는다.
        StringView diagnostics("no compiler output");
        if (errors) {
            diagnostics = StringView(static_cast<const char*>(errors->GetBufferPointer()),
                                     errors->GetBufferSize())
                              .LeftBytesAtCodePoint(MaxShaderDiagnosticLogBytes);
        }

        char storage[MaxShaderDiagnosticLogBytes + 128];
        FixedStringBuilder text{Span<char>(storage)};
        (void)text.TryAppendFormat("Shader {} ({}) failed to compile (HRESULT {:#x}): {}", entry,
                                   target, static_cast<uint32>(compiled), diagnostics);
        auto assigned = lastError_.TryAssign(text.View());
        (void)assigned;
        failureTruncated_ = CopyStage("D3DCompile");
        failureResult_ = compiled;
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }

    return Result<void>::Success();
}

} // namespace rocks::graphics::d3d12
