#include "GlesDevice.h"

#include <Platform/Memory.h>

// 상수 버퍼(Docs/Design/RenderBase 9.11절 GX6)다. OpenGL ES는 상수 버퍼를 늘 에뮬레이션한다
// (ShaderSystem 7.2절 4항): handle과 기록 명령 모두 CPU 사본이고, 그리기 때 값 2 복사 표대로
// <단계>c<slot>_<타입> 배열에 glUniform4fv·4iv·4uiv로 올린다. program마다 (단계, slot)에 마지막으로
// 올린 (identity, version)을 기억해 다를 때만 올린다(RenderBase 9.4절 판단, UE와 같다). GL 명령은
// 기록 때 바로 실행되므로 갱신은 그 뒤의 그리기부터 보인다(기록 순서).
namespace rocks::graphics::gles {

Result<ConstantBufferHandle> GlesDevice::CreateConstantBuffer(ConstantBufferLifetime lifetime,
                                                              Span<const uint8> contents) noexcept {
    // OpenGL ES 장치는 생성도 소유 스레드 전용이다(concurrentResourceCreation 거짓).
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<ConstantBufferHandle>::Failure(owner.Error());
    }

    if ((lifetime != ConstantBufferLifetime::SingleFrame &&
         lifetime != ConstantBufferLifetime::MultiFrame) ||
        !IsValidConstantBufferBytes(contents.Size())) {
        Diagnostic message;
        message.Text("constant buffer contents must be 16-byte multiples of 16 to 65536 bytes");
        auto rejected = Reject(message, ErrorCode::InvalidArgument);
        return Result<ConstantBufferHandle>::Failure(rejected.Error());
    }

    ConstantBufferRecord record(allocator_);
    record.lifetime = lifetime;
    record.identity = ++constantIdentity_;
    if (!record.bytes.TryAppend(contents)) {
        return Result<ConstantBufferHandle>::Failure(ErrorCode::OutOfMemory);
    }

    auto added = constantBuffers_.TryAdd(Move(record));
    if (!added) {
        return Result<ConstantBufferHandle>::Failure(added.Error());
    }

    return Result<ConstantBufferHandle>::Success(ConstantBufferHandle{added.Value()});
}

Result<void> GlesDevice::UpdateConstantBuffer(ConstantBufferHandle buffer,
                                              Span<const uint8> contents) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return owner;
    }

    ConstantBufferRecord* record = constantBuffers_.Find(buffer.slot);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (record->lifetime != ConstantBufferLifetime::MultiFrame ||
        contents.Size() != record->bytes.Size()) {
        Diagnostic message;
        message.Text("only MultiFrame constant buffers can be updated, with their size");
        return Reject(message, ErrorCode::InvalidArgument);
    }

    platform::MemoryCopy(record->bytes.Data(), contents.Data(), contents.Size());
    ++record->version;
    return Result<void>::Success();
}

// glUniform이 값을 복사하므로 GPU가 붙잡는 것이 없다(바로 지운다).
void GlesDevice::Retire(ConstantBufferHandle buffer) noexcept {
    if (!ownership_.Check()) {
        return;
    }

    (void)constantBuffers_.TryRemove(buffer.slot);
}

void GlesDevice::SetConstantBuffer(ShaderStage stage, uint32 slot,
                                   ConstantBufferHandle buffer) noexcept {
    const uint32 index = ConstantBufferStageIndex(stage);
    if (index >= ConstantBufferStageCount || slot >= MaxConstantBufferSlots) {
        return;
    }

    const bool live = constantBuffers_.Find(buffer.slot) != nullptr;
    boundConstants_[index][slot] = live ? BoundConstant{buffer.slot, false} : BoundConstant{};
}

Result<void> GlesDevice::SetConstantData(ShaderStage stage, uint32 slot,
                                         Span<const uint8> bytes) noexcept {
    const uint32 index = ConstantBufferStageIndex(stage);
    if (!recording_ || index >= ConstantBufferStageCount || slot >= MaxConstantBufferSlots ||
        !IsValidConstantBufferBytes(bytes.Size())) {
        Diagnostic message;
        message.Text("SetConstantData needs a recording, a stage slot below 14 and 16-byte "
                     "multiples of 16 to 65536 bytes");
        return Reject(message, ErrorCode::InvalidArgument);
    }

    // 기록 하나의 사본은 한 저장소에 이어 붙이고 기록 시작 때 비운다(위치로 가리킨다).
    const usize offset = transientConstants_.Size();
    if (!transientConstants_.TryAppend(bytes)) {
        boundConstants_[index][slot] = {};
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    transientOffsets_[index][slot] = offset;
    transientBytes_[index][slot] = static_cast<uint32>(bytes.Size());

    // 묶을 때마다 새 identity라 다음 그리기가 늘 올린다.
    transientIdentity_[index][slot] = ++constantIdentity_;
    boundConstants_[index][slot] = BoundConstant{SlotHandle{}, true};
    return Result<void>::Success();
}

bool GlesDevice::ApplyConstantBuffers(SlotHandle pipelineSlot) noexcept {
    PipelineRecord* pipeline = pipelines_.Find(pipelineSlot);
    if (pipeline == nullptr) {
        return false;
    }

    const ConstantBufferSlots& slots = pipeline->slots.constantSlots;
    const uint8* sources[ConstantBufferStageCount][MaxConstantBufferSlots] = {};
    uint32 bytes[ConstantBufferStageCount][MaxConstantBufferSlots] = {};
    uint64 identities[ConstantBufferStageCount][MaxConstantBufferSlots] = {};
    uint64 versions[ConstantBufferStageCount][MaxConstantBufferSlots] = {};
    for (uint32 stage = 0; stage < ConstantBufferStageCount; ++stage) {
        for (uint32 slot = 0; slot < MaxConstantBufferSlots; ++slot) {
            if ((slots.usedMask[stage] & (1u << slot)) == 0) {
                continue;
            }

            const BoundConstant& bound = boundConstants_[stage][slot];
            if (bound.data) {
                sources[stage][slot] = transientConstants_.Data() + transientOffsets_[stage][slot];
                bytes[stage][slot] = transientBytes_[stage][slot];
                identities[stage][slot] = transientIdentity_[stage][slot];
                continue;
            }

            const ConstantBufferRecord* record = constantBuffers_.Find(bound.handle);
            if (record != nullptr) {
                sources[stage][slot] = record->bytes.Data();
                bytes[stage][slot] = static_cast<uint32>(record->bytes.Size());
                identities[stage][slot] = record->identity;
                versions[stage][slot] = record->version;
            }
        }
    }

    if (!ConstantBuffersSatisfied(slots, bytes)) {
        return false;
    }

    for (uint32 stage = 0; stage < ConstantBufferStageCount; ++stage) {
        for (uint32 slot = 0; slot < MaxConstantBufferSlots; ++slot) {
            if ((slots.usedMask[stage] & (1u << slot)) == 0 ||
                (pipeline->uploadedIdentity[stage][slot] == identities[stage][slot] &&
                 pipeline->uploadedVersion[stage][slot] == versions[stage][slot])) {
                if ((slots.usedMask[stage] & (1u << slot)) != 0) {
                    ++constantUploadSkips_;
                }

                continue;
            }

            for (uint32 index = 0; index < pipeline->constantCopyCount; ++index) {
                const GlesConstantCopy& copy = pipeline->constantCopies[index];
                if (copy.stage != stage || copy.slot != slot) {
                    continue;
                }

                const uint8* source = sources[stage][slot] + copy.sourceOffset;
                switch (copy.type) {
                case shader::ShaderGlesUniformType::Float:
                    gl_.Uniform4fv(copy.location, copy.vectors,
                                   reinterpret_cast<const GLfloat*>(source));
                    break;
                case shader::ShaderGlesUniformType::Int:
                    gl_.Uniform4iv(copy.location, copy.vectors,
                                   reinterpret_cast<const GLint*>(source));
                    break;
                case shader::ShaderGlesUniformType::Uint:
                    gl_.Uniform4uiv(copy.location, copy.vectors,
                                    reinterpret_cast<const GLuint*>(source));
                    break;
                }
            }

            pipeline->uploadedIdentity[stage][slot] = identities[stage][slot];
            pipeline->uploadedVersion[stage][slot] = versions[stage][slot];
            ++constantUploads_;
        }
    }

    return true;
}

} // namespace rocks::graphics::gles
