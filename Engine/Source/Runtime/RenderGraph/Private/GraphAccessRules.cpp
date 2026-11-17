#include "RenderGraphInternal.h"

namespace rocks::render::detail {

using graphics::ResourceAccess;

namespace {

bool SkipsUavBarrier(GraphAccessFlags previous, GraphAccessFlags next) noexcept {
    return previous == GraphAccessFlags::NoUavBarrier && next == GraphAccessFlags::NoUavBarrier;
}

} // namespace

bool CanMergeAcrossPasses(ResourceAccess previous, GraphAccessFlags previousFlags,
                          ResourceAccess next, GraphAccessFlags nextFlags) noexcept {
    // 같은 접근이 아니면 합친 조합이 한 상태로 성립해야 한다(읽기 전용과 쓰기, UAV와 다른 것, 서로
    // 다른 쓰기는 합치지 않는다).
    if (previous != next && !graphics::CanCombineAccess(previous | next)) {
        return false;
    }

    // 다음 접근이 UAV면 그 사이 UAV barrier가 필요하므로 합치지 않는다(생략 표시가 둘 다 있을 때만
    // 합친다).
    if (graphics::HasAnyAccess(next, graphics::UavAccessMask) &&
        !SkipsUavBarrier(previousFlags, nextFlags)) {
        return false;
    }

    // 표시가 다르면 합치지 않는다(UE와 같다).
    return previousFlags == nextFlags;
}

bool CanMergeWithinPass(ResourceAccess first, ResourceAccess second) noexcept {
    return first == second || graphics::CanCombineAccess(first | second);
}

bool IsTransitionRequired(ResourceAccess previous, GraphAccessFlags previousFlags,
                          ResourceAccess next, GraphAccessFlags nextFlags) noexcept {
    if (previous != next) {
        return true;
    }

    // 같은 UAV 상태라도 앞 쓰기가 끝나기를 기다려야 한다(UAV barrier). 생략 표시가 둘 다 있으면
    // 겹쳐도 되는 쓰기다.
    if (graphics::HasAnyAccess(next, graphics::UavAccessMask)) {
        return !SkipsUavBarrier(previousFlags, nextFlags);
    }

    return false;
}

void FormatAccess(ResourceAccess access, char* out, usize capacity) noexcept {
    struct Named {
        ResourceAccess bit;
        const char* name;
    };

    static constexpr Named Names[] = {{ResourceAccess::VertexOrIndex, "VertexOrIndex"},
                                      {ResourceAccess::Constant, "Constant"},
                                      {ResourceAccess::IndirectArgs, "IndirectArgs"},
                                      {ResourceAccess::SrvGraphics, "SrvGraphics"},
                                      {ResourceAccess::SrvCompute, "SrvCompute"},
                                      {ResourceAccess::CopySource, "CopySource"},
                                      {ResourceAccess::Present, "Present"},
                                      {ResourceAccess::DepthRead, "DepthRead"},
                                      {ResourceAccess::UavGraphics, "UavGraphics"},
                                      {ResourceAccess::UavCompute, "UavCompute"},
                                      {ResourceAccess::RenderTarget, "RenderTarget"},
                                      {ResourceAccess::DepthWrite, "DepthWrite"},
                                      {ResourceAccess::CopyDestination, "CopyDestination"},
                                      {ResourceAccess::Discard, "Discard"}};

    if (capacity == 0) {
        return;
    }

    usize used = 0;
    auto append = [&](const char* text) noexcept {
        for (const char* cursor = text; *cursor != '\0' && used + 1 < capacity; ++cursor) {
            out[used++] = *cursor;
        }
    };

    if (access == ResourceAccess::Unknown) {
        append("Unknown");
    }

    for (const Named& named : Names) {
        if (graphics::HasAnyAccess(access, named.bit)) {
            if (used != 0) {
                append("|");
            }

            append(named.name);
        }
    }

    out[used] = '\0';
}

} // namespace rocks::render::detail
