#pragma once

#include <Base/Compiler.h>
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

// 모듈을 자동으로 시작하는 시점이다(UE ELoadingPhase의 일부, 기술자 loadingPhase와 같은 값·순서 —
// Docs/Design/ModuleLoader.ko.md 3.3·3.11절). 빌드가 모듈 표 행에 싣는다. 기술자
// 모듈(게임·플러그인)은 기술자의 값, 엔진 모듈은 규칙의 LoadingPhase(기본 Default)다. 로더의
// StartPhase가 앞 단계부터 차례로 돈다. 값 순서가 단계 순서다.
enum class LoadingPhase : uint8 {
    // 로더 생성 직후(Launch가 부트 모듈과 함께 시작한다). 프로세스 묶음.
    EarliestPossible,
    // HostSettings가 설정을 확정하고 LogHost가 시작한 뒤. 호스트 묶음.
    PostConfigInit,
    // Engine::Initialize 안, 엔진 TypeRegistry를 만든 뒤(아래 셋). 엔진 묶음.
    PreDefault,
    Default,
    PostDefault,
    // EngineClient::OnEngineStart 뒤, 첫 프레임 전. 엔진 묶음.
    PostEngineInit,
    // 자동으로 시작하지 않는다. 코드가 ModuleLoader::Load로 시작한다.
    None
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
    case LoadingPhase::PostEngineInit:
        return "PostEngineInit";
    case LoadingPhase::None:
        return "None";
    }

    return "Unknown";
}

// ModuleEntry 구조체의 ABI 버전이다. 칸을 바꾸면 올린다. 로더는 다른 버전의 진입 기술을 거부한다.
// 2: unregisterTypes를 뺐다(모듈별 ModuleRegistration이 되돌린다 —
// Docs/Design/ModuleLoader.ko.md 3.10절).
inline constexpr uint32 ModuleEntryVersion = 2;

// 모듈 하나의 상수 진입 기술이다. ROCKS_IMPLEMENT_MODULE이 모듈 cpp에 상수로 정의한다(정적 수명,
// 읽기 전용). 직접 만들지 않는다.
struct ModuleEntry {
    // 수명 객체를 allocator로 만든다. 실패는 allocator의 오류(OutOfMemory 등)다. 돌려준 객체는
    // 호출자(로더)가 소유한다.
    using CreateFunction =
        Result<UniquePtr<ModuleImplementation>> (*)(Allocator& allocator) noexcept;
    // 이 모듈 자신의 리플렉션 타입만 registration에 등록하는 생성 함수다(빌드 생성 TU의
    // rocks::generated::<Name>::RegisterModuleOwnTypes, 3.10절). lease는 로더가 발급한 이 모듈의
    // 코드 수명 토큰이며 등록한 타입의 RuntimeTypeOps.module에 들어간다. 실패하면 이 호출이 등록한
    // 것을 되돌린다. 의존 모듈의 타입은 등록하지 않는다(로더가 의존 모듈을 먼저 등록한다). 해제는
    // registration의 Unregister다. 리플렉션 메타데이터가 없는 모듈은 null이다.
    using RegisterTypesFunction = Result<void> (*)(ModuleRegistration& registration,
                                                   const ModuleLease& lease) noexcept;
    // 수명 객체 클래스의 static RegisterSettings다(3.1절). 없으면 null이다.
    using RegisterSettingsFunction = Result<void> (*)(config::ConfigContext& context) noexcept;

    uint32 entryVersion = ModuleEntryVersion;
    // 모듈 이름(NUL 종료 ASCII 식별자)이다. 빌드의 모듈 이름과 같다.
    const char* name = nullptr;
    CreateFunction create = nullptr;
    RegisterTypesFunction registerTypes = nullptr;
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
    // 요청 시 적재 행이다(실행 파일의 링크 의존으로는 닿지 않고 DynamicModules 관계로만 표에 든
    // 모듈 — Docs/Design/ModuleLoader.ko.md 3.8절). 단계 실행(StartPhase)이 건너뛰고 코드의
    // Load(이름)으로만 시작한다(UE LoadingPhase None과 같다).
    bool onRequest = false;
};

// 모듈 표다. 행 배열은 정적 수명이다.
using ModuleTable = Span<const ModuleTableRow>;

#if ROCKS_MONOLITHIC
// 이 실행 파일의 모듈 표다. 빌드가 실행 파일마다 만드는 생성 TU(<실행 파일>.ModuleTable.cpp)가
// 정의한다. 로더를 만드는 쪽(Launch·시험 runner)이 ModuleLoader::TryCreate에 넘긴다. Modular에는
// 없다 — 모듈 표는 manifest이고 Launch의 Modular 진입이 로더를 만든다
// (Docs/Design/ModuleLoader.ko.md 3.14절). 그래서 Modular에서 부르면 컴파일 오류다.
ModuleTable GeneratedModuleTable() noexcept;
#endif

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
consteval ModuleEntry
MakeModuleEntry(const char* name,
                ModuleEntry::RegisterTypesFunction registerTypes = nullptr) noexcept {
    static_assert(IsBaseOfV<ModuleImplementation, Class>,
                  "ROCKS_IMPLEMENT_MODULE class must derive from rocks::ModuleImplementation");
    static_assert(IsNothrowDefaultConstructibleV<Class>,
                  "ROCKS_IMPLEMENT_MODULE class needs a noexcept default constructor");
    ModuleEntry entry;
    entry.name = name;
    entry.create = &CreateModuleImplementation<Class>;
    entry.registerTypes = registerTypes;
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

// 빌드가 진입점 모듈의 소스마다 정의하는 값이다(ROCKS_MODULE_TYPES=1이면 그 모듈에 리플렉션
// 메타데이터가 있고, 메타 컴파일러가 낸 모듈 생성 소스 <Name>.module.generated.cpp가
// RegisterModuleOwnTypes를 정의한다 — Docs/Design/ModuleLoader.ko.md 3.10절). 빌드 밖 컴파일은
// 0이다.
#ifndef ROCKS_MODULE_TYPES
#define ROCKS_MODULE_TYPES 0
#endif

#define ROCKS_MODULE_SELECT_EXPANDED(prefix, flag) prefix##flag
#define ROCKS_MODULE_SELECT(prefix, flag) ROCKS_MODULE_SELECT_EXPANDED(prefix, flag)

// 타입 등록 함수의 선언(ROCKS_MODULE_TYPES=1)이나 아무것도 아님(0)으로 펼쳐진다.
#define ROCKS_MODULE_TYPES_DECLARATION_0(Name)
#define ROCKS_MODULE_TYPES_DECLARATION_1(Name)                                                     \
    namespace rocks::generated::Name {                                                             \
    ::rocks::Result<void> RegisterModuleOwnTypes(::rocks::ModuleRegistration& registration,        \
                                                 const ::rocks::ModuleLease& module) noexcept;     \
    }

// 진입 기술 registerTypes 칸의 값이다.
#define ROCKS_MODULE_TYPES_FUNCTION_0(Name) nullptr
#define ROCKS_MODULE_TYPES_FUNCTION_1(Name) &::rocks::generated::Name::RegisterModuleOwnTypes

// 빌드가 진입점 모듈의 소스마다 정의하는 값이다(ROCKS_MODULE_SHARED_LIBRARY=1이면 Modular에서 공유
// 라이브러리로 컴파일한다 — Docs/Design/BuildTargets.ko.md 3.8절 BS8d1). 빌드 밖 컴파일은 0이다.
#ifndef ROCKS_MODULE_SHARED_LIBRARY
#define ROCKS_MODULE_SHARED_LIBRARY 0
#endif

// 공유 라이브러리마다 하나뿐인 export 진입점(ROCKS_MODULE_SHARED_LIBRARY=1)이나 아무것도
// 아님(0)으로 펼쳐진다. 모듈 로더가 OS로 적재한 라이브러리에서 이 이름으로 진입 기술을 찾는다
// (Docs/Design/ModuleLoader.ko.md 3.2절, UE GetDllExport("InitializeModule")와 같은 역할). 같은
// 자리에서 export하지 않는 모듈별 C 기호 RocksModuleEntryOf_<Name>도 정의하고 빌드가 링크에 그
// 기호를 강제 참조로 넣는다 — 매크로가 빠진 모듈은 링크 오류다(의존 모듈의 import 라이브러리도
// RocksModuleEntry를 export하므로 그 이름으로는 빠짐을 잡지 못한다).
#define ROCKS_MODULE_EXPORTED_ENTRY_0(Name)
#define ROCKS_MODULE_EXPORTED_ENTRY_1(Name)                                                        \
    extern "C" const ::rocks::ModuleEntry* RocksModuleEntryOf_##Name() noexcept;                   \
    extern "C" const ::rocks::ModuleEntry* RocksModuleEntryOf_##Name() noexcept {                  \
        return &RocksModuleEntry_##Name();                                                         \
    }                                                                                              \
    extern "C" ROCKS_DLLEXPORT const ::rocks::ModuleEntry* RocksModuleEntry() noexcept;            \
    extern "C" ROCKS_DLLEXPORT const ::rocks::ModuleEntry* RocksModuleEntry() noexcept {           \
        return RocksModuleEntryOf_##Name();                                                        \
    }

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
// - 리플렉션 메타데이터가 있는 모듈(빌드가 ROCKS_MODULE_TYPES=1로 알린다)은 모듈 생성 소스의
//   RegisterModuleOwnTypes를 진입 기술 registerTypes에 싣는다. 손으로 쓴 소스가 없는 메타데이터
//   모듈은 빌드 생성 TU <Name>.ModuleEntry.cpp가 이 매크로를 쓴다.
// - Modular에서 공유 라이브러리로 컴파일하면(빌드가 ROCKS_MODULE_SHARED_LIBRARY=1로 알린다)
//   export 진입점 extern "C" RocksModuleEntry()도 정의한다(공유 라이브러리마다 하나).
#define ROCKS_IMPLEMENT_MODULE(Class, Name) ROCKS_IMPLEMENT_MODULE_EXPANDED(Class, Name)

#define ROCKS_IMPLEMENT_MODULE_EXPANDED(Class, Name)                                               \
    static_assert(!::rocks::detail::ModuleNamesEqual(ROCKS_MODULE_NAME_TEXT, "ROCKS_MODULE_NAME"), \
                  "ROCKS_MODULE_NAME is not defined: ROCKS_IMPLEMENT_MODULE must be compiled "     \
                  "as part of a module built by Rocks.Build");                                     \
    static_assert(::rocks::detail::ModuleNamesEqual(#Name, ROCKS_MODULE_NAME_TEXT),                \
                  "ROCKS_IMPLEMENT_MODULE name does not match the module being built "             \
                  "(ROCKS_MODULE_NAME)");                                                          \
    ROCKS_MODULE_SELECT(ROCKS_MODULE_TYPES_DECLARATION_, ROCKS_MODULE_TYPES)(Name)                 \
        const ::rocks::ModuleEntry&                                                                \
        RocksModuleEntry_##Name() noexcept;                                                        \
    const ::rocks::ModuleEntry& RocksModuleEntry_##Name() noexcept {                               \
        static constexpr ::rocks::ModuleEntry entry = ::rocks::detail::MakeModuleEntry<Class>(     \
            #Name, ROCKS_MODULE_SELECT(ROCKS_MODULE_TYPES_FUNCTION_, ROCKS_MODULE_TYPES)(Name));   \
        return entry;                                                                              \
    }                                                                                              \
    ROCKS_MODULE_SELECT(ROCKS_MODULE_EXPORTED_ENTRY_, ROCKS_MODULE_SHARED_LIBRARY)(Name)
