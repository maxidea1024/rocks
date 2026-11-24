#include "D3D11DeviceImpl.h"

// GX3·GX4 명령 기록이다: 파이프라인 고정 기능 상태, compute 파이프라인·dispatch, 패스
// attachment(BeginPass), 간접 그리기·dispatch, viewport·scissor·스텐실 기준, 버퍼·텍스처 복사,
// UAV clear. D3D11은 드라이버가 자원 상태와 hazard를 관리하므로 전이를 내지 않는다(GX5).
namespace rocks::graphics::d3d11 {

namespace {

D3D11_COMPARISON_FUNC CompareOf(CompareFunction function) noexcept {
    // D3D11_COMPARISON_FUNC는 NEVER(1)부터 ALWAYS(8)까지 Rocks 순서와 같다.
    return static_cast<D3D11_COMPARISON_FUNC>(static_cast<uint32>(function) + 1);
}

D3D11_STENCIL_OP StencilOf(StencilOperation operation) noexcept {
    switch (operation) {
    case StencilOperation::Keep:
        return D3D11_STENCIL_OP_KEEP;
    case StencilOperation::Zero:
        return D3D11_STENCIL_OP_ZERO;
    case StencilOperation::Replace:
        return D3D11_STENCIL_OP_REPLACE;
    case StencilOperation::IncrementSaturate:
        return D3D11_STENCIL_OP_INCR_SAT;
    case StencilOperation::DecrementSaturate:
        return D3D11_STENCIL_OP_DECR_SAT;
    case StencilOperation::Invert:
        return D3D11_STENCIL_OP_INVERT;
    case StencilOperation::IncrementWrap:
        return D3D11_STENCIL_OP_INCR;
    case StencilOperation::DecrementWrap:
        return D3D11_STENCIL_OP_DECR;
    }

    return D3D11_STENCIL_OP_KEEP;
}

D3D11_BLEND BlendOf(BlendFactor factor) noexcept {
    switch (factor) {
    case BlendFactor::Zero:
        return D3D11_BLEND_ZERO;
    case BlendFactor::One:
        return D3D11_BLEND_ONE;
    case BlendFactor::SourceColor:
        return D3D11_BLEND_SRC_COLOR;
    case BlendFactor::InverseSourceColor:
        return D3D11_BLEND_INV_SRC_COLOR;
    case BlendFactor::SourceAlpha:
        return D3D11_BLEND_SRC_ALPHA;
    case BlendFactor::InverseSourceAlpha:
        return D3D11_BLEND_INV_SRC_ALPHA;
    case BlendFactor::DestinationColor:
        return D3D11_BLEND_DEST_COLOR;
    case BlendFactor::InverseDestinationColor:
        return D3D11_BLEND_INV_DEST_COLOR;
    case BlendFactor::DestinationAlpha:
        return D3D11_BLEND_DEST_ALPHA;
    case BlendFactor::InverseDestinationAlpha:
        return D3D11_BLEND_INV_DEST_ALPHA;
    case BlendFactor::SourceAlphaSaturate:
        return D3D11_BLEND_SRC_ALPHA_SAT;
    }

    return D3D11_BLEND_ONE;
}

// 알파 계수 자리에는 색 계수를 알파 계수로 바꿔 쓴다(D3D11 알파 블렌드는 *_COLOR 계수를 받지
// 않는다).
D3D11_BLEND AlphaBlendOf(BlendFactor factor) noexcept {
    switch (factor) {
    case BlendFactor::SourceColor:
        return D3D11_BLEND_SRC_ALPHA;
    case BlendFactor::InverseSourceColor:
        return D3D11_BLEND_INV_SRC_ALPHA;
    case BlendFactor::DestinationColor:
        return D3D11_BLEND_DEST_ALPHA;
    case BlendFactor::InverseDestinationColor:
        return D3D11_BLEND_INV_DEST_ALPHA;
    default:
        return BlendOf(factor);
    }
}

D3D11_BLEND_OP BlendOperationOf(BlendOperation operation) noexcept {
    // D3D11_BLEND_OP는 ADD(1)·SUBTRACT·REV_SUBTRACT·MIN·MAX로 Rocks 순서와 같다.
    return static_cast<D3D11_BLEND_OP>(static_cast<uint32>(operation) + 1);
}

D3D11_PRIMITIVE_TOPOLOGY TopologyOf(PrimitiveTopology topology) noexcept {
    switch (topology) {
    case PrimitiveTopology::TriangleStrip:
        return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case PrimitiveTopology::LineList:
        return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
    case PrimitiveTopology::LineStrip:
        return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case PrimitiveTopology::PointList:
        return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
    default:
        return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

D3D11_DEPTH_STENCILOP_DESC StencilFaceOf(const StencilFace& face) noexcept {
    D3D11_DEPTH_STENCILOP_DESC desc = {};
    desc.StencilFailOp = StencilOf(face.fail);
    desc.StencilDepthFailOp = StencilOf(face.depthFail);
    desc.StencilPassOp = StencilOf(face.pass);
    desc.StencilFunc = CompareOf(face.compare);
    return desc;
}

} // namespace

Result<void> D3D11Device::CreatePipelineStates(const PipelineDesc& desc,
                                               PipelineRecord& record) noexcept {
    D3D11_BLEND_DESC blend = {};
    blend.AlphaToCoverageEnable = desc.alphaToCoverage ? TRUE : FALSE;
    blend.IndependentBlendEnable = TRUE;
    for (uint32 index = 0; index < MaxColorTargets; ++index) {
        const ColorTargetBlend& source = desc.blend[index];
        D3D11_RENDER_TARGET_BLEND_DESC& target = blend.RenderTarget[index];
        target.BlendEnable = source.enable ? TRUE : FALSE;
        target.SrcBlend = BlendOf(source.sourceColor);
        target.DestBlend = BlendOf(source.destinationColor);
        target.BlendOp = BlendOperationOf(source.colorOperation);
        target.SrcBlendAlpha = AlphaBlendOf(source.sourceAlpha);
        target.DestBlendAlpha = AlphaBlendOf(source.destinationAlpha);
        target.BlendOpAlpha = BlendOperationOf(source.alphaOperation);
        target.RenderTargetWriteMask = static_cast<UINT8>(source.writeMask);
    }

    HRESULT result = device_->CreateBlendState(&blend, record.blend.Out());
    if (FAILED(result)) {
        return Fail("CreateBlendState", result);
    }

    // 앞면은 화면 반시계다(UE D3D11State.cpp:211, ShaderSystem 7.2절 8항, D3D12와 같다). scissor는
    // 늘 켠다(패스 시작 때 대상 전체).
    D3D11_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = RasterCullMode(desc.raster.cullMode);
    raster.FrontCounterClockwise = TRUE;
    raster.MultisampleEnable = desc.sampleCount > 1 ? TRUE : FALSE;
    raster.DepthBias = desc.raster.depthBias;
    raster.DepthBiasClamp = desc.raster.depthBiasClamp;
    raster.SlopeScaledDepthBias = desc.raster.depthBiasSlope;
    raster.DepthClipEnable = desc.raster.depthClamp ? FALSE : TRUE;
    raster.ScissorEnable = TRUE;
    result = device_->CreateRasterizerState(&raster, record.raster.Out());
    if (FAILED(result)) {
        return Fail("CreateRasterizerState", result);
    }

    const DepthStencilState& state = desc.depthStencil;
    D3D11_DEPTH_STENCIL_DESC depth = {};
    depth.DepthEnable = state.depthTest || state.depthWrite ? TRUE : FALSE;
    depth.DepthWriteMask =
        state.depthWrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = state.depthTest ? CompareOf(state.depthCompare) : D3D11_COMPARISON_ALWAYS;
    depth.StencilEnable = state.stencilTest ? TRUE : FALSE;
    depth.StencilReadMask = state.stencilReadMask;
    depth.StencilWriteMask = state.stencilWriteMask;
    depth.FrontFace = StencilFaceOf(state.front);
    depth.BackFace = StencilFaceOf(state.back);
    result = device_->CreateDepthStencilState(&depth, record.depthStencil.Out());
    if (FAILED(result)) {
        return Fail("CreateDepthStencilState", result);
    }

    record.topology = TopologyOf(desc.topology);
    record.writesDepthStencil = PipelineWritesDepthStencil(desc);
    return Result<void>::Success();
}

Result<PipelineHandle>
D3D11Device::CreateComputePipeline(const ComputePipelineDesc& desc) noexcept {
    platform::LockGuard lock(gate_);
    if (!ValidateComputePipelineDesc(desc)) {
        auto failed = Fail("compute pipeline needs valid drawConstantBytes");
        (void)failed;
        return Result<PipelineHandle>::Failure(ErrorCode::InvalidArgument);
    }

    char error[PipelineSlotErrorBytes] = {};
    const DeviceShaderCode* shader = nullptr;
    PipelineRecord record;
    record.drawConstantBytes = desc.drawConstantBytes;
    if (!FindComputeShader(shaders_, desc, shader, error) ||
        !ResolvePipelineSlots(Span<const DeviceShaderCode* const>(&shader, 1), desc.bindings,
                              desc.drawConstantBytes, record.slots, error)) {
        auto failed = Fail(error);
        (void)failed;
        return Result<PipelineHandle>::Failure(ErrorCode::InvalidArgument);
    }

    record.bufferSlots = record.slots.bufferSlots;
    record.writeSlots = record.slots.writeSlots;
    record.writeTextures = record.slots.writeTextures;
    const HRESULT result = device_->CreateComputeShader(
        shader->Code().Data(), shader->Code().Size(), nullptr, record.computeShader.Out());
    if (FAILED(result)) {
        return FailWith<PipelineHandle>("CreateComputeShader", result);
    }

    record.compute = true;
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

Result<CommandContext*> D3D11Device::BeginPass(const PassDesc& pass) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<CommandContext*>::Failure(owner.Error());
    }

    platform::LockGuard lock(gate_);
    const auto fail = [this](ErrorCode code, const char* message) noexcept {
        auto assigned = lastError_.TryAssign(StringView(message, platform::CStringLength(message)));
        (void)assigned;
        return Result<CommandContext*>::Failure(code);
    };
    if (pass.queue != QueueKind::Graphics) {
        return fail(ErrorCode::Unsupported, "D3D11 has no async compute queue");
    }

    if (pass.colorCount > MaxColorTargets) {
        return fail(ErrorCode::InvalidArgument,
                    "pass has more color attachments than MaxColorTargets");
    }

    if (pass.colorCount > caps_.limits.colorAttachments) {
        return fail(ErrorCode::Unsupported,
                    "pass has more color attachments than the device supports");
    }

    // attachment 확인: 살아 있는 텍스처, view 아님, 용도, 같은 크기·표본 수, 중복 없음.
    Target target;
    bool sized = false;
    const auto check = [&](TextureHandle handle, bool depth, uint32 mip,
                           uint32 slice) noexcept -> ErrorCode {
        const TextureRecord* record = textures_.Find(handle.slot);
        if (record == nullptr) {
            return ErrorCode::StaleHandle;
        }

        // 크기는 attachment mip의 크기다(텍스처 GX2 C부).
        if (record->view || (depth ? !record->depthTarget : !record->renderTarget) ||
            !IsAttachmentInRange(record->dimension, record->depth, record->mipLevels,
                                 record->slices, mip, slice)) {
            return ErrorCode::InvalidArgument;
        }

        const uint32 mipWidth = MipExtent(record->width, mip);
        const uint32 mipHeight = MipExtent(record->height, mip);
        if (sized && (mipWidth != target.width || mipHeight != target.height ||
                      record->sampleCount != target.sampleCount)) {
            return ErrorCode::InvalidArgument;
        }

        target.width = mipWidth;
        target.height = mipHeight;
        target.sampleCount = record->sampleCount;
        sized = true;
        return ErrorCode::None;
    };
    for (uint32 index = 0; index < pass.colorCount; ++index) {
        const ErrorCode checked = check(pass.colors[index].texture, false,
                                        pass.colors[index].mipLevel, pass.colors[index].slice);
        if (checked != ErrorCode::None) {
            return fail(checked, "color attachment is stale, not a render target, a view or of a "
                                 "different size");
        }

        for (uint32 other = 0; other < index; ++other) {
            if (pass.colors[other].texture == pass.colors[index].texture &&
                pass.colors[other].mipLevel == pass.colors[index].mipLevel &&
                pass.colors[other].slice == pass.colors[index].slice) {
                return fail(ErrorCode::InvalidArgument,
                            "color attachment appears twice in the pass");
            }
        }
    }

    const bool hasDepth = !pass.depth.texture.IsNull();
    if (hasDepth) {
        const ErrorCode checked =
            check(pass.depth.texture, true, pass.depth.mipLevel, pass.depth.slice);
        if (checked != ErrorCode::None) {
            return fail(checked, "depth attachment is stale, not a depth target, a view or of a "
                                 "different size");
        }
    }

    // resolve 대상: MSAA attachment의 1표본 RenderTarget, 같은 형식·크기, view 아님, 겹침 없음.
    for (uint32 index = 0; index < pass.colorCount; ++index) {
        const TextureHandle resolve = pass.colors[index].resolveTarget;
        if (resolve.IsNull()) {
            continue;
        }

        const TextureRecord* source = textures_.Find(pass.colors[index].texture.slot);
        const TextureRecord* resolved = textures_.Find(resolve.slot);
        if (resolved == nullptr) {
            return Result<CommandContext*>::Failure(ErrorCode::StaleHandle);
        }

        bool overlaps = resolve == pass.depth.texture;
        for (uint32 other = 0; other < pass.colorCount; ++other) {
            overlaps = overlaps || pass.colors[other].texture == resolve ||
                       (other != index && pass.colors[other].resolveTarget == resolve);
        }

        // 대상은 그 mip·slice가 원본과 같은 크기다. 3D 대상(깊이 면 resolve)은 받지 않는다.
        const uint32 resolveMip = pass.colors[index].resolveMipLevel;
        const uint32 resolveSlice = pass.colors[index].resolveSlice;
        if (source->sampleCount < 2 || resolved->view || !resolved->renderTarget ||
            resolved->sampleCount != 1 || resolved->format != source->format ||
            resolved->dimension == TextureDimension::Texture3D ||
            !IsAttachmentInRange(resolved->dimension, resolved->depth, resolved->mipLevels,
                                 resolved->slices, resolveMip, resolveSlice) ||
            MipExtent(resolved->width, resolveMip) != source->width ||
            MipExtent(resolved->height, resolveMip) != source->height || overlaps) {
            return fail(ErrorCode::InvalidArgument,
                        "resolve target needs an MSAA color attachment and a distinct "
                        "single-sample render target of the same format and size");
        }

        target.resolves[index] = resolve;
        target.resolveMips[index] = resolveMip;
        target.resolveSlices[index] = resolveSlice;
        target.colorSlices[index] = pass.colors[index].slice;
    }

    if (recording_) {
        return Result<CommandContext*>::Failure(ErrorCode::Busy);
    }

    if (timeline_.IsLost()) {
        return Result<CommandContext*>::Failure(ErrorCode::Destroyed);
    }

    // attachment view는 패스마다 그 mip·slice로 만든다.
    target.colorCount = pass.colorCount;
    for (uint32 index = 0; index < pass.colorCount; ++index) {
        const TextureRecord* record = textures_.Find(pass.colors[index].texture.slot);
        const HRESULT created = CreateAttachmentView(*record, pass.colors[index].mipLevel,
                                                     pass.colors[index].slice, passRtvs_[index]);
        if (FAILED(created)) {
            return FailWith<CommandContext*>("CreateRenderTargetView (pass)", created);
        }

        target.rtvs[index] = passRtvs_[index].Get();
        target.colors[index] = pass.colors[index].texture;
    }

    if (pass.colorCount != 0) {
        const TextureRecord* first = textures_.Find(pass.colors[0].texture.slot);
        target.rtv = target.rtvs[0];
        target.resource = first->texture.Get();
        target.format = first->format;
        target.texture = pass.colors[0].texture;
    }

    if (hasDepth) {
        const TextureRecord* depth = textures_.Find(pass.depth.texture.slot);
        target.depth = pass.depth.texture;
        target.depthReadOnly = pass.depth.readOnly;
        const HRESULT created = CreateAttachmentView(*depth, pass.depth.mipLevel, pass.depth.slice,
                                                     pass.depth.readOnly, passDsv_);
        if (FAILED(created)) {
            return FailWith<CommandContext*>("CreateDepthStencilView (pass)", created);
        }

        target.dsv = passDsv_.Get();
    }

    BeginRecording(target, pass);
    presentTarget_ = {};
    return Result<CommandContext*>::Success(this);
}

void D3D11Device::SetWritableBufferView(uint32 slot, BufferViewHandle view) noexcept {
    platform::LockGuard lock(gate_);
    if (slot >= MaxDrawBufferViews) {
        return;
    }

    const BufferViewRecord* record = bufferViews_.Find(view.slot);
    const bool writable = record != nullptr && record->access == ViewAccess::ReadWrite;
    boundWriteViews_[slot] = writable ? view : BufferViewHandle{};
    // 쓰기 slot은 버퍼·텍스처가 같은 번호 공간이다(나중에 묶은 하나만 남는다).
    boundWriteTextures_[slot] = {};
}

void D3D11Device::SetWritableTexture(uint32 slot, TextureHandle view) noexcept {
    platform::LockGuard lock(gate_);
    if (slot >= MaxDrawBufferViews) {
        return;
    }

    const TextureRecord* record = textures_.Find(view.slot);
    const bool writable = record != nullptr && record->uav;
    boundWriteTextures_[slot] = writable ? view : TextureHandle{};
    boundWriteViews_[slot] = {};
}

void D3D11Device::ApplySlots(const PipelineRecord& pipeline) noexcept {
    const TextureRecord* texture = textures_.Find(boundTexture_);
    if (texture == nullptr || !texture->srv) {
        texture = textures_.Find(white_.slot);
    }

    for (uint32 index = 0; index < pipeline.slots.count; ++index) {
        const PipelineSlotBinding& binding = pipeline.slots.bindings[index];
        const UINT location = binding.location;
        ID3D11ShaderResourceView* view = nullptr;
        ID3D11SamplerState* sampler = nullptr;
        ID3D11UnorderedAccessView* unordered = nullptr;
        switch (binding.role) {
        case PipelineSlotRole::Texture:
            view = texture->srv.Get();
            break;
        case PipelineSlotRole::Sampler:
            sampler = texture->sampler;
            break;
        case PipelineSlotRole::BufferView: {
            const BufferViewRecord* record = bufferViews_.Find(boundViews_[binding.slot].slot);
            view = record != nullptr ? record->srv.Get() : nullptr;
            break;
        }
        case PipelineSlotRole::WritableBuffer: {
            const BufferViewRecord* record = bufferViews_.Find(boundWriteViews_[binding.slot].slot);
            unordered = record != nullptr ? record->uav.Get() : nullptr;
            deferred_->CSSetUnorderedAccessViews(location, 1, &unordered, nullptr);
            continue;
        }
        case PipelineSlotRole::WritableTexture: {
            const TextureRecord* record = textures_.Find(boundWriteTextures_[binding.slot].slot);
            unordered = record != nullptr ? record->uav.Get() : nullptr;
            deferred_->CSSetUnorderedAccessViews(location, 1, &unordered, nullptr);
            continue;
        }
        }

        if (binding.role == PipelineSlotRole::Sampler) {
            switch (binding.stage) {
            case ShaderStage::Vertex:
                deferred_->VSSetSamplers(location, 1, &sampler);
                break;
            case ShaderStage::Pixel:
                deferred_->PSSetSamplers(location, 1, &sampler);
                break;
            case ShaderStage::Compute:
                deferred_->CSSetSamplers(location, 1, &sampler);
                break;
            }

            continue;
        }

        switch (binding.stage) {
        case ShaderStage::Vertex:
            deferred_->VSSetShaderResources(location, 1, &view);
            break;
        case ShaderStage::Pixel:
            deferred_->PSSetShaderResources(location, 1, &view);
            break;
        case ShaderStage::Compute:
            deferred_->CSSetShaderResources(location, 1, &view);
            break;
        }
    }
}

bool D3D11Device::PrepareDraw(bool indexed) noexcept {
    const PipelineRecord* pipeline = pipelines_.Find(boundPipeline_);
    if (!recording_ || pipeline == nullptr || pipeline->compute ||
        (target_.colorCount == 0 && target_.depth.IsNull()) || !StreamsBound() ||
        !BufferViewsBound() || (target_.depthReadOnly && pipeline->writesDepthStencil)) {
        return false;
    }

    const BufferRecord* index = buffers_.Find(boundIndex_.buffer.slot);
    if (indexed && (index == nullptr || boundIndex_.offset >= index->size)) {
        return false;
    }

    // dispatch가 같은 버퍼를 UAV로 묶었다면 런타임이 입력 binding을 풀었으므로 다시 묶는다.
    if (inputsDirty_) {
        for (uint32 stream = 0; stream < MaxVertexStreams; ++stream) {
            if (!boundVertex_[stream].buffer.IsNull()) {
                ApplyVertexBuffer(stream);
            }
        }

        if (index != nullptr && boundIndex_.offset < index->size) {
            deferred_->IASetIndexBuffer(index->buffer.Get(),
                                        boundIndex_.format == IndexFormat::UInt16
                                            ? DXGI_FORMAT_R16_UINT
                                            : DXGI_FORMAT_R32_UINT,
                                        static_cast<UINT>(boundIndex_.offset));
        }

        inputsDirty_ = false;
    }

    ApplySlots(*pipeline);
    return true;
}

bool D3D11Device::PrepareDispatch() noexcept {
    const PipelineRecord* pipeline = pipelines_.Find(boundPipeline_);
    if (!recording_ || pipeline == nullptr || !pipeline->compute || !BufferViewsBound()) {
        return false;
    }

    BoundBufferViewInfo written[MaxDrawBufferViews] = {};
    ID3D11UnorderedAccessView* uavs[MaxDrawBufferViews] = {};
    for (uint32 slot = 0; slot < MaxDrawBufferViews; ++slot) {
        const BufferViewRecord* view = bufferViews_.Find(boundWriteViews_[slot].slot);
        if (view != nullptr) {
            written[slot] = {true, view->kind, view->shaderType, view->stride};
            uavs[slot] = view->uav.Get();
        }
    }

    if (!DrawBufferSlotsSatisfied(pipeline->writeSlots, written)) {
        return false;
    }

    // 텍스처로 쓰는 slot마다 쓰기 텍스처 view가 묶여야 한다.
    for (uint32 slot = 0; slot < MaxDrawBufferViews; ++slot) {
        if ((pipeline->writeTextures & (1u << slot)) == 0) {
            continue;
        }

        const TextureRecord* view = textures_.Find(boundWriteTextures_[slot].slot);
        if (view == nullptr) {
            return false;
        }

        uavs[slot] = view->uav.Get();
    }

    // 그래픽 단계의 SRV를 먼저 풀어 같은 자원을 UAV로 묶을 때의 hazard를 없앤다. 다음 그리기가
    // 다시 묶는다(inputsDirty_).
    ID3D11ShaderResourceView* none[MaxBoundShaderResources] = {};
    deferred_->VSSetShaderResources(0, MaxBoundShaderResources, none);
    deferred_->PSSetShaderResources(0, MaxBoundShaderResources, none);
    ApplySlots(*pipeline);
    inputsDirty_ = true;
    return true;
}

void D3D11Device::FinishDispatch() noexcept {
    ID3D11UnorderedAccessView* noUavs[MaxBoundUnorderedViews] = {};
    ID3D11ShaderResourceView* noViews[MaxBoundShaderResources] = {};
    deferred_->CSSetUnorderedAccessViews(0, MaxBoundUnorderedViews, noUavs, nullptr);
    deferred_->CSSetShaderResources(0, MaxBoundShaderResources, noViews);
}

ID3D11Buffer* D3D11Device::IndirectArguments(BufferHandle arguments, uint64 offset,
                                             uint64 bytes) noexcept {
    const BufferRecord* record = buffers_.Find(arguments.slot);
    if (record == nullptr || !HasAnyBufferUsage(record->desc.usage, BufferUsage::Indirect) ||
        offset % 4 != 0 || offset > record->size || record->size - offset < bytes) {
        return nullptr;
    }

    return record->buffer.Get();
}

void D3D11Device::Dispatch(uint32 groupCountX, uint32 groupCountY, uint32 groupCountZ) noexcept {
    platform::LockGuard lock(gate_);
    const uint32 counts[3] = {groupCountX, groupCountY, groupCountZ};
    for (uint32 axis = 0; axis < 3; ++axis) {
        if (counts[axis] > caps_.limits.computeGroupCount[axis]) {
            ++ignoredDraws_;
            return;
        }
    }

    if (!PrepareDispatch()) {
        ++ignoredDraws_;
        return;
    }

    deferred_->Dispatch(groupCountX, groupCountY, groupCountZ);
    FinishDispatch();
}

void D3D11Device::DispatchIndirect(BufferHandle arguments, uint64 offset) noexcept {
    platform::LockGuard lock(gate_);
    ID3D11Buffer* buffer = IndirectArguments(arguments, offset, sizeof(DispatchIndirectArguments));
    if (buffer == nullptr || !PrepareDispatch()) {
        ++ignoredDraws_;
        return;
    }

    deferred_->DispatchIndirect(buffer, static_cast<UINT>(offset));
    FinishDispatch();
}

void D3D11Device::DrawIndirect(BufferHandle arguments, uint64 offset) noexcept {
    platform::LockGuard lock(gate_);
    ID3D11Buffer* buffer = IndirectArguments(arguments, offset, sizeof(DrawIndirectArguments));
    if (buffer == nullptr || !PrepareDraw(false)) {
        ++ignoredDraws_;
        return;
    }

    deferred_->DrawInstancedIndirect(buffer, static_cast<UINT>(offset));
}

void D3D11Device::DrawIndexedIndirect(BufferHandle arguments, uint64 offset) noexcept {
    platform::LockGuard lock(gate_);
    ID3D11Buffer* buffer =
        IndirectArguments(arguments, offset, sizeof(DrawIndexedIndirectArguments));
    if (buffer == nullptr || !PrepareDraw(true)) {
        ++ignoredDraws_;
        return;
    }

    deferred_->DrawIndexedInstancedIndirect(buffer, static_cast<UINT>(offset));
}

void D3D11Device::SetViewport(const Viewport& viewport) noexcept {
    if (viewport.width <= 0.0f || viewport.height <= 0.0f) {
        return;
    }

    D3D11_VIEWPORT value = {};
    value.TopLeftX = viewport.x;
    value.TopLeftY = viewport.y;
    value.Width = viewport.width;
    value.Height = viewport.height;
    value.MinDepth = viewport.minDepth;
    value.MaxDepth = viewport.maxDepth;
    deferred_->RSSetViewports(1, &value);
}

void D3D11Device::SetScissor(const ScissorRect& scissor) noexcept {
    const D3D11_RECT rect = {static_cast<LONG>(scissor.x), static_cast<LONG>(scissor.y),
                             static_cast<LONG>(scissor.x + scissor.width),
                             static_cast<LONG>(scissor.y + scissor.height)};
    deferred_->RSSetScissorRects(1, &rect);
}

void D3D11Device::SetStencilReference(uint32 reference) noexcept {
    platform::LockGuard lock(gate_);
    stencilReference_ = reference;
    // D3D11은 기준 값을 깊이·스텐실 상태와 함께 건다. 그래픽 파이프라인이 없으면 SetPipeline이
    // 건다.
    const PipelineRecord* pipeline = pipelines_.Find(boundPipeline_);
    if (pipeline != nullptr && !pipeline->compute) {
        deferred_->OMSetDepthStencilState(pipeline->depthStencil.Get(), reference);
    }
}

Result<void> D3D11Device::CopyBuffer(BufferHandle destination, uint64 destinationOffset,
                                     BufferHandle source, uint64 sourceOffset,
                                     uint64 size) noexcept {
    platform::LockGuard lock(gate_);
    if (!recording_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const BufferRecord* to = buffers_.Find(destination.slot);
    const BufferRecord* from = buffers_.Find(source.slot);
    if (to == nullptr || from == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (size == 0 || destination == source || destinationOffset % 4 != 0 || sourceOffset % 4 != 0 ||
        size % 4 != 0 || destinationOffset > to->size || to->size - destinationOffset < size ||
        sourceOffset > from->size || from->size - sourceOffset < size) {
        auto failed = Fail("buffer copy range is empty, out of range, unaligned or within one "
                           "buffer");
        (void)failed;
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const D3D11_BOX box = {static_cast<UINT>(sourceOffset),        0, 0,
                           static_cast<UINT>(sourceOffset + size), 1, 1};
    deferred_->CopySubresourceRegion(to->buffer.Get(), 0, static_cast<UINT>(destinationOffset), 0,
                                     0, from->buffer.Get(), 0, &box);
    return Result<void>::Success();
}

Result<void> D3D11Device::CopyTexture(TextureHandle destination, TextureHandle source) noexcept {
    platform::LockGuard lock(gate_);
    if (!recording_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const TextureRecord* to = textures_.Find(destination.slot);
    const TextureRecord* from = textures_.Find(source.slot);
    if (to == nullptr || from == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    const auto attached = [this](TextureHandle handle) noexcept {
        for (uint32 index = 0; index < target_.colorCount; ++index) {
            if (target_.colors[index] == handle) {
                return true;
            }
        }

        return handle == target_.depth;
    };
    const PixelFormatInfo& info = GetPixelFormatInfo(from->format);
    if (destination == source || to->view || from->view || to->width != from->width ||
        to->height != from->height || to->format != from->format || info.planeCount > 1 ||
        to->sampleCount > 1 || from->sampleCount > 1 ||
        to->dimension != TextureDimension::Texture2D ||
        from->dimension != TextureDimension::Texture2D || attached(destination) ||
        attached(source)) {
        auto failed = Fail("texture copy needs two distinct non-view single-plane 2D textures of "
                           "the same size and format outside the current pass");
        (void)failed;
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    deferred_->CopySubresourceRegion(to->texture.Get(), 0, 0, 0, 0, from->texture.Get(), 0,
                                     nullptr);
    return Result<void>::Success();
}

Result<void> D3D11Device::ClearWritableBufferView(BufferViewHandle view, uint32 value) noexcept {
    platform::LockGuard lock(gate_);
    if (!recording_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const BufferViewRecord* record = bufferViews_.Find(view.slot);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (record->access != ViewAccess::ReadWrite) {
        auto failed = Fail("only writable views can be cleared");
        (void)failed;
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const bool clearable =
        record->kind == BufferViewKind::Raw ||
        (record->kind == BufferViewKind::Typed &&
         (record->format == PixelFormat::R32Uint || record->format == PixelFormat::R32Sint));
    if (!clearable) {
        auto failed = Fail("only raw and R32Uint or R32Sint typed views can be cleared");
        (void)failed;
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    const UINT values[4] = {value, value, value, value};
    deferred_->ClearUnorderedAccessViewUint(record->uav.Get(), values);
    return Result<void>::Success();
}

} // namespace rocks::graphics::d3d11
