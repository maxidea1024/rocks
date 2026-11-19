#pragma once

#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>

namespace rocks {

// 호스트 서비스 형식 하나의 식별자다. 주소가 식별자이므로 형식마다 한 벌만 있어야 한다. 그래서
// 서비스 형식을 가진 모듈의 cpp에 ROCKS_DEFINE_HOST_SERVICE로 한 번만 정의한다(공개 헤더에 정의하면
// Modular에서 공유 라이브러리마다 다른 주소가 된다 — Docs/Design/BuildTargets.ko.md BD29).
struct HostServiceKey {
    // 진단용 형식 이름(ROCKS_DEFINE_HOST_SERVICE에 쓴 철자)이다.
    const char* name = nullptr;
};

// 서비스 형식 T의 키를 찾는 traits다. 기본 템플릿은 정의하지 않는다. ROCKS_DECLARE_HOST_SERVICE로
// 특수화하지 않은 형식을 Register·Find에 쓰면 컴파일 오류다.
template <class T>
struct HostServiceTraits;

// Launch와 엔진 호스트가 단계마다 등록하는 호스트 서비스 표다(Docs/Design/ModuleLoader.ko.md 3.1절,
// ConfigContext·LogSystem·TraceSession·Engine 등). 모듈은 전역 접근자 대신
// ModuleStartContext::Services() 로 찾는다. 로더가 하나를 소유한다(ModuleLoader::Services()).
//
// 사용 예:
//   // 서비스 형식의 공개 헤더(전역 namespace에서, 형식 선언 뒤):
//   ROCKS_DECLARE_HOST_SERVICE(rocks::config::ConfigContext)
//   // 그 모듈의 cpp 하나:
//   ROCKS_DEFINE_HOST_SERVICE(rocks::config::ConfigContext)
//   // 호스트: 서비스를 만든 뒤 등록하고, 파괴하기 전에 뗀다.
//   (void)loader.Services().Register(configContext);
//   ...
//   loader.Services().Unregister(configContext);
//   // 모듈 Startup:
//   if (auto* config = context.Services().Find<config::ConfigContext>()) { ... }
//
// 계약:
// - 서비스 객체는 호출자가 소유한다. 등록한 동안 살아 있어야 하며 파괴 전에 Unregister한다.
// - 형식마다 하나만 등록한다. 키는 형식 그대로다(파생 형식으로 등록하면 기반 형식 Find로 찾지
// 않는다).
// - 내부 동기화가 없다. 등록·해제·조회는 로더 스레드에서 한다.
class HostServices final {
public:
    // allocator는 표 저장소용이며 이 객체보다 오래 살아야 한다.
    explicit HostServices(Allocator& allocator) noexcept;

    HostServices(const HostServices&) = delete;
    HostServices& operator=(const HostServices&) = delete;

    // service를 T의 서비스로 등록한다. 실패: 이미 T가 등록되어 있으면 AlreadyRegistered, 표 할당
    // 실패는 그 오류다. 실패하면 표는 바뀌지 않는다.
    template <class T>
    Result<void> Register(T& service) noexcept {
        return RegisterKey(HostServiceTraits<T>::Key(), &service);
    }

    // service가 T로 등록되어 있으면 떼고 true다. 다른 객체가 등록되어 있거나 없으면 false이고
    // 아무것도 바꾸지 않는다.
    template <class T>
    bool Unregister(T& service) noexcept {
        return UnregisterKey(HostServiceTraits<T>::Key(), &service);
    }

    // T로 등록된 서비스다. 아직 등록되지 않았으면 null이다.
    template <class T>
    T* Find() const noexcept {
        return static_cast<T*>(FindKey(HostServiceTraits<T>::Key()));
    }

    // 등록된 서비스 수다.
    usize Count() const noexcept {
        return entries_.Size();
    }

private:
    struct Entry {
        const HostServiceKey* key = nullptr;
        void* service = nullptr;
    };

    Result<void> RegisterKey(const HostServiceKey& key, void* service) noexcept;
    bool UnregisterKey(const HostServiceKey& key, void* service) noexcept;
    void* FindKey(const HostServiceKey& key) const noexcept;

    Array<Entry> entries_;
};

} // namespace rocks

// 서비스 형식 Type의 키 조회를 선언한다. Type의 공개 헤더에서 전역 namespace에 한 번 쓴다. 정의는
// 소유 모듈 cpp의 ROCKS_DEFINE_HOST_SERVICE(Type)다.
#define ROCKS_DECLARE_HOST_SERVICE(Type)                                                           \
    template <>                                                                                    \
    struct rocks::HostServiceTraits<Type> {                                                        \
        static const ::rocks::HostServiceKey& Key() noexcept;                                      \
    };

// 서비스 형식 Type의 키를 정의한다. Type을 가진 모듈의 cpp 하나에서 전역 namespace에 한 번 쓴다. 키
// 저장소는 링커가 같은 내용의 상수끼리 합치지 않게(ICF) const가 아닌 정적 객체다(상수 초기화라 전역
// 생성자는 없다).
#define ROCKS_DEFINE_HOST_SERVICE(Type)                                                            \
    const ::rocks::HostServiceKey& rocks::HostServiceTraits<Type>::Key() noexcept {                \
        static ::rocks::HostServiceKey key{#Type};                                                 \
        return key;                                                                                \
    }
