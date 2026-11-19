#pragma once

#include <Base/TypeTraits.h>
#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Module/ModuleImplementation.h>
#include <Core/Move.h>
#include <Core/Result.h>
#include <Platform/Memory.h>

// 모듈 진입점 계약이다(Docs/Design/ModuleLoader.ko.md 3.1·3.2절, UE IMPLEMENT_MODULE·
// FModuleInitializerEntry 대응). 진입점은 전역 생성자 없이 상수 기술(ModuleEntry)과 그것을 돌려주는
// 함수(RocksModuleEntry_<Name>)다. 빌드는 실행 파일마다 모듈 표(ModuleTableRow 배열)를 만들어 이
// 함수를 모두 직접 참조하므로, 매크로가 빠진 모듈은 링크 오류가 난다. 로더(ModuleLoader 모듈)는 이
// 표로 모듈을 의존 순서대로 시작한다.
//
// 이 헤더는 Core에 있다. 모든 C++ 모듈(Core 자신 포함)이 매크로를 써야 하는데 로더 모듈은 Core에
// 의존하기 때문이다. 상위 모듈의 형식(ConfigContext·ModuleRegistration)은 전방 선언만 한다.

namespace rocks {

class Allocator;
class ModuleLease;
class ModuleRegistration;

namespace config {

class ConfigContext;

} // namespace config

// 모듈을 자동으로 시작하는 시점이다(UE ELoadingPhase의 일부, 기술자 loadingPhase와 같은 값·순서).
// 빌드가 모듈 표 행에 싣는다. 기술자 모듈(게임·플러그인)은 기술자의 값, 엔진 모듈은 Default다.
// 단계 실행(StartPhase)과 PostEngineInit·None은 ML1에 더한다.
enum class LoadingPhase : uint8 {
    EarliestPossible,
    PostConfigInit,
    PreDefault,
    Default,
    PostDefault
};

// 진단용 단계 이름이다(예: LoadingPhase::Default → "Default"). 모르는 값은 "Unknown"이다. 반환
// 문자열은 정적 수명이다.
constexpr const char* LoadingPhaseName(LoadingPhase phase) noexcept {
    switch (phase) {
    case LoadingPhase::EarliestPossible:
        return "EarliestPossible";
    case LoadingPhase::PostConfigInit:
        return "PostConfigInit";
    case LoadingPhase::PreDefault:
        return "PreDefault";
    case LoadingPhase::Default:
        return "Default";
    case LoadingPhase::PostDefault:
        return "PostDefault";
    }

    return "Unknown";
}

// ModuleEntry 구조체의 ABI 버전이다. 칸을 바꾸면 올린다. 로더는 다른 버전의 진입 기술을 거부한다.
inline constexpr uint32 ModuleEntryVersion = 1;

// 모듈 하나의 상수 진입 기술이다. ROCKS_IMPLEMENT_MODULE이 모듈 cpp에 상수로 정의한다(정적 수명,
// 읽기 전용). 직접 만들지 않는다.
struct ModuleEntry {
    // 수명 객체를 allocator로 만든다. 실패는 allocator의 오류(OutOfMemory 등)다. 돌려준 객체는
    // 호출자(로더)가 소유한다.
    using CreateFunction =
        Result<UniquePtr<ModuleImplementation>> (*)(Allocator& allocator) noexcept;
    // 이 모듈 자신의 리플렉션 타입만 등록·해제하는 생성 함수다(3.6절, ML2에서 채운다. 그때까지
    // null).
    using RegisterTypesFunction = Result<void> (*)(ModuleRegistration& registration,
                                                   const ModuleLease& lease) noexcept;
    using UnregisterTypesFunction = void (*)(ModuleRegistration& registration) noexcept;
    // 수명 객체 클래스의 static RegisterSettings다(3.1절). 없으면 null이다.
    using RegisterSettingsFunction = Result<void> (*)(config::ConfigContext& context) noexcept;

    uint32 entryVersion = ModuleEntryVersion;
    // 모듈 이름(NUL 종료 ASCII 식별자)이다. 빌드의 모듈 이름과 같다.
    const char* name = nullptr;
    CreateFunction create = nullptr;
    RegisterTypesFunction registerTypes = nullptr;
    UnregisterTypesFunction unregisterTypes = nullptr;
    RegisterSettingsFunction registerSettings = nullptr;
};

// RocksModuleEntry_<Name> 함수의 형식이다. 돌려준 참조는 프로그램 수명 동안 유효하다.
using ModuleEntryFunction = const ModuleEntry& (*)() noexcept;

// 빌드가 실행 파일마다 만드는 모듈 표의 행 하나다(Monolithic manifest, 3.2절). 표는 의존 순서다:
// 모든 의존 모듈은 앞 행에 있다. 모든 칸은 생성 TU의 정적 상수를 가리킨다.
struct ModuleTableRow {
    // 모듈 이름(NUL 종료 ASCII 식별자)이다.
    const char* name = nullptr;
    // 그 모듈의 RocksModuleEntry_<Name>이다.
    ModuleEntryFunction entry = nullptr;
    // 직접 의존 모듈 이름 dependencyCount개다(표에 든 모듈만, 면제 모듈을 거치는 의존은 그 너머의
    // 표 모듈로 바꿔 실었다). 없으면 null이다.
    const char* const* dependencies = nullptr;
    usize dependencyCount = 0;
    LoadingPhase phase = LoadingPhase::Default;
};

// 모듈 표다. 행 배열은 정적 수명이다.
using ModuleTable = Span<const ModuleTableRow>;

// 이 실행 파일의 모듈 표다. 빌드가 실행 파일마다 만드는 생성 TU(<실행 파일>.ModuleTable.cpp)가
// 정의한다. 로더를 만드는 쪽(Launch·시험 runner)이 ModuleLoader::TryCreate에 넘긴다.
ModuleTable GeneratedModuleTable() noexcept;

namespace detail {

// 진입점 매크로가 쓰는 수명 객체 생성 함수다.
template <class Class>
Result<UniquePtr<ModuleImplementation>> CreateModuleImplementation(Allocator& allocator) noexcept {
    using Created = Result<UniquePtr<ModuleImplementation>>;
    auto created = UniquePtr<Class>::TryMake(allocator);
    if (!created) {
        return Created::Failure(created.Error());
    }

    if constexpr (IsSameV<Class, ModuleImplementation>) {
        return Created::Success(Move(created.Value()));
    } else {
        return Created::Success(UniquePtr<ModuleImplementation>(Move(created.Value())));
    }
}

// RegisterSettings 이름 감지용 기반이다. 탐침 ModuleSettingsNameProbe<Class>가 Class와 이것을 함께
// 상속하므로 Class에도 그 이름이 있으면 이름 조회가 모호해진다.
struct ModuleSettingsNameFallback {
    int RegisterSettings = 0;
};

template <class Class>
struct ModuleSettingsNameProbe : Class, ModuleSettingsNameFallback {};

// Class(와 그 기반)에 RegisterSettings라는 이름이 있는지다. 접근 권한·오버로드·정적 여부와
// 무관하다: 이름 조회가 접근 검사보다 먼저이므로 private이거나 오버로드여도 탐침의 조회가
// 모호해진다(requires 식이 거짓). final 클래스는 탐침이 상속할 수 없으므로 public 이름만 본다(한계
// — Docs/Design/ModuleLoader.ko.md 3.1절).
template <class Class>
consteval bool DeclaresRegisterSettings() noexcept {
    if constexpr (IsFinalV<Class>) {
        return requires { &Class::RegisterSettings; } ||
               requires(config::ConfigContext& context) { Class::RegisterSettings(context); };
    } else {
        return !requires { &ModuleSettingsNameProbe<Class>::RegisterSettings; };
    }
}

// Class::RegisterSettings가 public 단일 정적 함수이고 서명이
// ModuleEntry::RegisterSettingsFunction인지다.
template <class Class>
consteval bool HasUsableRegisterSettings() noexcept {
    if constexpr (requires { &Class::RegisterSettings; }) {
        return IsSameV<decltype(&Class::RegisterSettings), ModuleEntry::RegisterSettingsFunction>;
    } else {
        return false;
    }
}

// 수명 객체 클래스에 static RegisterSettings가 있으면 그 주소, 이름이 없으면 null이다. 이름은
// 있는데 주소로 쓸 수 없으면(private·protected, 오버로드, 비정적, 다른 서명) 설정 등록이 조용히
// 빠지지 않게 static_assert 오류다.
template <class Class>
consteval ModuleEntry::RegisterSettingsFunction ModuleSettingsRegistration() noexcept {
    if constexpr (DeclaresRegisterSettings<Class>()) {
        static_assert(HasUsableRegisterSettings<Class>(),
                      "RegisterSettings must be a public, non-overloaded static member declared as "
                      "static Result<void> RegisterSettings(config::ConfigContext&) noexcept");
        if constexpr (HasUsableRegisterSettings<Class>()) {
            return &Class::RegisterSettings;
        } else {
            return nullptr;
        }
    } else {
        return nullptr;
    }
}

template <class Class>
consteval ModuleEntry MakeModuleEntry(const char* name) noexcept {
    static_assert(IsBaseOfV<ModuleImplementation, Class>,
                  "ROCKS_IMPLEMENT_MODULE class must derive from rocks::ModuleImplementation");
    static_assert(IsNothrowDefaultConstructibleV<Class>,
                  "ROCKS_IMPLEMENT_MODULE class needs a noexcept default constructor");
    ModuleEntry entry;
    entry.name = name;
    entry.create = &CreateModuleImplementation<Class>;
    entry.registerSettings = ModuleSettingsRegistration<Class>();
    return entry;
}

// NUL 종료 문자열 둘이 같은지다(컴파일 시점 이름 비교용).
constexpr bool ModuleNamesEqual(const char* left, const char* right) noexcept {
    const usize length = platform::CStringLength(left);
    if (length != platform::CStringLength(right)) {
        return false;
    }

    for (usize index = 0; index < length; ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }

    return true;
}

} // namespace detail

} // namespace rocks

#define ROCKS_MODULE_STRINGIZE_EXPANDED(value) #value
#define ROCKS_MODULE_STRINGIZE(value) ROCKS_MODULE_STRINGIZE_EXPANDED(value)

// 빌드가 모듈 소스마다 정의하는 모듈 이름(ROCKS_MODULE_NAME=<이름>)의 문자열이다. 빌드 밖에서
// 컴파일해 정의가 없으면 "ROCKS_MODULE_NAME" 그대로다.
#define ROCKS_MODULE_NAME_TEXT ROCKS_MODULE_STRINGIZE(ROCKS_MODULE_NAME)

// 모듈 진입점을 정의한다(UE IMPLEMENT_MODULE 대응). 모듈의 cpp 하나에 전역 namespace에서 한 번
// 쓴다. Class는 ModuleImplementation 파생 클래스(noexcept 기본 생성자), Name은 빌드의 모듈
// 이름이다.
//
// 사용 예(Core/Source/Runtime/Curves/Private/CurvesModule.cpp):
//   #include <Core/Module/ModuleEntry.h>
//   ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, Curves)
//
// - 상수 기술 ModuleEntry와 함수 RocksModuleEntry_<Name>()을 정의한다. 전역 생성자는 없다.
// - Name이 빌드가 넣은 ROCKS_MODULE_NAME과 다르면 static_assert 오류다. Name은 한 번
//   펼쳐지므로 여러 실행 파일이 함께 컴파일하는 소스(시험 runner)는 Name에 ROCKS_MODULE_NAME을
//   쓴다.
// - 한 모듈에 두 번 쓰면 같은 기호가 겹쳐 링크 오류다. 빠뜨리면 모듈 표가 그 함수를 찾지 못해 링크
//   오류다.
// - Modular의 export 진입점 RocksModuleEntry()는 ML4에서 이 매크로에 더한다.
#define ROCKS_IMPLEMENT_MODULE(Class, Name) ROCKS_IMPLEMENT_MODULE_EXPANDED(Class, Name)

#define ROCKS_IMPLEMENT_MODULE_EXPANDED(Class, Name)                                               \
    static_assert(!::rocks::detail::ModuleNamesEqual(ROCKS_MODULE_NAME_TEXT, "ROCKS_MODULE_NAME"), \
                  "ROCKS_MODULE_NAME is not defined: ROCKS_IMPLEMENT_MODULE must be compiled "     \
                  "as part of a module built by Rocks.Build");                                     \
    static_assert(::rocks::detail::ModuleNamesEqual(#Name, ROCKS_MODULE_NAME_TEXT),                \
                  "ROCKS_IMPLEMENT_MODULE name does not match the module being built "             \
                  "(ROCKS_MODULE_NAME)");                                                          \
    const ::rocks::ModuleEntry& RocksModuleEntry_##Name() noexcept;                                \
    const ::rocks::ModuleEntry& RocksModuleEntry_##Name() noexcept {                               \
        static constexpr ::rocks::ModuleEntry entry =                                              \
            ::rocks::detail::MakeModuleEntry<Class>(#Name);                                        \
        return entry;                                                                              \
    }
