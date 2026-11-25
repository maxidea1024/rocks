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
Result<ShaderHandle> D3D12Device::CreateShader(const ShaderCodeDesc& desc) noexcept {
    platform::LockGuard lock(gate_);
    const char* reason = "";
    auto created = shaders_.Create(desc, ShaderCodeKind::Dxil, &reason);
    if (!created && created.Error() == ErrorCode::InvalidArgument) {
        return FailCode<ShaderHandle>(ErrorCode::InvalidArgument, reason);
    }

    return created;
}

void D3D12Device::Retire(ShaderHandle shader) noexcept {
    platform::LockGuard lock(gate_);
    shaders_.Retire(shader);
}

// 쿠킹된 DXIL과 정규화 layout으로 만든다(ShaderSystem 16.1절 SSD18). root signature는 두 단계의
// slot 표·상수 버퍼·진단 버퍼로 만들고 캐시한다(7.2절 4항, RenderBase 9.11절 5항).
Result<PipelineHandle> D3D12Device::CreatePipeline(const PipelineDesc& desc) noexcept {
    platform::LockGuard lock(gate_);
    if (!ValidatePipelineDesc(desc)) {
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
        return FailCode<PipelineHandle>(ErrorCode::InvalidArgument,
                                        "vertex layout is invalid (RenderBase 10.4 limits)");
    }

    char error[PipelineSlotErrorBytes] = {};
    const DeviceShaderCode* vertex = nullptr;
    const DeviceShaderCode* pixel = nullptr;
    PipelineSlots slots;
    if (!FindPipelineShaders(shaders_, desc, vertex, pixel, error)) {
        return FailCode<PipelineHandle>(ErrorCode::InvalidArgument, error);
    }

    const DeviceShaderCode* stages[] = {vertex, pixel};
    if (!MatchShaderVertexInputs(vertex->Layout(), desc.layout, error) ||
        !ResolvePipelineSlots(stages, desc.bindings, desc.drawConstantBytes, slots, error)) {
        return FailCode<PipelineHandle>(ErrorCode::InvalidArgument, error);
    }

    RootLayout rootLayout;
    auto root = RootSignatureFor(slots, false, desc.drawConstantBytes, rootLayout);
    if (!root) {
        return Result<PipelineHandle>::Failure(root.Error());
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
    state.pRootSignature = root.Value().Get();
    state.VS = {vertex->Code().Data(), vertex->Code().Size()};
    state.PS = {pixel->Code().Data(), pixel->Code().Size()};
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
    state.SampleDesc.Count = desc.sampleCount;
    state.RasterizerState.MultisampleEnable = desc.sampleCount > 1 ? TRUE : FALSE;

    PipelineRecord record;
    record.rootSignature = Move(root.Value());
    record.root = rootLayout;
    record.slots = slots;
    record.bufferSlots = slots.bufferSlots;
    record.drawConstantBytes = desc.drawConstantBytes;
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

Result<void> D3D12Device::RejectDrawConstantSize(bool pipelineBound, usize bytes,
                                                 uint32 drawConstantBytes) noexcept {
    Diagnostic message;
    if (!pipelineBound) {
        message.Text("SetDrawConstants needs a pipeline bound while recording");
    } else {
        message.Text("SetDrawConstants got ")
            .Number(bytes > NumericLimits<uint32>::Max() ? NumericLimits<uint32>::Max()
                                                         : static_cast<uint32>(bytes))
            .Text(" bytes but the bound pipeline declares drawConstantBytes ")
            .Number(drawConstantBytes);
    }

    return Reject(message);
}

Result<void> D3D12Device::Reject(const Diagnostic& message) noexcept {
    auto assigned =
        lastError_.TryAssign(StringView(message.CStr(), platform::CStringLength(message.CStr())));
    (void)assigned;
    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

namespace {

D3D12_SHADER_VISIBILITY VisibilityOf(ShaderStage stage) noexcept {
    switch (stage) {
    case ShaderStage::Vertex:
        return D3D12_SHADER_VISIBILITY_VERTEX;
    case ShaderStage::Pixel:
        return D3D12_SHADER_VISIBILITY_PIXEL;
    case ShaderStage::Compute:
        break;
    }

    return D3D12_SHADER_VISIBILITY_ALL;
}

uint32 LowestBit(uint32 mask) noexcept {
    uint32 bit = 0;
    while ((mask & (1u << bit)) == 0) {
        ++bit;
    }

    return bit;
}

uint32 HighestBit(uint32 mask) noexcept {
    uint32 bit = 31;
    while ((mask & (1u << bit)) == 0) {
        --bit;
    }

    return bit;
}

// root signature 매개변수 수의 상한이다(table 8 + root CBV 28 + 진단 UAV 1 + 루트 상수 1보다
// 넉넉하다).
constexpr uint32 MaxRootParameters = MaxRootSignatureDwords;
// descriptor range 수의 상한이다(SRV·UAV는 자원마다 하나, Sampler·CBV는 단계마다 하나).
constexpr uint32 MaxRootRanges = MaxPipelineSlotBindings + 2 * MaxRootBudgetStages;

} // namespace

// 단계별 table과 예산 계획으로 root 배치를 정하고(RenderBase 9.11절 5항) 같은 배치의 root
// signature를 다시 쓴다.
Result<ComRef<ID3D12RootSignature>> D3D12Device::RootSignatureFor(const PipelineSlots& slots,
                                                                  bool compute,
                                                                  uint32 drawConstantBytes,
                                                                  RootLayout& layout) noexcept {
    using Outcome = Result<ComRef<ID3D12RootSignature>>;
    layout = RootLayout{};
    layout.stageCount = compute ? 1 : 2;
    layout.stages[0].stage = compute ? ShaderStage::Compute : ShaderStage::Vertex;
    layout.stages[1].stage = ShaderStage::Pixel;
    bool samplers[MaxRootBudgetStages] = {};
    for (uint32 index = 0; index < slots.count; ++index) {
        const PipelineSlotBinding& binding = slots.bindings[index];
        const uint32 stage = !compute && binding.stage == ShaderStage::Pixel ? 1u : 0u;
        RootStageTables& tables = layout.stages[stage];
        switch (binding.role) {
        case PipelineSlotRole::Texture:
        case PipelineSlotRole::BufferView:
            tables.srvBindings[tables.srvCount++] = static_cast<uint8>(index);
            break;
        case PipelineSlotRole::Sampler:
            tables.samplerBinding = static_cast<uint8>(index);
            samplers[stage] = true;
            break;
        case PipelineSlotRole::WritableBuffer:
        case PipelineSlotRole::WritableTexture:
            tables.uavBindings[tables.uavCount++] = static_cast<uint8>(index);
            break;
        }
    }

    RootBudgetInput input;
    input.stageCount = layout.stageCount;
    input.rootConstantDwords = drawConstantBytes / sizeof(uint32);
    input.debugUav = slots.debugBuffer && !compute;
    for (uint32 stage = 0; stage < layout.stageCount; ++stage) {
        const RootStageTables& tables = layout.stages[stage];
        input.stages[stage].tables = (tables.srvCount != 0 ? 1u : 0u) +
                                     (samplers[stage] ? 1u : 0u) + (tables.uavCount != 0 ? 1u : 0u);
        input.stages[stage].constantMask =
            slots.constantSlots.usedMask[ConstantBufferStageIndex(tables.stage)];
    }

    auto planned = PlanRootBudget(input);
    if (!planned) {
        return FailCode<ComRef<ID3D12RootSignature>>(
            ErrorCode::InvalidArgument, "pipeline bindings do not fit the 64 DWORD root signature");
    }

    layout.plan = planned.Value();
    layout.constantDwords = input.rootConstantDwords;

    // 매개변수: 단계마다 SRV·Sampler·UAV·CBV table → root CBV → 진단 root UAV → 루트 상수.
    D3D12_DESCRIPTOR_RANGE ranges[MaxRootRanges] = {};
    D3D12_ROOT_PARAMETER parameters[MaxRootParameters] = {};
    uint32 rangeCount = 0;
    uint32 count = 0;
    const auto table = [&](D3D12_SHADER_VISIBILITY visibility, uint32 first,
                           uint32 number) noexcept -> int32 {
        D3D12_ROOT_PARAMETER& parameter = parameters[count];
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable.NumDescriptorRanges = number;
        parameter.DescriptorTable.pDescriptorRanges = &ranges[first];
        parameter.ShaderVisibility = visibility;
        return static_cast<int32>(count++);
    };
    for (uint32 stage = 0; stage < layout.stageCount; ++stage) {
        RootStageTables& tables = layout.stages[stage];
        const D3D12_SHADER_VISIBILITY visibility = VisibilityOf(tables.stage);
        if (tables.srvCount != 0) {
            const uint32 first = rangeCount;
            for (uint32 entry = 0; entry < tables.srvCount; ++entry) {
                D3D12_DESCRIPTOR_RANGE& range = ranges[rangeCount++];
                range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                range.NumDescriptors = 1;
                range.BaseShaderRegister = slots.bindings[tables.srvBindings[entry]].location;
                range.OffsetInDescriptorsFromTableStart = entry;
            }

            tables.srvParam = table(visibility, first, tables.srvCount);
        }

        if (samplers[stage]) {
            D3D12_DESCRIPTOR_RANGE& range = ranges[rangeCount];
            range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
            range.NumDescriptors = 1;
            range.BaseShaderRegister = slots.bindings[tables.samplerBinding].location;
            tables.samplerParam = table(visibility, rangeCount++, 1);
        }

        if (tables.uavCount != 0) {
            const uint32 first = rangeCount;
            for (uint32 entry = 0; entry < tables.uavCount; ++entry) {
                D3D12_DESCRIPTOR_RANGE& range = ranges[rangeCount++];
                range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
                range.NumDescriptors = 1;
                range.BaseShaderRegister = slots.bindings[tables.uavBindings[entry]].location;
                range.OffsetInDescriptorsFromTableStart = entry;
            }

            tables.uavParam = table(visibility, first, tables.uavCount);
        }

        const uint32 overflow = layout.plan.tableMask[stage];
        if (overflow != 0) {
            tables.cbvFirst = LowestBit(overflow);
            tables.cbvCount = HighestBit(overflow) - tables.cbvFirst + 1;
            D3D12_DESCRIPTOR_RANGE& range = ranges[rangeCount];
            range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
            range.NumDescriptors = tables.cbvCount;
            range.BaseShaderRegister = tables.cbvFirst;
            tables.cbvParam = table(visibility, rangeCount++, 1);
        }
    }

    layout.rootCbvParam = count;
    for (uint32 entry = 0; entry < layout.plan.rootCbvCount; ++entry) {
        D3D12_ROOT_PARAMETER& parameter = parameters[count++];
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameter.Descriptor.ShaderRegister = layout.plan.rootOrderSlot[entry];
        parameter.Descriptor.RegisterSpace = 0;
        parameter.ShaderVisibility =
            VisibilityOf(layout.stages[layout.plan.rootOrderStage[entry]].stage);
    }

    if (input.debugUav) {
        D3D12_ROOT_PARAMETER& parameter = parameters[count];
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameter.Descriptor.ShaderRegister = 0;
        parameter.Descriptor.RegisterSpace = DebugBufferSpace;
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        layout.debugParam = static_cast<int32>(count++);
    }

    if (layout.constantDwords != 0) {
        D3D12_ROOT_PARAMETER& parameter = parameters[count];
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameter.Constants.ShaderRegister = 0;
        parameter.Constants.RegisterSpace = RootConstantsSpace;
        parameter.Constants.Num32BitValues = layout.constantDwords;
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        layout.constantsParam = static_cast<int32>(count++);
    }

    layout.parameterCount = count;

    // 키: compute 여부, 루트 상수 DWORD, 진단 UAV, 자원마다 (단계, 종류, register), 단계별 상수
    // 버퍼 마스크. 배치는 이 값들로만 정해진다.
    Array<uint8> key(allocator_);
    const uint8 head[3] = {static_cast<uint8>(compute ? 1 : 0),
                           static_cast<uint8>(layout.constantDwords),
                           static_cast<uint8>(input.debugUav ? 1 : 0)};
    if (!key.TryAppend(Span<const uint8>(head, sizeof(head)))) {
        return Outcome::Failure(ErrorCode::OutOfMemory);
    }

    for (uint32 index = 0; index < slots.count; ++index) {
        const PipelineSlotBinding& binding = slots.bindings[index];
        const uint8 entry[4] = {static_cast<uint8>(binding.stage), static_cast<uint8>(binding.kind),
                                static_cast<uint8>(binding.location & 0xFF),
                                static_cast<uint8>(binding.location >> 8)};
        if (!key.TryAppend(Span<const uint8>(entry, sizeof(entry)))) {
            return Outcome::Failure(ErrorCode::OutOfMemory);
        }
    }

    for (uint32 stage = 0; stage < ConstantBufferStageCount; ++stage) {
        const uint32 mask = slots.constantSlots.usedMask[stage];
        const uint8 entry[2] = {static_cast<uint8>(mask & 0xFF), static_cast<uint8>(mask >> 8)};
        if (!key.TryAppend(Span<const uint8>(entry, sizeof(entry)))) {
            return Outcome::Failure(ErrorCode::OutOfMemory);
        }
    }

    for (const RootSignatureEntry& entry : rootSignatures_) {
        if (entry.key.Size() == key.Size() &&
            memcmp(entry.key.Data(), key.Data(), key.Size()) == 0) {
            return Outcome::Success(entry.signature.Share());
        }
    }

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = count;
    desc.pParameters = parameters;
    desc.Flags = compute ? D3D12_ROOT_SIGNATURE_FLAG_NONE
                         : D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComRef<ID3DBlob> blob;
    ComRef<ID3DBlob> errors;
    RootSignatureEntry entry(allocator_);
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, blob.Out(),
                                           errors.Out())) ||
        FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                            IID_PPV_ARGS(entry.signature.Out())))) {
        return FailWith<ComRef<ID3D12RootSignature>>("root signature creation failed");
    }

    entry.key = Move(key);
    ComRef<ID3D12RootSignature> signature = entry.signature.Share();
    if (!rootSignatures_.TryPushBack(Move(entry))) {
        return Outcome::Failure(ErrorCode::OutOfMemory);
    }

    return Outcome::Success(Move(signature));
}

// root 인자를 건다(그리기·dispatch 직전). SRV·UAV table은 CPU 전용 heap의 원본을 shader-visible
// ring으로 복사하고(바로 앞 같은 목록이면 다시 쓴다), 넘친 CBV table은 ring에 CBV를 만든다. ring이
// 다 찼으면 거짓이다(그리기를 무시한다).
bool D3D12Device::ApplyRootBindings(const PipelineRecord& pipeline) noexcept {
    const bool compute = pipeline.compute;
    ID3D12RootSignature* signature = pipeline.rootSignature.Get();
    if (compute) {
        if (appliedComputeRoot_ != signature) {
            list_->SetComputeRootSignature(signature);
            appliedComputeRoot_ = signature;
        }
    } else if (appliedGraphicsRoot_ != signature) {
        list_->SetGraphicsRootSignature(signature);
        appliedGraphicsRoot_ = signature;
    }

    const auto setTable = [&](int32 parameter, D3D12_GPU_DESCRIPTOR_HANDLE handle) noexcept {
        if (compute) {
            list_->SetComputeRootDescriptorTable(static_cast<UINT>(parameter), handle);
        } else {
            list_->SetGraphicsRootDescriptorTable(static_cast<UINT>(parameter), handle);
        }
    };
    const TextureRecord* texture = textures_.Find(boundTexture_);
    if (texture == nullptr || !texture->hasSrv || texture->writable) {
        texture = textures_.Find(white_.slot);
    }

    const RootLayout& root = pipeline.root;
    uint32 sources[MaxPipelineSlotBindings] = {};
    for (uint32 stage = 0; stage < root.stageCount; ++stage) {
        const RootStageTables& tables = root.stages[stage];
        const uint32 stageIndex = ConstantBufferStageIndex(tables.stage);
        D3D12_GPU_DESCRIPTOR_HANDLE handle = {};
        if (tables.srvParam >= 0) {
            for (uint32 entry = 0; entry < tables.srvCount; ++entry) {
                const PipelineSlotBinding& binding =
                    pipeline.slots.bindings[tables.srvBindings[entry]];
                if (binding.role == PipelineSlotRole::Texture) {
                    sources[entry] = texture->srv;
                } else {
                    const BufferViewRecord* view =
                        bufferViews_.Find(boundViews_[binding.slot].slot);
                    sources[entry] = view != nullptr ? view->descriptor : nullBufferSrv_;
                }
            }

            if (!CopyTable(stageIndex, 0, sources, tables.srvCount, handle)) {
                return false;
            }

            setTable(tables.srvParam, handle);
        }

        if (tables.samplerParam >= 0) {
            setTable(tables.samplerParam, samplers_.Gpu(texture->sampler));
        }

        if (tables.uavParam >= 0) {
            for (uint32 entry = 0; entry < tables.uavCount; ++entry) {
                const PipelineSlotBinding& binding =
                    pipeline.slots.bindings[tables.uavBindings[entry]];
                if (binding.role == PipelineSlotRole::WritableTexture) {
                    const TextureRecord* view =
                        textures_.Find(boundWriteTextures_[binding.slot].slot);
                    sources[entry] = view != nullptr ? view->srv : nullBufferUav_;
                } else {
                    const BufferViewRecord* view =
                        bufferViews_.Find(boundWriteViews_[binding.slot].slot);
                    sources[entry] = view != nullptr ? view->descriptor : nullBufferUav_;
                }
            }

            if (!CopyTable(stageIndex, 1, sources, tables.uavCount, handle)) {
                return false;
            }

            setTable(tables.uavParam, handle);
        }

        if (tables.cbvParam >= 0) {
            D3D12_CPU_DESCRIPTOR_HANDLE cpu = {};
            if (!AllocateOnline(tables.cbvCount, cpu, handle)) {
                return false;
            }

            // 셰이더가 읽지 않는 사이 slot은 0 구간을 가리킨다.
            const uint32 overflow = root.plan.tableMask[stage];
            for (uint32 entry = 0; entry < tables.cbvCount; ++entry) {
                const uint32 slot = tables.cbvFirst + entry;
                D3D12_CONSTANT_BUFFER_VIEW_DESC view = {zeroConstants_, ConstantBufferAlignment};
                if ((overflow & (1u << slot)) != 0) {
                    uint32 bytes = 0;
                    view.BufferLocation = BoundConstantAddress(stageIndex, slot, bytes);
                    view.SizeInBytes = static_cast<UINT>(AlignConstantBufferBytes(bytes));
                }

                D3D12_CPU_DESCRIPTOR_HANDLE destination = cpu;
                destination.ptr += static_cast<SIZE_T>(entry) * onlineIncrement_;
                device_->CreateConstantBufferView(&view, destination);
            }

            setTable(tables.cbvParam, handle);
        }
    }

    for (uint32 entry = 0; entry < root.plan.rootCbvCount; ++entry) {
        const uint32 stageIndex =
            ConstantBufferStageIndex(root.stages[root.plan.rootOrderStage[entry]].stage);
        uint32 bytes = 0;
        const D3D12_GPU_VIRTUAL_ADDRESS address =
            BoundConstantAddress(stageIndex, root.plan.rootOrderSlot[entry], bytes);
        const UINT parameter = root.rootCbvParam + entry;
        if (compute) {
            list_->SetComputeRootConstantBufferView(parameter, address);
        } else {
            list_->SetGraphicsRootConstantBufferView(parameter, address);
        }
    }

    if (root.debugParam >= 0) {
        list_->SetGraphicsRootUnorderedAccessView(static_cast<UINT>(root.debugParam),
                                                  debugBuffer_->GetGPUVirtualAddress());
    }

    if (root.constantsParam >= 0) {
        if (compute) {
            list_->SetComputeRoot32BitConstants(static_cast<UINT>(root.constantsParam),
                                                root.constantDwords, constants_, 0);
        } else {
            list_->SetGraphicsRoot32BitConstants(static_cast<UINT>(root.constantsParam),
                                                 root.constantDwords, constants_, 0);
        }
    }

    return true;
}

} // namespace rocks::graphics::d3d12
