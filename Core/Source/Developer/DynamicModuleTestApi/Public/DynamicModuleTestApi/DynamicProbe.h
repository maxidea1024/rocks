#pragma once

#include <Base/Types.h>
#include <Core/Module/ModuleImplementation.h>

namespace rocks::test {

// 실행 중 적재 시험 모듈의 공개 interface다(Docs/Design/ModuleLoader 3.8절). 시험 실행
// 파일은 그 모듈을 DynamicModules로 두므로 그 헤더를 볼 수 없다. 그래서 이 interface와 로더의
// Find<DynamicProbe>(ModuleName)으로 수명 객체를 쓴다(UE에서 공개 interface를 별도 모듈에 두고
// FModuleManager::GetModuleChecked<T>로 얻는 방식).
//
// 사용 예:
//   ModuleLoadReport loaded = loader.Load(DynamicProbe::ModuleName);
//   DynamicProbe* probe = loader.Find<DynamicProbe>(DynamicProbe::ModuleName);
//   int32 answer = probe->Answer();
class DynamicProbe : public ModuleImplementation {
public:
    // 이 interface를 구현하는 모듈의 이름이다(빌드의 모듈 이름과 같다).
    static constexpr const char* ModuleName = "DynamicModuleTestFixture";

    // 구현 모듈이 돌려주는 고정 값이다(링크·적재가 실제 구현에 닿았는지 확인한다).
    static constexpr int32 ExpectedAnswer = 42;

    // 구현 모듈의 고정 값(ExpectedAnswer)이다.
    virtual int32 Answer() const noexcept = 0;

    // 이 수명 객체의 Startup이 끝났는지다(로더가 Startup을 불렀는지 확인한다).
    virtual bool Started() const noexcept = 0;
};

} // namespace rocks::test
