#include <RenderBase/ShaderParameters.h>

#include <Platform/Atomics.h>
#include <ShaderCore/ShaderFormatTypes.h>
#include <string.h>

// 바인딩 지도·상수 블록(Docs/Design/RenderBase 9.3·9.4절)이다. 규칙은 공개 헤더 주석을
// 따른다.
namespace rocks::render {

namespace shader_parameters_detail {

// 상수 블록 identity 발급기다(0은 쓰지 않는다).
uint64 nextBlockIdentity = 0;

Result<ShaderBindingMap> Reject(const char** reason, const char* text) noexcept {
    if (reason != nullptr) {
        *reason = text;
    }

    return Result<ShaderBindingMap>::Failure(ErrorCode::InvalidArgument);
}

const shader::ShaderParameterMember* FindLayoutMember(const shader::ShaderParameterLayout& layout,
                                                      StringView name) noexcept {
    for (const shader::ShaderParameterMember& member : layout.members) {
        if (member.name == name) {
            return &member;
        }
    }

    return nullptr;
}

} // namespace shader_parameters_detail

using namespace shader_parameters_detail;

Result<ShaderBindingMap>
ShaderBindingMap::Build(const shader::ShaderParameterLayout& layout,
                        Span<const shader::ShaderBindingLayout* const> stages,
                        const char** reason) noexcept {
    if (!shader::ValidateShaderParameterLayout(layout, reason)) {
        return Result<ShaderBindingMap>::Failure(ErrorCode::InvalidArgument);
    }

    ShaderBindingMap map;
    map.layout_ = &layout;
    for (const shader::ShaderBindingLayout* stage : stages) {
        if (stage == nullptr) {
            continue;
        }

        if (map.count_ == MaxShaderConstantPlacements) {
            return Reject(reason, "binding map takes at most three stages");
        }

        if (layout.rootConstants) {
            if (stage->rootConstantBytes == 0) {
                continue;
            }

            if (stage->rootConstantBytes > layout.totalBytes) {
                return Reject(reason, "stage root constants are larger than the layout table");
            }

            map.placements_[map.count_++] =
                ShaderConstantPlacement{stage->stage, true, 0, stage->rootConstantBytes};
            continue;
        }

        for (const shader::ShaderBindingResource& resource : stage->resources) {
            if (resource.kind != shader::ShaderResourceKind::ConstantBuffer ||
                resource.name != layout.name) {
                continue;
            }

            if (resource.constantBufferBytes > layout.totalBytes) {
                return Reject(reason, "stage constant buffer is larger than the layout table");
            }

            for (const shader::ShaderBindingMember& member : stage->MembersOf(resource)) {
                const shader::ShaderParameterMember* table = FindLayoutMember(layout, member.name);
                if (table == nullptr || table->offset != member.offset ||
                    shader::ShaderParameterMemberBytes(*table) != member.bytes) {
                    return Reject(reason, "stage constant buffer member differs from the layout "
                                          "table (name, offset or size)");
                }
            }

            map.placements_[map.count_++] = ShaderConstantPlacement{
                stage->stage, false, resource.slot, resource.constantBufferBytes};
        }
    }

    if (map.count_ == 0) {
        return Reject(reason, "no stage reads the layout table");
    }

    return Result<ShaderBindingMap>::Success(map);
}

Result<void> ShaderBindingMap::FindMember(StringView name, uint32& offset,
                                          uint32& bytes) const noexcept {
    const shader::ShaderParameterMember* member = FindLayoutMember(*layout_, name);
    if (member == nullptr) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    offset = member->offset;
    bytes = static_cast<uint32>(shader::ShaderParameterMemberBytes(*member));
    return Result<void>::Success();
}

ConstantBlock::ConstantBlock(Allocator& allocator, const shader::ShaderParameterLayout& layout,
                             ConstantBlockLifetime lifetime) noexcept
    : layout_(&layout), lifetime_(lifetime), bytes_(allocator),
      identity_(platform::AtomicFetchAdd(&nextBlockIdentity, uint64{1}) + 1) {
}

Result<void> ConstantBlock::Initialize() noexcept {
    if (!bytes_.TryResize(layout_->totalBytes)) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    if (layout_->totalBytes != 0) {
        memset(bytes_.Data(), 0, layout_->totalBytes);
    }

    ++version_;
    return Result<void>::Success();
}

Result<void> ConstantBlock::Set(StringView member, Span<const uint8> bytes) noexcept {
    const shader::ShaderParameterMember* found = FindLayoutMember(*layout_, member);
    if (found == nullptr) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    if (bytes.Size() != shader::ShaderParameterMemberBytes(*found) ||
        static_cast<uint64>(found->offset) + bytes.Size() > bytes_.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    memcpy(bytes_.Data() + found->offset, bytes.Data(), bytes.Size());
    ++version_;
    return Result<void>::Success();
}

graphics::ShaderStage ShaderStageOfBit(uint32 stage) noexcept {
    switch (stage) {
    case shader::ShaderStageVertex:
        return graphics::ShaderStage::Vertex;
    case shader::ShaderStagePixel:
        return graphics::ShaderStage::Pixel;
    default:
        break;
    }

    return graphics::ShaderStage::Compute;
}

Result<void> ConstantBlock::Commit(graphics::Device& device) noexcept {
    if (bytes_.Size() == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (IsCommitted()) {
        return Result<void>::Success();
    }

    if (lifetime_ == ConstantBlockLifetime::Persistent && !buffer_.IsNull()) {
        auto updated = device.UpdateConstantBuffer(buffer_, bytes_.View());
        if (!updated) {
            return updated;
        }

        committedVersion_ = version_;
        return Result<void>::Success();
    }

    const graphics::ConstantBufferLifetime lifetime =
        lifetime_ == ConstantBlockLifetime::Persistent
            ? graphics::ConstantBufferLifetime::MultiFrame
            : graphics::ConstantBufferLifetime::SingleFrame;
    auto created = device.CreateConstantBuffer(lifetime, bytes_.View());
    if (!created) {
        return Result<void>::Failure(created.Error());
    }

    // SingleFrame은 바꿀 수 없으므로 값이 바뀌면 새 버퍼를 쓰고 옛것은 마지막 사용 fence 뒤 놓는다.
    if (!buffer_.IsNull()) {
        device.Retire(buffer_);
    }

    buffer_ = created.Value();
    committedVersion_ = version_;
    return Result<void>::Success();
}

void ConstantBlock::Release(graphics::Device& device) noexcept {
    if (!buffer_.IsNull()) {
        device.Retire(buffer_);
        buffer_ = {};
    }

    committedVersion_ = 0;
}

Result<void> ApplyConstantBlock(graphics::CommandContext& commands, const ShaderBindingMap& map,
                                const ConstantBlock& block) noexcept {
    if (map.Layout().rootConstants || &block.Layout() != &map.Layout() || !block.IsCommitted()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (block.Lifetime() != ConstantBlockLifetime::PerDraw) {
        return ApplyConstantBuffer(commands, map, block.Buffer());
    }

    for (const ShaderConstantPlacement& placement : map.Placements()) {
        auto set = commands.SetConstantData(ShaderStageOfBit(placement.stage), placement.slot,
                                            block.Bytes());
        if (!set) {
            return set;
        }
    }

    return Result<void>::Success();
}

Result<void> ApplyConstantBuffer(graphics::CommandContext& commands, const ShaderBindingMap& map,
                                 graphics::ConstantBufferHandle buffer) noexcept {
    if (map.Layout().rootConstants) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (const ShaderConstantPlacement& placement : map.Placements()) {
        commands.SetConstantBuffer(ShaderStageOfBit(placement.stage), placement.slot, buffer);
    }

    return Result<void>::Success();
}

Result<void> ApplyDrawConstants(graphics::CommandContext& commands, const ShaderBindingMap& map,
                                const ConstantBlock& block) noexcept {
    if (!map.Layout().rootConstants || &block.Layout() != &map.Layout()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return commands.SetDrawConstants(block.Bytes());
}

bool ConstantUploadTracker::NeedsUpload(uint64 program, uint32 slot,
                                        const ConstantBlock& block) noexcept {
    for (Upload& upload : uploads_) {
        if (upload.program == program && upload.slot == slot) {
            if (upload.identity == block.Identity() && upload.version == block.Version()) {
                ++skipCount_;
                return false;
            }

            upload.identity = block.Identity();
            upload.version = block.Version();
            ++uploadCount_;
            return true;
        }
    }

    (void)uploads_.TryPushBack(Upload{program, slot, block.Identity(), block.Version()});
    ++uploadCount_;
    return true;
}

void ConstantUploadTracker::ForgetProgram(uint64 program) noexcept {
    for (usize index = uploads_.Size(); index > 0; --index) {
        if (uploads_[index - 1].program == program) {
            (void)uploads_.TryRemoveAtSwap(index - 1);
        }
    }
}

} // namespace rocks::render
