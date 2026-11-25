#pragma once

#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Module/HostServiceKey.h>
#include <Core/Result.h>

namespace rocks {

// 엔진 호스트가 모듈 단계 실행 전에 등록하는 호스트 서비스 표다(Docs/Design/ModuleLoader 3.1·
// 3.12절, 지금은 LogSystem·ConfigContext·TraceSession). 모듈은 전역 접근자 대신
// ModuleStartContext::Services() 로 찾는다. 로더가 하나를 소유한다(ModuleLoader::Services()).
//
// 사용 예:
//   // 서비스 형식의 공개 헤더(전역 namespace에서, 형식 선언 뒤):
//   ROCKS_DECLARE_HOST_SERVICE(rocks::config::ConfigContext, ROCKS_CONFIG_API)
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
// - 서비스를 Startup 뒤에도 쥐는 모듈은 서비스가 등록된 기간 안에 시작·종료하는 묶음이어야 한다.
//   호스트는 서비스 등록 기간에 그런 모듈을 프로세스 묶음으로 Load하지 않는다(3.12절).
class ROCKS_MODULELOADER_API HostServices final {
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
