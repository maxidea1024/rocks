#include "D3D11DeviceImpl.h"

// 상수 버퍼(Docs/Design/RenderBase 9.11절 GX6)다. handle은 버퍼 하나(SingleFrame IMMUTABLE,
// MultiFrame DEFAULT)이고, 기록 명령(SetConstantData)은 (단계, slot)마다 DYNAMIC 버퍼 하나를 기록
// context에서 WRITE_DISCARD로 갈아 쓴다. deferred context의 DYNAMIC 내용은 그 기록에서 DISCARD로
// 쓰기 전까지 정해지지 않으므로 handle에는 DYNAMIC을 쓰지 않는다.
namespace rocks::graphics::d3d11 {

Result<ConstantBufferHandle>
D3D11Device::CreateConstantBuffer(ConstantBufferLifetime lifetime,
                                  Span<const uint8> contents) noexcept {
    platform::LockGuard lock(gate_);
    if ((lifetime != ConstantBufferLifetime::SingleFrame &&
         lifetime != ConstantBufferLifetime::MultiFrame) ||
        !IsValidConstantBufferBytes(contents.Size())) {
        auto failed =
            Fail("constant buffer contents must be 16-byte multiples of 16 to 65536 bytes");
        (void)failed;
        return Result<ConstantBufferHandle>::Failure(ErrorCode::InvalidArgument);
    }

    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return Result<ConstantBufferHandle>::Failure(reserved.Error());
    }

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = static_cast<UINT>(contents.Size());
    desc.Usage = lifetime == ConstantBufferLifetime::SingleFrame ? D3D11_USAGE_IMMUTABLE
                                                                 : D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const D3D11_SUBRESOURCE_DATA initial = {contents.Data(), 0, 0};
    ConstantBufferRecord record;
    record.lifetime = lifetime;
    record.bytes = static_cast<uint32>(contents.Size());
    const HRESULT result = device_->CreateBuffer(&desc, &initial, record.buffer.Out());
    if (FAILED(result)) {
        return FailWith<ConstantBufferHandle>("CreateBuffer (constant buffer)", result);
    }

    auto added = constantBuffers_.TryAdd(Move(record));
    if (!added) {
        return Result<ConstantBufferHandle>::Failure(added.Error());
    }

    return Result<ConstantBufferHandle>::Success(ConstantBufferHandle{added.Value()});
}

Result<void> D3D11Device::UpdateConstantBuffer(ConstantBufferHandle buffer,
                                               Span<const uint8> contents) noexcept {
    platform::LockGuard lock(gate_);
    const ConstantBufferRecord* record = constantBuffers_.Find(buffer.slot);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (record->lifetime != ConstantBufferLifetime::MultiFrame ||
        contents.Size() != record->bytes) {
        auto failed = Fail("only MultiFrame constant buffers can be updated, with their size");
        (void)failed;
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 기록 중인 소유 스레드는 기록 context에 갱신을 넣어 기록 순서를 지킨다(앞 그리기는 옛 내용).
    // 기록 밖이거나 다른 스레드면 다음 제출 앞의 업로드 기록에 쌓는다(다른 스레드가 기록 중에
    // 바꾸면 그 기록 전체가 새 내용을 본다).
    if (recording_ && ownership_.IsOwner()) {
        deferred_->UpdateSubresource(record->buffer.Get(), 0, nullptr, contents.Data(), 0, 0);
        return Result<void>::Success();
    }

    return RecordBufferUpload(record->buffer.Get(), 0, contents);
}

void D3D11Device::Retire(ConstantBufferHandle buffer) noexcept {
    if (!ownership_.Check()) {
        return;
    }

    platform::LockGuard lock(gate_);
    ConstantBufferRecord* record = constantBuffers_.Find(buffer.slot);
    if (record == nullptr) {
        return;
    }

    Defer(Move(record->buffer));
    auto removed = constantBuffers_.TryRemove(buffer.slot);
    (void)removed;
}

void D3D11Device::SetConstantBuffer(ShaderStage stage, uint32 slot,
                                    ConstantBufferHandle buffer) noexcept {
    platform::LockGuard lock(gate_);
    const uint32 index = ConstantBufferStageIndex(stage);
    if (index >= ConstantBufferStageCount || slot >= MaxConstantBufferSlots) {
        return;
    }

    const bool live = constantBuffers_.Find(buffer.slot) != nullptr;
    boundConstants_[index][slot] = live ? BoundConstant{buffer.slot, false, 0} : BoundConstant{};
}

Result<void> D3D11Device::SetConstantData(ShaderStage stage, uint32 slot,
                                          Span<const uint8> bytes) noexcept {
    platform::LockGuard lock(gate_);
    const uint32 index = ConstantBufferStageIndex(stage);
    if (!recording_ || index >= ConstantBufferStageCount || slot >= MaxConstantBufferSlots ||
        slot == RootConstantsRegister || !IsValidConstantBufferBytes(bytes.Size())) {
        auto failed = Fail("SetConstantData needs a recording, a stage slot below 13 and 16-byte "
                           "multiples of 16 to 65536 bytes");
        (void)failed;
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 버퍼가 작으면 더 큰 것으로 바꾼다(옛것은 이 기록이 참조하므로 회수 대기로 넘긴다).
    ComRef<ID3D11Buffer>& transient = transientConstants_[index][slot];
    if (transientBytes_[index][slot] < bytes.Size()) {
        auto reserved = ReserveRetireCapacity(1);
        if (!reserved) {
            boundConstants_[index][slot] = {};
            return reserved;
        }

        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = static_cast<UINT>(bytes.Size());
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ComRef<ID3D11Buffer> grown;
        const HRESULT result = device_->CreateBuffer(&desc, nullptr, grown.Out());
        if (FAILED(result)) {
            boundConstants_[index][slot] = {};
            return Fail("CreateBuffer (transient constants)", result);
        }

        if (transient) {
            Defer(Move(transient));
        }

        transient = Move(grown);
        transientBytes_[index][slot] = static_cast<uint32>(bytes.Size());
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    const HRESULT result = deferred_->Map(transient.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(result)) {
        boundConstants_[index][slot] = {};
        return Fail("Map (transient constants)", result);
    }

    platform::MemoryCopy(mapped.pData, bytes.Data(), bytes.Size());
    deferred_->Unmap(transient.Get(), 0);
    boundConstants_[index][slot] =
        BoundConstant{SlotHandle{}, true, static_cast<uint32>(bytes.Size())};
    return Result<void>::Success();
}

ID3D11Buffer* D3D11Device::BoundConstantBuffer(uint32 stage, uint32 slot,
                                               uint32& bytes) const noexcept {
    const BoundConstant& bound = boundConstants_[stage][slot];
    if (bound.data) {
        bytes = bound.bytes;
        return transientConstants_[stage][slot].Get();
    }

    const ConstantBufferRecord* record = constantBuffers_.Find(bound.handle);
    bytes = record != nullptr ? record->bytes : 0u;
    return record != nullptr ? record->buffer.Get() : nullptr;
}

bool D3D11Device::ApplyConstants(const PipelineRecord& pipeline) noexcept {
    uint32 bytes[ConstantBufferStageCount][MaxConstantBufferSlots] = {};
    ID3D11Buffer* buffers[ConstantBufferStageCount][MaxConstantBufferSlots] = {};
    const ConstantBufferSlots& slots = pipeline.slots.constantSlots;
    for (uint32 stage = 0; stage < ConstantBufferStageCount; ++stage) {
        for (uint32 slot = 0; slot < MaxConstantBufferSlots; ++slot) {
            if ((slots.usedMask[stage] & (1u << slot)) != 0) {
                buffers[stage][slot] = BoundConstantBuffer(stage, slot, bytes[stage][slot]);
            }
        }
    }

    if (!ConstantBuffersSatisfied(slots, bytes)) {
        return false;
    }

    for (uint32 stage = 0; stage < ConstantBufferStageCount; ++stage) {
        for (uint32 slot = 0; slot < MaxConstantBufferSlots; ++slot) {
            if ((slots.usedMask[stage] & (1u << slot)) == 0) {
                continue;
            }

            ID3D11Buffer* buffer = buffers[stage][slot];
            switch (static_cast<ShaderStage>(stage)) {
            case ShaderStage::Vertex:
                deferred_->VSSetConstantBuffers(slot, 1, &buffer);
                break;
            case ShaderStage::Pixel:
                deferred_->PSSetConstantBuffers(slot, 1, &buffer);
                break;
            case ShaderStage::Compute:
                deferred_->CSSetConstantBuffers(slot, 1, &buffer);
                break;
            }
        }
    }

    return true;
}

} // namespace rocks::graphics::d3d11
