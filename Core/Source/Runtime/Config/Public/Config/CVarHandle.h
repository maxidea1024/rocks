#pragma once

#include <Base/Compiler.h>
#include <Config/CVarDefinition.h>
#include <Core/Diagnostics/SourceLocation.h>
#include <Core/Result.h>
#include <Platform/Atomics.h>
#include <Platform/Memory.h>

// CVar 핸들 타입이다(Docs/Design/CVarSystem.ko.md 4절 — CVR1). 핸들은 빌더(Config/CVarBuilder.h)의
// consteval 변환으로만 만들어지는 상수 초기화 값이다: 전역 생성자가 없고 정적 초기화 순서 문제가
// 없다. 사용자는 Config/CVar.h를 include한다.
//
// 읽기 `Get()`은 이 스레드에 묶인 snapshot(Config/ConfigSnapshot.h)의 번호 칸을 읽는다(7.1절 —
// 잠금·할당·문자열 연산 없음). 쓰기 `Set`·`Clear`는 어느 스레드에서나 부르고, 바로 검사한 뒤 문맥의
// 쓰기 큐에 넣는다(적용은 다음 Sync — 6.2절).
namespace rocks::cvar {

// `Set`·`Clear`의 선택 사항이다(6.2절). layer는 쓸 층(기본 Code — CodeDefault는 받지 않는다), tag는
// 그 층 안의 묶음 이름이다(같은 (층, 태그)의 이전 `Set`을 바꾸고, `Clear`는 그 자리를 지운다).
// reason이 있으면 `InvalidArgument`로 거부할 때 그 이유를 쓴다(ValueOutOfRange·ValidationFailed·
// InvalidEnum·ConsoleReadOnly·LayerNotAllowed 등 — 통과·보정이면 그대로 둔다, 콘솔 도구가 쓴다).
// 범위(scope)는 CVR3에서 더한다.
struct SetOptions {
    config::ConfigLayer layer = config::ConfigLayer::Code;
    // 범위 값을 쓸 범위다(8절 — Global이면 층 스택에 쓴다). 범위 쓰기는 층 스택에 들지 않고,
    // layer는 그 값의 출처 층(권한 판정·표시)이다.
    CVarScopeId scope{};
    StringView tag{};
    config::ConfigDiagnosticCode* reason = nullptr;
};

// `Set`의 값 인자 타입이다(String·Name은 StringView — Name은 적용 때 문맥의 NamePool에 intern한다).
template <class T>
using SetArg = ConditionalT<IsTextVarType<T>, StringView, T>;

// `Get()`의 반환 타입이다(String은 StringView — 묶인 snapshot이 바뀔 때까지 유효, 7.1절).
template <class T>
using GetResult = ConditionalT<IsSameV<T, String>, StringView, T>;

namespace detail {

// 묶음 없는 읽기다(Fatal — 조용히 최신 값을 읽지 않는다, 7.1절).
[[noreturn]] ROCKS_CONFIG_API void FatalUnbound(const VarDefinition& definition) noexcept;

// 게임 lane에서 등록되지 않은 핸들을 읽었다(Fatal — 5절).
[[noreturn]] ROCKS_CONFIG_API void FatalUnregistered(const VarDefinition& definition) noexcept;

// 봉인(CompleteStartup) 전에 BootCVar를 읽었다(Debug·Development Fatal — 초기화 순서 버그, 9절).
[[noreturn]] ROCKS_CONFIG_API void FatalUnsealed(const VarDefinition& definition) noexcept;

// 이 스레드에 묶인 문맥의 쓰기 큐에 (layer, tag)의 값 쓰기를 넣는다(6.2절). value는 범위·Validate로
// 바로 검사한다(Clamp는 보정한다). 오류: 묶음 없음 NotFound, 출시 빌드 치트 변수 Unsupported, 층·
// 범위·Validate 위반 InvalidArgument, 문맥이 없어졌으면 ShuttingDown, 큐가 가득 차면 Busy, 할당
// 실패.
Result<void> SubmitSet(const VarDefinition& definition, ScalarValue value,
                       const SetOptions& options, SourceLocation where) noexcept;

// 이 스레드에 묶인 문맥의 쓰기 큐에 (layer, tag)의 값 지우기를 넣는다. 오류: SubmitSet과 같다(값
// 검사 없음).
Result<void> SubmitClear(const VarDefinition& definition, const SetOptions& options) noexcept;

// 칸이 없을 때의 값이다(정의의 기본값 — Name은 pool 토큰을 상수로 담을 수 없으므로 None).
template <class T>
GetResult<T> FallbackOf(const VarDefinition& definition) noexcept {
    if constexpr (IsSameV<T, rocks::Name>) {
        return rocks::Name{};
    } else {
        return ValueOf<T>(definition.defaultValue);
    }
}

// 칸의 값이다.
template <class T>
GetResult<T> ReadSlot([[maybe_unused]] const config::CVarText* text,
                      const config::CVarSlot& slot) noexcept {
    if constexpr (IsSameV<T, String>) {
        return text->text.View();
    } else if constexpr (IsSameV<T, rocks::Name>) {
        return text->name;
    } else if constexpr (IsSameV<T, bool>) {
        return slot.bits != 0;
    } else if constexpr (IsSameV<T, float> || IsSameV<T, double>) {
        return static_cast<T>(platform::BitCast<double>(slot.bits));
    } else {
        return static_cast<T>(static_cast<int64>(slot.bits));
    }
}

// 이 스레드에 묶인 snapshot에서 정의의 값을 읽는다(7.1절 경로 — 묶음 → 조각 표 → 조각 → 칸). 번호가
// 없으면 게임 lane에서는 Fatal, 다른 스레드에서는 기본값이다(5절). 이 snapshot에 칸이 없으면
// 기본값이다. Boot면 부팅 블록을 읽는다(9절 — 봉인 전 snapshot이면 Debug·Development Fatal, 출시
// 빌드는 일반 칸을 읽는다).
template <class T, bool Boot = false>
GetResult<T> ReadBound(const VarDefinition& definition) noexcept {
    if constexpr (IsReleaseBuild) {
        if (HasFlag(definition.flags, VarFlags::Cheat)) {
            return FallbackOf<T>(definition);
        }
    }

    const config::detail::ThreadBinding& binding = config::detail::CurrentBinding();
    if (binding.snapshot == nullptr) {
        FatalUnbound(definition);
    }

    const uint32 index = platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&definition.index);
    if (index == Index::None) {
        if (binding.gameLane != nullptr) {
            FatalUnregistered(definition);
        }

        return FallbackOf<T>(definition);
    }

    const config::ConfigSnapshot& snapshot = *binding.snapshot;
    if constexpr (Boot) {
        if (snapshot.IsSealed()) {
            const config::CVarSlot* slot = snapshot.FindBootSlot(index);
            if (slot == nullptr) {
                return FallbackOf<T>(definition);
            }

            if constexpr (IsTextVarType<T>) {
                return ReadSlot<T>(&snapshot.BootTextAt(index), *slot);
            } else {
                return ReadSlot<T>(nullptr, *slot);
            }
        }

        if constexpr (!IsReleaseBuild) {
            FatalUnsealed(definition);
        }
    }

    const config::CVarSlot* slot = snapshot.FindSlot(index);
    if (slot == nullptr) {
        return FallbackOf<T>(definition);
    }

    if constexpr (IsTextVarType<T>) {
        return ReadSlot<T>(&snapshot.TextAt(index), *slot);
    } else {
        return ReadSlot<T>(nullptr, *slot);
    }
}

// 이 스레드에 묶인 snapshot에서 범위 scope의 값을 읽는다(7.1·8절): 범위 사슬에서 처음 만난 범위
// 값, 없으면 ReadBound(Global 유효값)다. 묶음·번호 규칙은 ReadBound와 같다.
template <class T>
GetResult<T> ReadBoundScoped(const VarDefinition& definition, CVarScopeId scope) noexcept {
    if (scope.IsGlobal()) {
        return ReadBound<T>(definition);
    }

    if constexpr (IsReleaseBuild) {
        if (HasFlag(definition.flags, VarFlags::Cheat)) {
            return FallbackOf<T>(definition);
        }
    }

    const config::ConfigSnapshot* snapshot = config::detail::CurrentBinding().snapshot;
    const uint32 index = platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&definition.index);
    if (snapshot != nullptr && index != Index::None) {
        const config::CVarText* text = nullptr;
        if (const config::CVarSlot* slot = snapshot->FindScopedSlot(scope.value, index, text)) {
            return ReadSlot<T>(text, *slot);
        }
    }

    return ReadBound<T>(definition);
}

// `Set` 인자의 ScalarValue다.
template <class T>
constexpr ScalarValue ScalarOfArg(SetArg<T> value) noexcept {
    if constexpr (IsTextVarType<T>) {
        ScalarValue scalar;
        scalar.text = value;
        return scalar;
    } else {
        return ScalarOf<T>(value);
    }
}

} // namespace detail

namespace detail {

// T로 읽을 수 있는 칸 종류인지다(enum 칸은 int64로 읽는다 — 기저 정수).
template <class T>
constexpr bool IsReadableKind(ValueKind kind) noexcept {
    if constexpr (IsSameV<T, bool>) {
        return kind == ValueKind::Bool;
    } else if constexpr (IsSameV<T, int32>) {
        return kind == ValueKind::Int32;
    } else if constexpr (IsSameV<T, int64>) {
        return kind == ValueKind::Int64 || kind == ValueKind::Enum;
    } else if constexpr (IsSameV<T, float>) {
        return kind == ValueKind::Float32;
    } else if constexpr (IsSameV<T, double>) {
        return kind == ValueKind::Float64;
    } else if constexpr (IsSameV<T, String>) {
        return kind == ValueKind::Utf8String;
    } else {
        return kind == ValueKind::Name;
    }
}

// snapshot에서 번호 index의 칸을 찾는다(봉인 뒤 snapshot은 부팅 블록 먼저). boot는 부팅 블록
// 칸인지다. 없으면 null.
inline const config::CVarSlot* FindDynamicSlot(const config::ConfigSnapshot& snapshot, uint32 index,
                                               bool& boot) noexcept {
    boot = false;
    if (snapshot.IsSealed()) {
        if (const config::CVarSlot* slot = snapshot.FindBootSlot(index)) {
            boot = true;
            return slot;
        }
    }

    return snapshot.FindSlot(index);
}

} // namespace detail

// 동적 변수(ConfigContext::RegisterDynamic — CVar 5절)의 칸 값 종류다(Bool·Int32·Int64·Float32·
// Float64·Utf8String·Name·Enum). 이 snapshot에 칸이 없으면(등록 전 snapshot·무효 번호) Opaque다.
// 실행 중 타입으로 등록하는 쪽(Sparks)이 ReadDynamic의 T를 고르는 데 쓴다. 어느 스레드에서나
// 부른다.
inline ValueKind DynamicKind(const config::ConfigSnapshot& snapshot, Index index) noexcept {
    if (!index.IsValid()) {
        return ValueKind::Opaque;
    }

    bool boot = false;
    const config::CVarSlot* slot = detail::FindDynamicSlot(snapshot, index.value, boot);
    return slot != nullptr ? slot->kind : ValueKind::Opaque;
}

// 동적 변수(ConfigContext::RegisterDynamic — CVar 5절)의 값을 snapshot에서 읽는다. T는 등록한
// 타입(bool·int32·int64·float·double·String·Name)이고 enum 변수는 int64로 읽는다(값은 기저 정수).
// 칸의 값 종류가 T와 다르면(모든 빌드에서 확인한다) fallback이다. String은 snapshot이 살아 있는
// 동안 유효한 StringView다. 봉인 뒤 snapshot의 부팅 변수는 부팅 블록을 읽는다. 이 snapshot에 칸이
// 없으면(등록 전 snapshot·무효 번호) fallback이다. 어느 스레드에서나 부른다(snapshot은 불변이다).
//
// 사용 예:
//   auto pool = config.RegisterDynamic(definition);
//   const int32 megabytes = cvar::ReadDynamic<int32>(*config.Acquire(), pool.Value(), 512);
template <class T>
GetResult<T> ReadDynamic(const config::ConfigSnapshot& snapshot, Index index,
                         GetResult<T> fallback) noexcept {
    static_assert(IsSameV<T, bool> || IsNumericVarType<T> || IsTextVarType<T>,
                  "ReadDynamic reads bool, int32, int64, float, double, String or Name (enum "
                  "variables read as int64)");
    if (!index.IsValid()) {
        return fallback;
    }

    bool boot = false;
    const config::CVarSlot* slot = detail::FindDynamicSlot(snapshot, index.value, boot);
    if (slot == nullptr || !detail::IsReadableKind<T>(slot->kind)) {
        return fallback;
    }

    if constexpr (IsTextVarType<T>) {
        return detail::ReadSlot<T>(
            boot ? &snapshot.BootTextAt(index.value) : &snapshot.TextAt(index.value), *slot);
    } else {
        return detail::ReadSlot<T>(nullptr, *slot);
    }
}

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
// 수명: 프로그램 수명(정적 저장 기간 — namespace 범위 또는 static 멤버). 힙 생성(new)은 컴파일
// 오류이고, 함수 안 지역 핸들은 Register가 거부한다(Debug·Development). 비정적 멤버·thread_local·
// const 정의는 Policy cvar-registration이 거부한다.
// 스레드: 상수 부분(이름·기본값 등)은 어느 스레드에서나 읽는다. Index는 원자적으로 읽는다.
template <class T>
class CVar {
public:
    CVar(const CVar&) = delete;
    CVar& operator=(const CVar&) = delete;
    CVar(CVar&&) = delete;
    CVar& operator=(CVar&&) = delete;

    // 핸들은 정적 저장 기간이어야 하므로 힙에 만들 수 없다(해제 뒤 문맥이 정의를 가리키게 된다).
    static void* operator new(usize) = delete;
    static void* operator new[](usize) = delete;
    static void* operator new(usize, void*) = delete;
    static void* operator new[](usize, void*) = delete;

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

    // 이 스레드에 묶인 snapshot의 값이다(7.1절). 게임 lane은 문맥이 스스로
    // 묶고(Initialize·Register· Sync), 다른 스레드는 cvar::SnapshotBinding으로 묶는다. 한 스레드가
    // 여러 문맥의 게임 lane이면 가장 최근에 게시하거나 Sync한 문맥이 묶이므로, 다른 문맥을 읽으려면
    // SnapshotBinding을 쓴다. 한 프레임 동안 같은 스레드는 같은 값을 본다(값은 sync 지점에서만
    // 바뀐다). 잠금·할당·문자열 연산이 없다.
    // - 묶음이 없으면 Fatal이다.
    // - 어느 문맥에도 등록된 적 없는 핸들(번호 없음)은 게임 lane에서 Fatal, 다른 스레드에서는
    //   기본값이다(5절). 번호는 프로세스 전체이므로 다른 문맥에만 등록된 핸들은 게임 lane에서도
    //   Fatal이 아니라 기본값이다(이 문맥 snapshot에 칸이 없다).
    // - 묶인 snapshot에 칸이 없으면(그 snapshot 뒤에 등록됐다) 기본값이다. Name 변수의 기본값은
    //   None이다(pool 토큰을 상수로 담을 수 없다).
    // - String은 StringView이고 묶인 snapshot이 바뀔 때까지 유효하다(게임 lane은 다음 Sync·Register
    //   까지). 더 오래 쓰려면 복사한다.
    // - 출시 빌드의 `Cheat()` 변수는 늘 기본값이다(13.1절).
    //
    // 사용 예:
    //   const float distance = ShadowDistance.Get();
    GetResult<T> Get() const noexcept {
        return detail::ReadBound<T>(definition_);
    }

    // 범위 scope의 값이다(7.1·8절 — 범위 사슬을 자식 → 부모로 따라 처음 만난 범위 값, 없으면 Global
    // 유효값 `Get()`). 묶인 snapshot에 없는 범위(아직 게시되지 않았거나 파괴된 범위, 다른 문맥의
    // 범위)는 Global 값이다. Global 범위면 `Get()`과 같다. 규칙(묶음·번호·String 수명·출시 빌드의
    // `Cheat()`)은 `Get()`과 같다.
    //
    // 사용 예:
    //   const float distance = ShadowDistance.Get(world.CVarScope());
    GetResult<T> Get(CVarScopeId scope) const noexcept {
        return detail::ReadBoundScoped<T>(definition_, scope);
    }

    // 값을 쓴다(6.2절): 범위·Validate·층 권한을 바로 검사해 결과로 알리고(`Clamp()`는 보정한다),
    // 통과한 쓰기는 이 스레드에 묶인 문맥의 쓰기 큐에 넣는다. 적용은 다음 Sync이고 그때 다시
    // 검사한다. 같은 (층, 태그)의 이전 쓰기를 바꾼다. 출처는 호출 위치다. 어느 스레드에서나 부를 수
    // 있다.
    //
    // 사용 예:
    //   Result<void> written = ShadowDistance.Set(4000.f);
    //   ShadowDistance.Set(4000.f, {.layer = ConfigLayer::Console});
    //
    // 오류: 묶음 없음 NotFound(쓸 문맥을 모른다), 출시 빌드 치트 변수 Unsupported, CodeDefault·
    // 정의되지 않은 층·층 권한(콘솔 읽기 전용·사용자 설정 아님·출시 규칙의 치트)·범위(NaN·무한대
    // 포함)·Validate 위반·이름 표에 없는 enum 값 InvalidArgument(이유는 options.reason에 —
    // ValueOutOfRange·ValidationFailed·InvalidEnum·ConsoleReadOnly·LayerNotAllowed 등), 문맥이
    // 없어졌으면 ShuttingDown, 다른 스레드의 쓰기가 큐 한도(MaxQueuedChanges)를 넘으면 Busy(버리지
    // 않는다 — 소유 스레드의 쓰기에는 한도가 없다), 할당 실패.
    Result<void> Set(SetArg<T> value, const SetOptions& options = {},
                     SourceLocation where = SourceLocation::Current()) const noexcept {
        return detail::SubmitSet(definition_, detail::ScalarOfArg<T>(value), options, where);
    }

    // (layer, tag)에 쓴 값을 지운다(다음 Sync에 아래 층 값이 드러난다). 기본은 코드 층의 태그 없는
    // 쓰기다. 오류: Set과 같다(값 검사 없음).
    Result<void> Clear(const SetOptions& options = {}) const noexcept {
        return detail::SubmitClear(definition_, options);
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
// 정의한다. 값은 파일·`--set` 등 시작 전 층에서 정하고, ConfigContext::CompleteStartup(봉인) 뒤에는
// 이번 실행 값이 바뀌지 않는다: 이름으로 하는 콘솔·코드 쓰기는 넣을 때 Error BootSealed로 거부하고,
// 파일 층 변경은 재시작 대기, 파생·휘발 층 변경은 Warning BootValueIgnored다. `Set`·`Clear`가
// 없다(부르면 컴파일 오류).
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

    // 핸들은 정적 저장 기간이어야 하므로 힙에 만들 수 없다(해제 뒤 문맥이 정의를 가리키게 된다).
    static void* operator new(usize) = delete;
    static void* operator new[](usize) = delete;
    static void* operator new(usize, void*) = delete;
    static void* operator new[](usize, void*) = delete;

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

    // 이번 실행의 값이다(9절): 이 스레드에 묶인 snapshot의 부팅 블록을 읽는다(봉인 때 굳은 값 —
    // 봉인 뒤 snapshot의 부팅 값은 모두 같으므로 어느 스레드·프레임이든 같다). 묶인 snapshot이 봉인
    // 전 snapshot이면 Debug·Development에서 Fatal이다(초기화 순서 버그 — 봉인 전 호출뿐 아니라
    // 봉인을 넘겨 살아 있는 명시 SnapshotBinding이나 다른 스레드가 쥔 봉인 전 snapshot도 같다;
    // 출시 빌드는 그 snapshot의 지금 칸을 읽는다). 봉인 뒤에 등록한 부팅 변수는 등록 때 값으로
    // 굳는다. 묶음·등록 규칙은 CVar::Get과 같다.
    GetResult<T> Get() const noexcept {
        return detail::ReadBound<T, true>(definition_);
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
