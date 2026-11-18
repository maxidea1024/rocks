#pragma once

#include <Config/CVarDefinition.h>
#include <Platform/Atomics.h>

// CVar 핸들 타입이다(Docs/Design/CVarSystem.ko.md 4절 — CVR1). 핸들은 빌더(Config/CVarBuilder.h)의
// consteval 변환으로만 만들어지는 상수 초기화 값이다: 전역 생성자가 없고 정적 초기화 순서 문제가
// 없다. 사용자는 Config/CVar.h를 include한다.
//
// 읽기(`Get()` — 이 스레드에 묶인 snapshot의 번호 칸)는 CVR2에서 snapshot·스레드 묶음과 함께
// 들어온다. 지금 핸들은 정의(이름·도움말·기본값·번호)와 등록(ConfigContext::Register)만 한다.
// 등록된 값은 이름으로 찾는 콘솔 설정 경로(ConfigContext::FindConsoleSetting·ConsoleValue)로 볼 수
// 있다.
namespace rocks::cvar {

template <class T>
class VarBuilder;

template <class T>
class BootVarBuilder;

// 실행 중 바꿀 수 있는 타입 있는 설정 변수다(UE TAutoConsoleVariable 대응). namespace 범위나 static
// 멤버(정적 저장 기간)에 `cvar::Var` 빌더 한 문장으로 정의하고, 모듈 등록 함수에서
// ConfigContext::Register로 올린다. 복사·이동할 수 없다(주소가 정체성이다).
//
// 사용 예:
//   CVar<float> ShadowDistance = cvar::Var("render.shadow.distance", 8000.f)
//       .Help("Max shadow distance (cm)").Range(500, 20000).Clamp();
//
//   Result<void> RegisterRenderVars(config::ConfigContext& config) noexcept {
//       return config.Register(ShadowDistance);
//   }
//
// 수명: 프로그램 수명(정적 저장 기간). 함수 안 지역 핸들은 Register가 거부한다(Debug·Development).
// 스레드: 상수 부분(이름·기본값 등)은 어느 스레드에서나 읽는다. Index는 원자적으로 읽는다.
template <class T>
class CVar {
public:
    CVar(const CVar&) = delete;
    CVar& operator=(const CVar&) = delete;
    CVar(CVar&&) = delete;
    CVar& operator=(CVar&&) = delete;

    // 콘솔 이름이다(정적 저장소). 출시 빌드의 `Cheat()` 변수는 비어 있다(13.1절).
    constexpr StringView Name() const noexcept {
        return definition_.name;
    }

    // 도움말이다(정적 저장소). 출시 빌드의 `Cheat()` 변수는 비어 있다.
    constexpr StringView Help() const noexcept {
        return definition_.help;
    }

    // 정의의 기본값이다(String·Name은 기본값 텍스트 — 정적 저장소).
    constexpr DefaultArg<T> DefaultValue() const noexcept {
        return detail::ValueOf<T>(definition_.defaultValue);
    }

    // 프로세스 전체 번호다(5절). 한 번도 등록되지 않았으면 IsValid가 거짓이다.
    cvar::Index Index() const noexcept {
        return cvar::Index{
            platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&definition_.index)};
    }

private:
    friend struct detail::HandleAccess;
    friend class VarBuilder<T>;

    constexpr explicit CVar(const detail::TypedDefinition<T>& definition) noexcept
        : definition_(definition) {
    }

    detail::TypedDefinition<T> definition_;
};

// 이번 실행 동안 고정인 설정 변수다(9절 — UE ECVF_ReadOnly + 시작 시 값). `cvar::Boot` 빌더로
// 정의한다. 값은 파일·`--set` 등 시작 전 층에서 정하고, ConfigContext::CompleteStartup 뒤에는 이번
// 실행 값이 바뀌지 않는다(지금은 Config의 StartupOnly 규칙 — 봉인 뒤 쓰기 거부 BootSealed는 CVR4).
// `Set`·`Clear`가 없다(부르면 컴파일 오류).
//
// 사용 예:
//   BootCVar<String> GraphicsBackend = cvar::Boot("render.backend", "vulkan")
//       .Help("Graphics backend");
//
// 수명·스레드: CVar와 같다.
template <class T>
class BootCVar {
public:
    BootCVar(const BootCVar&) = delete;
    BootCVar& operator=(const BootCVar&) = delete;
    BootCVar(BootCVar&&) = delete;
    BootCVar& operator=(BootCVar&&) = delete;

    constexpr StringView Name() const noexcept {
        return definition_.name;
    }

    constexpr StringView Help() const noexcept {
        return definition_.help;
    }

    constexpr DefaultArg<T> DefaultValue() const noexcept {
        return detail::ValueOf<T>(definition_.defaultValue);
    }

    cvar::Index Index() const noexcept {
        return cvar::Index{
            platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&definition_.index)};
    }

    // 부팅 변수는 실행 중에 쓸 수 없다(4.3절 표).
    template <class... Arguments>
    void Set(Arguments&&...) const noexcept {
        static_assert(
            detail::DependentFalse<Arguments...>,
            "BootCVar is fixed at boot; set it in a config file or with --set and restart");
    }

    template <class... Arguments>
    void Clear(Arguments&&...) const noexcept {
        static_assert(
            detail::DependentFalse<Arguments...>,
            "BootCVar is fixed at boot; set it in a config file or with --set and restart");
    }

private:
    friend struct detail::HandleAccess;
    friend class BootVarBuilder<T>;

    constexpr explicit BootCVar(const detail::TypedDefinition<T>& definition) noexcept
        : definition_(definition) {
    }

    detail::TypedDefinition<T> definition_;
};

namespace detail {

template <class T>
inline constexpr bool IsHandle<CVar<T>> = true;

template <class T>
inline constexpr bool IsHandle<BootCVar<T>> = true;

} // namespace detail

} // namespace rocks::cvar
