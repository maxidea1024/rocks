#pragma once

#include <Base/Endian.h>
#include <Base/NumericLimits.h>
#include <Core/Containers/Array.h>
#include <Core/Result.h>
#include <Mesh/Common/MeshRenderStream.h>
#include <Platform/Memory.h>

// 쿠킹 LOD 덩어리 쓰기다(Docs/Design/MeshAnimationAssets 5.5.2절 1·6.7.2절 7 — 정적·스킨 패킹이
// 같이 쓴다). 이름 있는 namespace라 유니티 빌드에서 파일마다 겹치지 않는다.
namespace rocks::mesh_packing {

// 성분 단위 쓰기다(little-endian, 넘침은 호출자가 크기를 먼저 잡아 막는다).
class PackWriter {
public:
    explicit PackWriter(Array<uint8>& bytes) noexcept : bytes_(bytes) {
    }

    // 16 byte 정렬까지 0을 채우고 n byte 구간을 잡아 시작을 돌려준다.
    Result<uint64> Begin(uint64 n) noexcept {
        const uint64 start =
            (bytes_.Size() + MeshRenderStreamAlignment - 1) & ~(MeshRenderStreamAlignment - 1);
        if (n > NumericLimits<usize>::Max() - start) {
            return Result<uint64>::Failure(ErrorCode::CapacityExceeded);
        }

        if (auto grown = bytes_.TryResize(static_cast<usize>(start + n)); !grown) {
            return Result<uint64>::Failure(grown.Error());
        }

        cursor_ = static_cast<usize>(start);
        return Result<uint64>::Success(start);
    }

    template <class T>
    void Put(T value) noexcept {
        StoreLittleEndian<T>(bytes_.Data() + cursor_, value);
        cursor_ += sizeof(T);
    }

    void PutFloat(float value) noexcept {
        Put<uint32>(platform::BitCast<uint32>(value));
    }

    void PutByte(uint8 value) noexcept {
        bytes_[cursor_++] = value;
    }

private:
    Array<uint8>& bytes_;
    usize cursor_ = 0;
};

} // namespace rocks::mesh_packing
