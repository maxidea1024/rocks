#pragma once

#include <Base/Types.h>
#include <Mesh/Common/MeshRenderStream.h>

// 쿠킹 LOD stream 구간을 배치 순서로 보는 도우미다(Docs/Design/MeshAnimationAssets 5.5.1절 9). 이름
// 있는 namespace라 유니티 빌드에서 파일마다 겹치지 않는다.
namespace rocks::mesh_render_checks {

// 배치 순서로 stream 구간을 본다(정렬, 앞 구간 끝 이상, 넘침 없음, data 안).
class StreamWalk {
public:
    explicit StreamWalk(uint64 dataBytes) noexcept : dataBytes_(dataBytes) {
    }

    bool Next(const MeshRenderStream& stream) noexcept {
        if (stream.offset % MeshRenderStreamAlignment != 0 || stream.offset < end_ ||
            stream.byteSize > dataBytes_ || stream.offset > dataBytes_ - stream.byteSize) {
            return false;
        }

        end_ = stream.offset + stream.byteSize;
        return true;
    }

private:
    uint64 dataBytes_;
    uint64 end_ = 0;
};

} // namespace rocks::mesh_render_checks
