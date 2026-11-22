#pragma once

// 호스트 서비스 형식의 키다(Docs/Design/ModuleLoader.ko.md 3.1·3.12절 MD18). 서비스 형식을 가진
// 모듈이 이 헤더로 키를 선언·정의하고, 서비스 표(ModuleLoader 모듈의 HostServices)가 키로 서비스를
// 찾는다. Core에 두는 이유는 Core의 형식(LogSystem)도 서비스가 되어야 하는데 Core는 ModuleLoader에
// 의존할 수 없기 때문이다(진입점 계약 ModuleEntry.h와 같다).
//
// 사용 예:
//   // 서비스 형식의 공개 헤더(전역 namespace, 형식 선언 뒤 — 둘째 인자는 그 모듈 API 매크로):
//   ROCKS_DECLARE_HOST_SERVICE(rocks::config::ConfigContext, ROCKS_CONFIG_API)
//   // 그 모듈의 cpp 하나(전역 namespace):
//   ROCKS_DEFINE_HOST_SERVICE(rocks::config::ConfigContext)

namespace rocks {

// 호스트 서비스 형식 하나의 식별자다. 주소가 식별자이므로 형식마다 한 벌만 있어야 한다. 그래서
// 서비스 형식을 가진 모듈의 cpp에 ROCKS_DEFINE_HOST_SERVICE로 한 번만 정의한다(공개 헤더에 정의하면
// Modular에서 공유 라이브러리마다 다른 주소가 된다 — Docs/Design/BuildTargets.ko.md BD29).
struct HostServiceKey {
    // 진단용 형식 이름(ROCKS_DEFINE_HOST_SERVICE에 쓴 철자)이다.
    const char* name = nullptr;
};

// 서비스 형식 T의 키를 찾는 traits다. 기본 템플릿은 정의하지 않는다. ROCKS_DECLARE_HOST_SERVICE로
// 특수화하지 않은 형식을 HostServices::Register·Find에 쓰면 컴파일 오류다.
template <class T>
struct HostServiceTraits;

} // namespace rocks

// 서비스 형식 Type의 키 조회를 선언한다. Type의 공개 헤더에서 전역 namespace에 한 번 쓴다. Api는
// 소유 모듈의 API 매크로다(Modular에서 다른 모듈이 Key를 링크한다 —
// Docs/Design/BuildTargets.ko.md 3.8절 BS8d4. 한 번역 단위 안에서만 쓰는 형식은 빈 값). 정의는 소유
// 모듈 cpp의 ROCKS_DEFINE_HOST_SERVICE(Type)다.
#define ROCKS_DECLARE_HOST_SERVICE(Type, Api)                                                      \
    template <>                                                                                    \
    struct rocks::HostServiceTraits<Type> {                                                        \
        Api static const ::rocks::HostServiceKey& Key() noexcept;                                  \
    };

// 서비스 형식 Type의 키를 정의한다. Type을 가진 모듈의 cpp 하나에서 전역 namespace에 한 번 쓴다. 키
// 저장소는 링커가 같은 내용의 상수끼리 합치지 않게(ICF) const가 아닌 정적 객체다(상수 초기화라 전역
// 생성자는 없다).
#define ROCKS_DEFINE_HOST_SERVICE(Type)                                                            \
    const ::rocks::HostServiceKey& rocks::HostServiceTraits<Type>::Key() noexcept {                \
        static ::rocks::HostServiceKey key{#Type};                                                 \
        return key;                                                                                \
    }
