#pragma once

#include <Config/CVarHandle.h>
#include <Config/ConfigSnapshot.h>

// CVar 읽기·쓰기 보조다(Docs/Design/CVarSystem.ko.md 6.3·7.1·7.3절 — CVR2): 스레드 묶음
// `SnapshotBinding`, 변경 감지 `Watcher`, 범위 임시 변경 `ScopedOverride`. 사용자는 Config/CVar.h를
// include한다.
namespace rocks::cvar {

namespace detail {

// 범위 임시 변경 태그의 byte 한도다(`scoped.<번호>` — 번호는 64-bit 십진수).
inline constexpr usize ScopedTagBytes = 32;

// 프로세스 전체에서 겹치지 않는 범위 임시 변경 태그를 tag에 쓰고 그 view를 돌려준다.
ROCKS_CONFIG_API StringView MakeScopedTag(char (&tag)[ScopedTagBytes]) noexcept;

// 범위 임시 변경을 끝낼 지우기다. 생성 때 미리 만들고 큐 자리도 잡아 두므로 소멸에 실패 경로가
// 없다. channel은 값을 쓴 문맥의 통로다(소멸 때 묶인 문맥과 무관하게 이 문맥의 값을 지운다).
struct ScopedRemoval {
    SharedPtr<config::detail::WriteChannel> channel;
    SharedPtr<config::detail::QueuedChange> change;
};

// 이 스레드에 묶인 문맥의 Scoped 층 (tag)에 값을 쓰고, 끝낼 때 넣을 지우기를 removal에 만든다.
// 오류: CVar::Set과 같다(실패하면 removal은 비어 있다).
ROCKS_CONFIG_API Result<void> BeginScopedOverride(const VarDefinition& definition,
                                                  const ScalarValue& value, StringView tag,
                                                  SourceLocation where,
                                                  ScopedRemoval& removal) noexcept;

// 범위 임시 변경을 끝낸다: 미리 만든 지우기를 그 문맥의 큐에 넣는다(큐 한도와 무관 — 실패하지
// 않는다). 문맥이 이미 없어졌으면 할 일이 없다. 어느 스레드에서나 부를 수 있다.
ROCKS_CONFIG_API void EndScopedOverride(ScopedRemoval& removal) noexcept;

} // namespace detail

// 이 스레드를 snapshot에 묶는다(7.1절). 렌더 lane·worker·도구 스레드가 `CVar::Get()`을 부르기 전에
// 만든다(게임 lane은 문맥이 스스로 묶는다). 범위가 끝나면 앞 묶음으로 되돌린다(중첩은 LIFO여야
// 한다). 묶음은 snapshot 참조 하나를 쥐므로 snapshot이 먼저 사라지지 않는다. 문맥은 살아 있는 묶음
// 수를 세고, 묶음이 남은 채 문맥을 없애면 Fatal이다(10.1절).
//
// 사용 예:
//   cvar::SnapshotBinding bind{config.Acquire()};
//   const float distance = ShadowDistance.Get();
//
// 전제: snapshot이 null이 아니다(아니면 Fatal). 스레드: 만든 스레드에서 없애야 한다(아니면 Fatal).
class ROCKS_CONFIG_API SnapshotBinding {
public:
    explicit SnapshotBinding(SharedPtr<const config::ConfigSnapshot> snapshot) noexcept;
    ~SnapshotBinding();

    SnapshotBinding(const SnapshotBinding&) = delete;
    SnapshotBinding& operator=(const SnapshotBinding&) = delete;
    SnapshotBinding(SnapshotBinding&&) = delete;
    SnapshotBinding& operator=(SnapshotBinding&&) = delete;

    // 묶은 snapshot이다.
    const config::ConfigSnapshot& Snapshot() const noexcept {
        return *snapshot_;
    }

private:
    SharedPtr<const config::ConfigSnapshot> snapshot_;
    config::detail::ThreadBinding previous_;
    // 앞 묶음이 게임 lane 자동 묶음이면 그 문맥의 통로다(되돌릴 때 그 문맥의 최신 snapshot에 묶는다
    // — 그 사이 Sync가 앞 snapshot을 바꿨을 수 있다).
    SharedPtr<config::detail::WriteChannel> previousGameLane_;
    uint64 thread_ = 0;
    // 이 묶음의 중첩 깊이(1부터 — 풀 때 LIFO를 확인한다).
    uint32 depth_ = 0;
};

// 변수 값의 세대가 바뀌었는지 묻는 감시자다(7.3절 — UE FConsoleVariableSinkHandle 대응). 마지막으로
// 본 세대를 기억한다. 첫 호출은 true다.
//
// 사용 예:
//   cvar::Watcher<float> distanceWatcher{ShadowDistance};
//   if (distanceWatcher.Changed()) { RebuildCascades(ShadowDistance.Get()); }
//
// 스레드: 감시자 하나는 한 스레드에서 쓴다(본 세대를 고친다).
template <class T>
class Watcher {
public:
    explicit Watcher(const CVar<T>& handle) noexcept
        : definition_(&detail::HandleAccess::Definition(handle)) {
    }

    // 이 스레드에 묶인 snapshot에서 세대가 바뀌었는지다(묶음이 없으면 Fatal).
    bool Changed() noexcept {
        const config::ConfigSnapshot* snapshot = config::detail::CurrentBinding().snapshot;
        if (snapshot == nullptr) {
            detail::FatalUnbound(*definition_);
        }

        return Changed(*snapshot);
    }

    // snapshot에서 세대가 바뀌었는지다. 바뀌었으면 본 세대를 갱신한다(칸이 없으면 세대 0으로 본다).
    bool Changed(const config::ConfigSnapshot& snapshot) noexcept {
        const uint32 index =
            platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&definition_->index);
        const config::CVarSlot* slot = index != Index::None ? snapshot.FindSlot(index) : nullptr;
        const uint32 generation = slot != nullptr ? slot->generation : 0;
        if (seen_ && generation == generation_) {
            return false;
        }

        seen_ = true;
        generation_ = generation;
        return true;
    }

    // 본 세대를 잊는다(다음 Changed가 true다). 바뀐 값의 적용이 일시적으로 실패해 다음 sync 지점에
    // 다시 시도할 때 쓴다.
    void Reset() noexcept {
        seen_ = false;
    }

private:
    const detail::VarDefinition* definition_;
    uint32 generation_ = 0;
    bool seen_ = false;
};

template <class T>
Watcher(const CVar<T>&) -> Watcher<T>;

// 범위 임시 변경이다(6.3절 — UE TGuardConsoleVariable 대응). Scoped 층에 고유 태그로 쓰고 소멸 때
// 그 태그만 지운다. 중첩하면 나중 것이 이기고, 어떤 순서로 풀어도 남은 것 중 가장 나중 것이 이긴다.
// 적용은 다른 쓰기와 같이 Sync에서 한다. Scoped 층은 콘솔보다 약하다(6.1절). BootCVar는 받지 않는다
// (컴파일 오류). 값은 만들 때 이 스레드에 묶인 문맥에 쓰고, 지우기는 소멸 때 묶음과 무관하게 그
// 문맥에 간다(다른 문맥의 묶음 안·다른 스레드에서 소멸해도 된다). 지우기는 만들 때 미리 만들고 큐
// 자리도 잡아 두므로 소멸에 실패 경로가 없다. 그 문맥이 먼저 없어졌으면 소멸은 할 일이 없다.
//
// 사용 예:
//   {
//       cvar::ScopedOverride shadows{ShadowDistance, 500.f};
//       config.Sync();
//       CHECK(ShadowDistance.Get() == 500.f);
//   }
//
// 수명: 쓰기가 실패했으면(Status) 소멸 때 할 일이 없다. 스레드: 만들 때 이 스레드에 묶인 문맥에
// 쓴다.
class CVarGroup;

template <class T>
class ScopedOverride {
public:
    ScopedOverride(const CVar<T>& handle, SetArg<T> value,
                   SourceLocation where = SourceLocation::Current()) noexcept {
        const StringView tag = detail::MakeScopedTag(tag_);
        auto written =
            detail::BeginScopedOverride(detail::HandleAccess::Definition(handle),
                                        detail::ScalarOfArg<T>(value), tag, where, removal_);
        error_ = written ? ErrorCode::None : written.Error();
    }

    // 프리셋 묶음의 등급을 범위 임시 변경한다(6.3절 — 묶음 변수는 int32 등급이다, CVR3). 그 동안
    // 묶음의 유효 등급이 바뀌고 다음 Sync가 그 등급을 펼친다. 정의되지 않은 등급은 InvalidArgument
    // (이유 UndefinedLevel)다.
    //
    // 사용 예:
    //   cvar::ScopedOverride low{ShadowQuality, 1};
    template <class Group>
        requires(IsSameV<T, int32> && IsSameV<Group, CVarGroup>)
    ScopedOverride(const Group& group, int32 level,
                   SourceLocation where = SourceLocation::Current()) noexcept {
        const StringView tag = detail::MakeScopedTag(tag_);
        auto written =
            detail::BeginScopedOverride(detail::HandleAccess::Definition(group),
                                        detail::ScalarOf<int32>(level), tag, where, removal_);
        error_ = written ? ErrorCode::None : written.Error();
    }

    template <class U, class V>
    ScopedOverride(const BootCVar<U>&, V&&) noexcept {
        static_assert(
            detail::DependentFalse<U>,
            "BootCVar is fixed at boot; set it in a config file or with --set and restart");
    }

    ~ScopedOverride() {
        detail::EndScopedOverride(removal_);
    }

    ScopedOverride(const ScopedOverride&) = delete;
    ScopedOverride& operator=(const ScopedOverride&) = delete;
    ScopedOverride(ScopedOverride&&) = delete;
    ScopedOverride& operator=(ScopedOverride&&) = delete;

    // 쓰기 결과다(CVar::Set과 같은 오류).
    Result<void> Status() const noexcept {
        return error_ == ErrorCode::None ? Result<void>::Success() : Result<void>::Failure(error_);
    }

private:
    detail::ScopedRemoval removal_;
    char tag_[detail::ScopedTagBytes] = {};
    ErrorCode error_ = ErrorCode::InvalidArgument;
};

template <class T, class V>
ScopedOverride(const CVar<T>&, V) -> ScopedOverride<T>;

template <class T, class V>
ScopedOverride(const BootCVar<T>&, V) -> ScopedOverride<T>;

template <class V>
ScopedOverride(const CVarGroup&, V) -> ScopedOverride<int32>;

} // namespace rocks::cvar
