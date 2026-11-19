#pragma once

#include <ModuleLoader/ModuleLoader.h>

// 시험이 로더의 현재 묶음을 바꾸는 모듈 내부 접근점이다. 현재 묶음은 단계 실행(StartPhase, ML1)이
// 정하는데 ML0에는 단계 실행이 없으므로, 묶음별 종료 시험은 이 접근점으로 묶음을 바꾼다. 공개 API에
// 두면 호출자가 단계와 어긋난 묶음을 만들 수 있으므로 Tests/ 소스만 include한다(정책 검사
// test-access-include).
namespace rocks::detail {

struct ModuleLoaderTestAccess {
    static void SetCurrentGroup(ModuleLoader& loader, ModuleGroup group) noexcept {
        loader.currentGroup_ = group;
    }
};

} // namespace rocks::detail
