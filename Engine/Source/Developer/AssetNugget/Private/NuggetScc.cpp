#include <AssetNugget/NuggetScc.h>
#include <Base/Endian.h>
#include <Platform/Fatal.h>

namespace rocks {

uint32 NuggetSccValueId(uint32 member, uint32 part) noexcept {
    ROCKS_ASSERT(member < NuggetSccMaxMembers && part < NuggetSccMaxParts);
    return (member << NuggetSccPartBits) | part;
}

Result<void> WriteNuggetSccMeta(Span<const AssetId> members, Array<uint8>& meta) noexcept {
    meta.Clear();
    if (members.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (members.Size() > NuggetSccMaxMembers) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    for (usize index = 0; index < members.Size(); ++index) {
        if (members[index].IsNull()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        for (usize earlier = 0; earlier < index; ++earlier) {
            if (members[earlier] == members[index]) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }
    }

    if (auto resized = meta.TryResize(members.Size() * NuggetSccMemberBytes); !resized) {
        return resized;
    }

    for (usize index = 0; index < members.Size(); ++index) {
        uint8* at = meta.Data() + index * NuggetSccMemberBytes;
        StoreBigEndian<uint64>(at, members[index].value.high);
        StoreBigEndian<uint64>(at + sizeof(uint64), members[index].value.low);
    }

    return Result<void>::Success();
}

Result<uint32> FindNuggetSccMember(Span<const uint8> meta, AssetId asset) noexcept {
    if (meta.Size() % NuggetSccMemberBytes != 0 ||
        meta.Size() / NuggetSccMemberBytes > NuggetSccMaxMembers) {
        return Result<uint32>::Failure(ErrorCode::InvalidArgument);
    }

    for (usize index = 0; index < meta.Size() / NuggetSccMemberBytes; ++index) {
        const uint8* at = meta.Data() + index * NuggetSccMemberBytes;
        if (LoadBigEndian<uint64>(at) == asset.value.high &&
            LoadBigEndian<uint64>(at + sizeof(uint64)) == asset.value.low) {
            return Result<uint32>::Success(static_cast<uint32>(index));
        }
    }

    return Result<uint32>::Failure(ErrorCode::NotFound);
}

} // namespace rocks
