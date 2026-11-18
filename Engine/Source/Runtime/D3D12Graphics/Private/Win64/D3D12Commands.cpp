#include "D3D12DeviceImpl.h"

// GX3·GX4 명령 기록이다: 패스 attachment(BeginPass), compute 파이프라인·dispatch, 간접 그리기·
// dispatch, viewport·scissor·스텐실 기준, 버퍼·텍스처 복사, UAV clear와 기록 중 버퍼 상태 추적.
namespace rocks::graphics::d3d12 {

namespace {

// compute root signature 매개변수 번호다: 그리기 상수(b0), 읽기 버퍼 view slot table(t1–t4), 쓰기
// 버퍼 view slot table(u0–u3).
constexpr UINT ComputeRootConstants = 0;
constexpr UINT ComputeRootReads = 1;
constexpr UINT ComputeRootWrites = ComputeRootReads + MaxDrawBufferViews;
// SetTexture의 텍스처(t0)·샘플러(s0) table이다(텍스처 GX2 A부).
constexpr UINT ComputeRootTexture = ComputeRootWrites + MaxDrawBufferViews;
constexpr UINT ComputeRootSampler = ComputeRootTexture + 1;
constexpr UINT ComputeRootCount = ComputeRootSampler + 1;

// 읽기 전용 상태 묶음이다(같은 기록 안에서 서로 합칠 수 있다).
constexpr D3D12_RESOURCE_STATES ReadOnlyStates =
    D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER |
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
    D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT | D3D12_RESOURCE_STATE_COPY_SOURCE;

bool IsReadOnlyState(D3D12_RESOURCE_STATES state) noexcept {
    return state != D3D12_RESOURCE_STATE_COMMON && (state & ~ReadOnlyStates) == 0;
}

} // namespace

Result<void> D3D12Device::CreateComputeObjects() noexcept {
    D3D12_DESCRIPTOR_RANGE readRanges[MaxDrawBufferViews] = {};
    D3D12_DESCRIPTOR_RANGE writeRanges[MaxDrawBufferViews] = {};
    D3D12_ROOT_PARAMETER parameters[ComputeRootCount] = {};
    D3D12_DESCRIPTOR_RANGE textureRange = {};
    textureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    textureRange.NumDescriptors = 1;
    D3D12_DESCRIPTOR_RANGE samplerRange = {};
    samplerRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    samplerRange.NumDescriptors = 1;
    parameters[ComputeRootTexture].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[ComputeRootTexture].DescriptorTable.NumDescriptorRanges = 1;
    parameters[ComputeRootTexture].DescriptorTable.pDescriptorRanges = &textureRange;
    parameters[ComputeRootTexture].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[ComputeRootSampler].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[ComputeRootSampler].DescriptorTable.NumDescriptorRanges = 1;
    parameters[ComputeRootSampler].DescriptorTable.pDescriptorRanges = &samplerRange;
    parameters[ComputeRootSampler].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[ComputeRootConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[ComputeRootConstants].Constants.ShaderRegister = 0;
    parameters[ComputeRootConstants].Constants.Num32BitValues = ConstantCount;
    parameters[ComputeRootConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // slot마다 table 하나라 view의 descriptor 칸을 그대로 가리킨다(그래픽 root signature와 같은
    // 방식, GX6 전 고정 배치).
    for (uint32 slot = 0; slot < MaxDrawBufferViews; ++slot) {
        readRanges[slot].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        readRanges[slot].NumDescriptors = 1;
        readRanges[slot].BaseShaderRegister = slot + 1;
        writeRanges[slot].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        writeRanges[slot].NumDescriptors = 1;
        writeRanges[slot].BaseShaderRegister = slot;
        D3D12_ROOT_PARAMETER& read = parameters[ComputeRootReads + slot];
        read.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        read.DescriptorTable.NumDescriptorRanges = 1;
        read.DescriptorTable.pDescriptorRanges = &readRanges[slot];
        read.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_PARAMETER& write = parameters[ComputeRootWrites + slot];
        write.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        write.DescriptorTable.NumDescriptorRanges = 1;
        write.DescriptorTable.pDescriptorRanges = &writeRanges[slot];
        write.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = ComputeRootCount;
    desc.pParameters = parameters;
    ComRef<ID3DBlob> blob;
    ComRef<ID3DBlob> errors;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, blob.Out(),
                                           errors.Out())) ||
        FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                            IID_PPV_ARGS(computeRootSignature_.Out())))) {
        return Fail("compute root signature creation failed");
    }

    // 간접 명령 signature다. 인자 하나만 담으므로 root signature가 필요 없다.
    const auto makeSignature = [this](D3D12_INDIRECT_ARGUMENT_TYPE type, UINT stride,
                                      ComRef<ID3D12CommandSignature>& out) noexcept {
        D3D12_INDIRECT_ARGUMENT_DESC argument = {};
        argument.Type = type;
        D3D12_COMMAND_SIGNATURE_DESC signature = {};
        signature.ByteStride = stride;
        signature.NumArgumentDescs = 1;
        signature.pArgumentDescs = &argument;
        return SUCCEEDED(
            device_->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(out.Out())));
    };
    if (!makeSignature(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW, sizeof(DrawIndirectArguments),
                       drawSignature_) ||
        !makeSignature(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED,
                       sizeof(DrawIndexedIndirectArguments), drawIndexedSignature_) ||
        !makeSignature(D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH, sizeof(DispatchIndirectArguments),
                       dispatchSignature_)) {
        return Fail("indirect command signature creation failed");
    }

    // 쓰기 slot이 비었을 때 묶는 null UAV와, ReadWrite view의 CPU UAV 칸 pool, 읽기 전용 DSV
    // 칸이다.
    auto nullUav = srvs_.Allocate();
    if (!nullUav) {
        return Fail("null buffer UAV descriptor allocation failed");
    }

    nullBufferUav_ = nullUav.Value();
    D3D12_UNORDERED_ACCESS_VIEW_DESC nullDesc = {};
    nullDesc.Format = DXGI_FORMAT_R32_UINT;
    nullDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    device_->CreateUnorderedAccessView(nullptr, nullptr, &nullDesc, srvs_.Cpu(nullBufferUav_));
    auto cpuUavs = cpuUavs_.Initialize(device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                       SrvCapacity, false);
    if (!cpuUavs) {
        return Fail("CPU UAV descriptor heap creation failed");
    }

    auto scratch = dsvs_.Allocate();
    if (!scratch) {
        return Fail("read-only DSV descriptor allocation failed");
    }

    scratchDsv_ = scratch.Value();
    for (uint32& rtv : scratchRtvs_) {
        auto allocated = rtvs_.Allocate();
        if (!allocated) {
            return Fail("pass RTV descriptor allocation failed");
        }

        rtv = allocated.Value();
    }

    return Result<void>::Success();
}

Result<PipelineHandle>
D3D12Device::CreateComputePipeline(const ComputePipelineDesc& desc) noexcept {
    platform::LockGuard lock(gate_);
    if (desc.shader.hlsl.ByteCount() == 0 || !ValidateComputePipelineDesc(desc)) {
        return FailCode<PipelineHandle>(
            ErrorCode::InvalidArgument,
            "compute pipeline needs HLSL source and valid drawConstantBytes");
    }

    ComRef<ID3DBlob> code;
    auto compiled = Compile(desc.shader.hlsl, "CSMain", "cs_5_1", code);
    if (!compiled) {
        return Result<PipelineHandle>::Failure(compiled.Error());
    }

    DrawBufferSlots readSlots;
    DrawBufferSlots writeSlots;
    uint32 writeTextures = 0;
    auto reflected = ReflectBindings(code.Get(), ShaderStage::Compute, desc.drawConstantBytes,
                                     readSlots, &writeSlots, &writeTextures);
    if (!reflected) {
        return Result<PipelineHandle>::Failure(reflected.Error());
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC state = {};
    state.pRootSignature = computeRootSignature_.Get();
    state.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    PipelineRecord record;
    const HRESULT created =
        device_->CreateComputePipelineState(&state, IID_PPV_ARGS(record.state.Out()));
    if (FAILED(created)) {
        return FailWith<PipelineHandle>("ID3D12Device::CreateComputePipelineState", created);
    }

    record.bufferSlots = readSlots;
    record.writeSlots = writeSlots;
    record.compute = true;
    record.drawConstantBytes = desc.drawConstantBytes;
    record.writeTextures = writeTextures;
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

Result<CommandContext*> D3D12Device::BeginPass(const PassDesc& pass) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<CommandContext*>::Failure(owner.Error());
    }

    platform::LockGuard lock(gate_);

    if (pass.colorCount > MaxColorTargets) {
        return FailCode<CommandContext*>(ErrorCode::InvalidArgument,
                                         "pass has more color attachments than MaxColorTargets");
    }

    if (pass.colorCount > caps_.limits.colorAttachments) {
        return FailCode<CommandContext*>(
            ErrorCode::Unsupported, "pass has more color attachments than the device supports");
    }

    // attachment 확인: 살아 있는 텍스처, view 아님, 용도, 같은 크기·표본 수, 중복 없음.
    uint32 width = 0;
    uint32 height = 0;
    uint32 samples = 1;
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
        if (sized && (mipWidth != width || mipHeight != height || record->sampleCount != samples)) {
            return ErrorCode::InvalidArgument;
        }

        width = mipWidth;
        height = mipHeight;
        samples = record->sampleCount;
        sized = true;
        return ErrorCode::None;
    };
    for (uint32 index = 0; index < pass.colorCount; ++index) {
        const ErrorCode checked = check(pass.colors[index].texture, false,
                                        pass.colors[index].mipLevel, pass.colors[index].slice);
        if (checked != ErrorCode::None) {
            return FailCode<CommandContext*>(checked, "color attachment is stale, not a render "
                                                      "target, a view or of a different size");
        }

        for (uint32 other = 0; other < index; ++other) {
            if (pass.colors[other].texture == pass.colors[index].texture &&
                pass.colors[other].mipLevel == pass.colors[index].mipLevel &&
                pass.colors[other].slice == pass.colors[index].slice) {
                return FailCode<CommandContext*>(ErrorCode::InvalidArgument,
                                                 "color attachment appears twice in the pass");
            }
        }
    }

    const bool hasDepth = !pass.depth.texture.IsNull();
    if (hasDepth) {
        const ErrorCode checked =
            check(pass.depth.texture, true, pass.depth.mipLevel, pass.depth.slice);
        if (checked != ErrorCode::None) {
            return FailCode<CommandContext*>(checked, "depth attachment is stale, not a depth "
                                                      "target, a view or of a different size");
        }
    }

    // resolve 대상: MSAA attachment의 1표본 RenderTarget, 같은 형식·크기, view 아님, 겹침 없음.
    for (uint32 index = 0; index < pass.colorCount; ++index) {
        const TextureHandle resolve = pass.colors[index].resolveTarget;
        if (resolve.IsNull()) {
            continue;
        }

        const TextureRecord* source = textures_.Find(pass.colors[index].texture.slot);
        const TextureRecord* target = textures_.Find(resolve.slot);
        if (target == nullptr) {
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
        if (source->sampleCount < 2 || target->view || !target->renderTarget ||
            target->sampleCount != 1 || target->format != source->format ||
            target->dimension == TextureDimension::Texture3D ||
            !IsAttachmentInRange(target->dimension, target->depth, target->mipLevels,
                                 target->slices, resolveMip, resolveSlice) ||
            MipExtent(target->width, resolveMip) != source->width ||
            MipExtent(target->height, resolveMip) != source->height || overlaps) {
            return FailCode<CommandContext*>(
                ErrorCode::InvalidArgument,
                "resolve target needs an MSAA color attachment and a distinct single-sample "
                "render target of the same format and size");
        }
    }

    if (recording_) {
        return Result<CommandContext*>::Failure(ErrorCode::Busy);
    }

    if (timeline_.IsLost()) {
        return Result<CommandContext*>::Failure(ErrorCode::Destroyed);
    }

    auto begun = BeginRecording();
    if (!begun) {
        return Result<CommandContext*>::Failure(begun.Error());
    }

    target_ = {};
    target_.width = width;
    target_.height = height;
    target_.sampleCount = samples;
    target_.colorCount = pass.colorCount;
    for (uint32 index = 0; index < pass.colorCount; ++index) {
        const TextureRecord* record = textures_.Find(pass.colors[index].texture.slot);
        target_.colors[index] = pass.colors[index].texture;
        target_.resolves[index] = pass.colors[index].resolveTarget;
        target_.colorMips[index] = pass.colors[index].mipLevel;
        target_.colorSlices[index] = pass.colors[index].slice;
        target_.resolveMips[index] = pass.colors[index].resolveMipLevel;
        target_.resolveSlices[index] = pass.colors[index].resolveSlice;
        // RTV는 패스마다 그 mip·slice로 scratch 칸에 쓴다.
        WriteRenderTargetView(*record, pass.colors[index].mipLevel, pass.colors[index].slice,
                              rtvs_.Cpu(scratchRtvs_[index]));
        target_.rtvs[index] = rtvs_.Cpu(scratchRtvs_[index]);
    }

    if (pass.colorCount != 0) {
        const TextureRecord* first = textures_.Find(pass.colors[0].texture.slot);
        target_.resource = first->resource.Get();
        target_.rtv = target_.rtvs[0];
        target_.format = first->format;
        target_.texture = pass.colors[0].texture;
    }

    if (hasDepth) {
        const TextureRecord* depth = textures_.Find(pass.depth.texture.slot);
        target_.depth = pass.depth.texture;
        target_.depthReadOnly = pass.depth.readOnly;
        target_.depthMip = pass.depth.mipLevel;
        target_.depthSlice = pass.depth.slice;
        // DSV는 칸 하나를 패스마다 그 mip·slice·읽기 전용 여부로 다시 쓴다(OMSetRenderTargets가
        // 기록 때 descriptor를 읽는다).
        WriteDepthStencilView(*depth, pass.depth.mipLevel, pass.depth.slice, pass.depth.readOnly,
                              dsvs_.Cpu(scratchDsv_));
        target_.dsv = dsvs_.Cpu(scratchDsv_);
    }

    presentTarget_ = {};
    StartPass(pass);
    return Result<CommandContext*>::Success(this);
}

void D3D12Device::SetWritableBufferView(uint32 slot, BufferViewHandle view) noexcept {
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

void D3D12Device::SetWritableTexture(uint32 slot, TextureHandle view) noexcept {
    platform::LockGuard lock(gate_);
    if (slot >= MaxDrawBufferViews) {
        return;
    }

    const TextureRecord* record = textures_.Find(view.slot);
    const bool writable = record != nullptr && record->writable;
    boundWriteTextures_[slot] = writable ? view : TextureHandle{};
    boundWriteViews_[slot] = {};
}

void D3D12Device::RequireBufferState(ID3D12Resource* resource,
                                     D3D12_RESOURCE_STATES needed) noexcept {
    BufferState* entry = nullptr;
    for (BufferState& state : bufferStates_) {
        if (state.resource == resource) {
            entry = &state;
            break;
        }
    }

    if (entry == nullptr) {
        // 기록 안 첫 사용이다. 버퍼는 COMMON에서 어떤 상태로든 암묵 승격된다.
        auto pushed = bufferStates_.TryPushBack(BufferState{resource, needed});
        if (!pushed) {
            // 추적할 수 없으면 COMMON에서 명시 전이하고 다음 사용도 같은 길로 간다.
            const D3D12_RESOURCE_BARRIER barrier =
                TransitionBarrier(resource, D3D12_RESOURCE_STATE_COMMON, needed);
            if (needed != D3D12_RESOURCE_STATE_COMMON) {
                list_->ResourceBarrier(1, &barrier);
            }
        }

        return;
    }

    const D3D12_RESOURCE_STATES current = entry->state;
    if (current == needed) {
        if (needed == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            const D3D12_RESOURCE_BARRIER barrier = UavBarrier(resource);
            list_->ResourceBarrier(1, &barrier);
        }

        return;
    }

    if (IsReadOnlyState(current) && IsReadOnlyState(needed)) {
        if ((current & needed) == needed) {
            return;
        }

        // 읽기끼리는 합친 상태로 옮긴다(앞선 읽기를 깨지 않는다).
        const D3D12_RESOURCE_STATES merged = current | needed;
        const D3D12_RESOURCE_BARRIER barrier = TransitionBarrier(resource, current, merged);
        list_->ResourceBarrier(1, &barrier);
        entry->state = merged;
        return;
    }

    const D3D12_RESOURCE_BARRIER barrier = TransitionBarrier(resource, current, needed);
    list_->ResourceBarrier(1, &barrier);
    entry->state = needed;
}

bool D3D12Device::PrepareDraw(bool indexed) noexcept {
    const PipelineRecord* pipeline = pipelines_.Find(boundPipeline_);
    if (!recording_ || pipeline == nullptr || pipeline->compute ||
        (target_.colorCount == 0 && target_.depth.IsNull()) || !StreamsBound() ||
        !BufferViewsBound() || (target_.depthReadOnly && pipeline->writesDepthStencil)) {
        return false;
    }

    const BufferRecord* index = nullptr;
    if (indexed) {
        index = buffers_.Find(boundIndex_.buffer.slot);
        if (index == nullptr || boundIndex_.offset >= index->size) {
            return false;
        }
    }

    // 묶인 버퍼의 상태를 맞춘다(GPU가 앞서 쓴 버퍼를 정점·index·SRV로 읽는다).
    for (uint32 stream = 0; stream < MaxVertexStreams; ++stream) {
        if ((pipeline->streamMask & (1u << stream)) != 0) {
            const BufferRecord* buffer = buffers_.Find(boundVertex_[stream].buffer.slot);
            RequireBufferState(buffer->resource.Get(),
                               D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        }
    }

    if (index != nullptr) {
        RequireBufferState(index->resource.Get(), D3D12_RESOURCE_STATE_INDEX_BUFFER);
    }

    for (uint32 slot = 0; slot < MaxDrawBufferViews; ++slot) {
        if ((pipeline->bufferSlots.usedMask & (1u << slot)) != 0) {
            const BufferViewRecord* view = bufferViews_.Find(boundViews_[slot].slot);
            RequireBufferState(view->resource.Get(), ShaderReadStates);
        }
    }

    return true;
}

bool D3D12Device::PrepareDispatch() noexcept {
    const PipelineRecord* pipeline = pipelines_.Find(boundPipeline_);
    if (!recording_ || pipeline == nullptr || !pipeline->compute || !BufferViewsBound()) {
        return false;
    }

    BoundBufferViewInfo written[MaxDrawBufferViews] = {};
    for (uint32 slot = 0; slot < MaxDrawBufferViews; ++slot) {
        const BufferViewRecord* view = bufferViews_.Find(boundWriteViews_[slot].slot);
        if (view != nullptr) {
            written[slot] = {true, view->kind, view->shaderType, view->stride};
        }
    }

    if (!DrawBufferSlotsSatisfied(pipeline->writeSlots, written)) {
        return false;
    }

    // 텍스처로 쓰는 slot마다 쓰기 텍스처 view가 묶여야 한다.
    for (uint32 slot = 0; slot < MaxDrawBufferViews; ++slot) {
        if ((pipeline->writeTextures & (1u << slot)) != 0 &&
            textures_.Find(boundWriteTextures_[slot].slot) == nullptr) {
            return false;
        }
    }

    // 쓰는 텍스처 view의 범위를 UAV 상태로 맞춘다(같은 상태가 이어지면 UAV barrier로 앞 dispatch의
    // 쓰기를 기다린다).
    for (uint32 slot = 0; slot < MaxDrawBufferViews; ++slot) {
        const TextureRecord* view = (pipeline->writeTextures & (1u << slot)) != 0
                                        ? textures_.Find(boundWriteTextures_[slot].slot)
                                        : nullptr;
        TextureRecord* owner = view != nullptr ? textures_.Find(view->source) : nullptr;
        if (owner != nullptr) {
            TrackTransition(*owner, view->source, view->viewRange,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        }
    }

    // 읽는 view는 셰이더 읽기, 쓰는 view는 UAV 상태로 맞추고 compute root 인자를 건다.
    list_->SetComputeRootSignature(computeRootSignature_.Get());
    const TextureRecord* texture = textures_.Find(boundTexture_);
    if (texture == nullptr) {
        texture = textures_.Find(white_.slot);
    }

    if (texture != nullptr) {
        list_->SetComputeRootDescriptorTable(ComputeRootTexture, srvs_.Gpu(texture->srv));
        list_->SetComputeRootDescriptorTable(ComputeRootSampler, samplers_.Gpu(texture->sampler));
    }
    if (pipeline->drawConstantBytes != 0) {
        list_->SetComputeRoot32BitConstants(
            ComputeRootConstants, static_cast<UINT>(pipeline->drawConstantBytes / sizeof(uint32)),
            constants_, 0);
    }

    for (uint32 slot = 0; slot < MaxDrawBufferViews; ++slot) {
        const BufferViewRecord* read = bufferViews_.Find(boundViews_[slot].slot);
        if (read != nullptr && (pipeline->bufferSlots.usedMask & (1u << slot)) != 0) {
            RequireBufferState(read->resource.Get(), ShaderReadStates);
        }

        list_->SetComputeRootDescriptorTable(
            ComputeRootReads + slot,
            srvs_.Gpu(read != nullptr ? read->descriptor : nullBufferSrv_));
        const BufferViewRecord* write = bufferViews_.Find(boundWriteViews_[slot].slot);
        if (write != nullptr && (pipeline->writeSlots.usedMask & (1u << slot)) != 0) {
            RequireBufferState(write->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        const TextureRecord* writeTexture = (pipeline->writeTextures & (1u << slot)) != 0
                                                ? textures_.Find(boundWriteTextures_[slot].slot)
                                                : nullptr;
        uint32 descriptor = write != nullptr ? write->descriptor : nullBufferUav_;
        if (writeTexture != nullptr) {
            descriptor = writeTexture->srv;
        }

        list_->SetComputeRootDescriptorTable(ComputeRootWrites + slot, srvs_.Gpu(descriptor));
    }

    return true;
}

ID3D12Resource* D3D12Device::IndirectArguments(BufferHandle arguments, uint64 offset,
                                               uint64 bytes) noexcept {
    const BufferRecord* record = buffers_.Find(arguments.slot);
    if (record == nullptr || !HasAnyBufferUsage(record->desc.usage, BufferUsage::Indirect) ||
        offset % 4 != 0 || offset > record->size || record->size - offset < bytes) {
        return nullptr;
    }

    return record->resource.Get();
}

void D3D12Device::Dispatch(uint32 groupCountX, uint32 groupCountY, uint32 groupCountZ) noexcept {
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

    list_->Dispatch(groupCountX, groupCountY, groupCountZ);
}

void D3D12Device::DispatchIndirect(BufferHandle arguments, uint64 offset) noexcept {
    platform::LockGuard lock(gate_);
    ID3D12Resource* resource =
        IndirectArguments(arguments, offset, sizeof(DispatchIndirectArguments));
    if (resource == nullptr || !PrepareDispatch()) {
        ++ignoredDraws_;
        return;
    }

    // 인자 상태는 view 상태를 맞춘 뒤에 맞춘다(인자 버퍼를 쓰기 view로 함께 묶으면 D3D12 검증이
    // 잡는다).
    RequireBufferState(resource, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    list_->ExecuteIndirect(dispatchSignature_.Get(), 1, resource, offset, nullptr, 0);
}

void D3D12Device::DrawIndirect(BufferHandle arguments, uint64 offset) noexcept {
    platform::LockGuard lock(gate_);
    ID3D12Resource* resource = IndirectArguments(arguments, offset, sizeof(DrawIndirectArguments));
    if (resource == nullptr || !PrepareDraw(false)) {
        ++ignoredDraws_;
        return;
    }

    RequireBufferState(resource, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    list_->ExecuteIndirect(drawSignature_.Get(), 1, resource, offset, nullptr, 0);
}

void D3D12Device::DrawIndexedIndirect(BufferHandle arguments, uint64 offset) noexcept {
    platform::LockGuard lock(gate_);
    ID3D12Resource* resource =
        IndirectArguments(arguments, offset, sizeof(DrawIndexedIndirectArguments));
    if (resource == nullptr || !PrepareDraw(true)) {
        ++ignoredDraws_;
        return;
    }

    RequireBufferState(resource, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    list_->ExecuteIndirect(drawIndexedSignature_.Get(), 1, resource, offset, nullptr, 0);
}

void D3D12Device::SetViewport(const Viewport& viewport) noexcept {
    if (viewport.width <= 0.0f || viewport.height <= 0.0f) {
        return;
    }

    D3D12_VIEWPORT value = {};
    value.TopLeftX = viewport.x;
    value.TopLeftY = viewport.y;
    value.Width = viewport.width;
    value.Height = viewport.height;
    value.MinDepth = viewport.minDepth;
    value.MaxDepth = viewport.maxDepth;
    list_->RSSetViewports(1, &value);
}

void D3D12Device::SetScissor(const ScissorRect& scissor) noexcept {
    const D3D12_RECT rect = {static_cast<LONG>(scissor.x), static_cast<LONG>(scissor.y),
                             static_cast<LONG>(scissor.x + scissor.width),
                             static_cast<LONG>(scissor.y + scissor.height)};
    list_->RSSetScissorRects(1, &rect);
}

void D3D12Device::SetStencilReference(uint32 reference) noexcept {
    stencilReference_ = reference;
    list_->OMSetStencilRef(reference);
}

Result<void> D3D12Device::CopyBuffer(BufferHandle destination, uint64 destinationOffset,
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
        return FailCode<void>(ErrorCode::InvalidArgument,
                              "buffer copy range is empty, out of range, unaligned or within "
                              "one buffer");
    }

    RequireBufferState(from->resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    RequireBufferState(to->resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
    list_->CopyBufferRegion(to->resource.Get(), destinationOffset, from->resource.Get(),
                            sourceOffset, size);
    return Result<void>::Success();
}

Result<void> D3D12Device::CopyTexture(TextureHandle destination, TextureHandle source) noexcept {
    platform::LockGuard lock(gate_);
    if (!recording_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    TextureRecord* to = textures_.Find(destination.slot);
    TextureRecord* from = textures_.Find(source.slot);
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
    if (destination == source || to->view || from->view || to->width != from->width ||
        to->height != from->height || to->format != from->format || to->devicePlanes != 1 ||
        to->sampleCount > 1 || from->sampleCount > 1 ||
        to->dimension != TextureDimension::Texture2D ||
        from->dimension != TextureDimension::Texture2D || attached(destination) ||
        attached(source)) {
        return FailCode<void>(ErrorCode::InvalidArgument,
                              "texture copy needs two distinct non-view single-plane 2D textures "
                              "of the same size and format outside the current pass");
    }

    const ResolvedSubresourceRange top{0, 1, 0, 1};
    TrackTransition(*from, source.slot, top, D3D12_RESOURCE_STATE_COPY_SOURCE);
    TrackTransition(*to, destination.slot, top, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION target = {};
    target.pResource = to->resource.Get();
    target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    target.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION origin = {};
    origin.pResource = from->resource.Get();
    origin.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    origin.SubresourceIndex = 0;
    list_->CopyTextureRegion(&target, 0, 0, 0, &origin, nullptr);
    return Result<void>::Success();
}

Result<void> D3D12Device::ClearWritableBufferView(BufferViewHandle view, uint32 value) noexcept {
    platform::LockGuard lock(gate_);
    if (!recording_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const BufferViewRecord* record = bufferViews_.Find(view.slot);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (record->access != ViewAccess::ReadWrite) {
        return FailCode<void>(ErrorCode::InvalidArgument, "only writable views can be cleared");
    }

    if (record->cpuDescriptor < 0) {
        return FailCode<void>(ErrorCode::Unsupported,
                              "only raw and R32Uint or R32Sint typed views can be cleared");
    }

    RequireBufferState(record->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const UINT values[4] = {value, value, value, value};
    list_->ClearUnorderedAccessViewUint(srvs_.Gpu(record->descriptor),
                                        cpuUavs_.Cpu(static_cast<uint32>(record->cpuDescriptor)),
                                        record->resource.Get(), values, 0, nullptr);
    return Result<void>::Success();
}

} // namespace rocks::graphics::d3d12
