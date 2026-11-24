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
// slot 표로 만들고 캐시한다(7.2절 4항).
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

    auto root = RootSignatureFor(slots, false);
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

Result<ComRef<ID3D12RootSignature>> D3D12Device::RootSignatureFor(const PipelineSlots& slots,
                                                                  bool compute) noexcept {
    using Outcome = Result<ComRef<ID3D12RootSignature>>;
    // 키: compute 여부, 자원마다 (단계, 종류, register).
    Array<uint8> key(allocator_);
    if (!key.TryPushBack(compute ? 1 : 0)) {
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

    for (const RootSignatureEntry& entry : rootSignatures_) {
        if (entry.key.Size() == key.Size() &&
            memcmp(entry.key.Data(), key.Data(), key.Size()) == 0) {
            return Outcome::Success(entry.signature.Share());
        }
    }

    // 0: 루트 상수(b0 space3, 모든 단계), 그 뒤 자원마다 descriptor 하나짜리 table이다.
    D3D12_DESCRIPTOR_RANGE ranges[MaxPipelineSlotBindings] = {};
    D3D12_ROOT_PARAMETER parameters[RootFirstTable + MaxPipelineSlotBindings] = {};
    parameters[RootConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[RootConstants].Constants.ShaderRegister = 0;
    parameters[RootConstants].Constants.RegisterSpace = RootConstantsSpace;
    parameters[RootConstants].Constants.Num32BitValues = ConstantCount;
    parameters[RootConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    for (uint32 index = 0; index < slots.count; ++index) {
        const PipelineSlotBinding& binding = slots.bindings[index];
        D3D12_DESCRIPTOR_RANGE& range = ranges[index];
        switch (binding.role) {
        case PipelineSlotRole::Sampler:
            range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
            break;
        case PipelineSlotRole::WritableBuffer:
        case PipelineSlotRole::WritableTexture:
            range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            break;
        default:
            range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            break;
        }

        range.NumDescriptors = 1;
        range.BaseShaderRegister = binding.location;
        D3D12_ROOT_PARAMETER& parameter = parameters[RootFirstTable + index];
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable.NumDescriptorRanges = 1;
        parameter.DescriptorTable.pDescriptorRanges = &range;
        parameter.ShaderVisibility =
            binding.stage == ShaderStage::Vertex
                ? D3D12_SHADER_VISIBILITY_VERTEX
                : (binding.stage == ShaderStage::Pixel ? D3D12_SHADER_VISIBILITY_PIXEL
                                                       : D3D12_SHADER_VISIBILITY_ALL);
    }

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = RootFirstTable + slots.count;
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

void D3D12Device::ApplyRootBindings(const PipelineRecord& pipeline) noexcept {
    const bool compute = pipeline.compute;
    ID3D12RootSignature* root = pipeline.rootSignature.Get();
    if (compute) {
        if (appliedComputeRoot_ != root) {
            list_->SetComputeRootSignature(root);
            appliedComputeRoot_ = root;
        }

        list_->SetComputeRoot32BitConstants(RootConstants, ConstantCount, constants_, 0);
    } else {
        if (appliedGraphicsRoot_ != root) {
            list_->SetGraphicsRootSignature(root);
            appliedGraphicsRoot_ = root;
        }

        list_->SetGraphicsRoot32BitConstants(RootConstants, ConstantCount, constants_, 0);
    }

    const TextureRecord* texture = textures_.Find(boundTexture_);
    if (texture == nullptr || !texture->hasSrv || texture->writable) {
        texture = textures_.Find(white_.slot);
    }

    for (uint32 index = 0; index < pipeline.slots.count; ++index) {
        const PipelineSlotBinding& binding = pipeline.slots.bindings[index];
        D3D12_GPU_DESCRIPTOR_HANDLE table = srvs_.Gpu(nullBufferSrv_);
        switch (binding.role) {
        case PipelineSlotRole::Texture:
            table = srvs_.Gpu(texture->srv);
            break;
        case PipelineSlotRole::Sampler:
            table = samplers_.Gpu(texture->sampler);
            break;
        case PipelineSlotRole::BufferView: {
            const BufferViewRecord* view = bufferViews_.Find(boundViews_[binding.slot].slot);
            table = srvs_.Gpu(view != nullptr ? view->descriptor : nullBufferSrv_);
            break;
        }
        case PipelineSlotRole::WritableBuffer: {
            const BufferViewRecord* view = bufferViews_.Find(boundWriteViews_[binding.slot].slot);
            table = srvs_.Gpu(view != nullptr ? view->descriptor : nullBufferUav_);
            break;
        }
        case PipelineSlotRole::WritableTexture: {
            const TextureRecord* view = textures_.Find(boundWriteTextures_[binding.slot].slot);
            table = srvs_.Gpu(view != nullptr ? view->srv : nullBufferUav_);
            break;
        }
        }

        if (compute) {
            list_->SetComputeRootDescriptorTable(RootFirstTable + index, table);
        } else {
            list_->SetGraphicsRootDescriptorTable(RootFirstTable + index, table);
        }
    }
}

} // namespace rocks::graphics::d3d12
