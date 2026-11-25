#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Optional.h>
#include <Core/Result.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>
#include <Collision/BodySettings.h>
#include <Collision/CollisionResponses.h>
#include <Collision/CollisionSettings.h>
#include <Collision/CollisionTypes.h>

// 충돌 profile 표와 실효 충돌 값 해석이다(Docs/Design/CollisionData.ko.md 4·5절). 표는 설정
// (CollisionSettings)에서 BuildCollisionProfileTable로 만드는 불변 값이다. 이름 조회는 모두 ASCII
// 대소문자를 무시한다(UE FName 비교와 같다).
//
// 사용 예:
//   auto table = BuildCollisionProfileTable(settings, allocator);
//   if (!table) { ... }                                  // 할당 실패만 오류
//   for (const auto& diagnostic : table->Diagnostics()) { ... }  // 빠진 항목(로그로 남긴다)
//   auto resolved = ResolveCollision(*table, component.CollisionProfileName(), body);
//   if (CollisionEnabledHasQuery(resolved.collisionEnabled)) { ... }
namespace rocks {

// 예약 profile 이름이다(UE UCollisionProfile::CustomCollisionProfileName —
// CollisionProfile.cpp:17). 이 이름이면 컴포넌트의 BodySettings 값을 직접 쓴다.
inline constexpr StringView CustomCollisionProfileName = "Custom";

// 채널의 쓰임이다.
enum class CollisionChannelKind : uint8 {
    // 이름이 없는 사용자 채널(8–63)이다.
    Unused,
    // body의 object type이 될 수 있다.
    Object,
    // 질의(trace) 채널이다.
    Trace
};

// 표를 만들 때 빠진 항목의 종류다. UE도 같은 경우에 로그를 남기고 항목을 지운다.
enum class CollisionProfileDiagnosticKind : uint8 {
    // channels 항목의 채널이 8–63 밖이다(무시).
    ChannelNotConfigurable,
    // 이미 이름이 주어진 채널에 또 이름을 줬다(뒤 항목 무시).
    ChannelAlreadyNamed,
    // 다른 채널의 표시 이름·enum 이름과 같은 이름이다(이름만 무시, 기본 응답은 적용 — Rocks 추가
    // 검사: 이름 조회가 모호해진다).
    ChannelNameInUse,
    // 채널 항목에 이름이 없다(기본 응답만 적용 — UE도 경고를 남긴다).
    UnnamedChannel,
    // profile 이름이 비었거나 예약 이름 "Custom"이다(profile 제외).
    InvalidProfileName,
    // objectTypeName이 없는 채널 이름이다(profile 제외).
    UnknownObjectType,
    // objectTypeName이 trace·미사용 채널이다(profile 제외).
    ObjectTypeNotObjectChannel,
    // 응답 칸의 채널 이름이 없다(그 칸만 건너뜀).
    UnknownResponseChannel,
    // redirect 이름이 비었거나 새 이름의 대상이 없다(redirect 무시).
    InvalidRedirect
};

// 빠진 항목 하나다. subject는 관련 profile·채널·redirect의 이름, detail은 문제가 된 값이다.
struct CollisionProfileDiagnostic {
    explicit CollisionProfileDiagnostic(Allocator& allocator) noexcept
        : subject(allocator), detail(allocator) {
    }

    CollisionProfileDiagnosticKind kind = CollisionProfileDiagnosticKind::InvalidProfileName;
    String subject;
    String detail;
};

// 표의 profile 하나다(UE FCollisionResponseTemplate의 계산된 값).
struct CollisionProfile {
    explicit CollisionProfile(Allocator& allocator) noexcept
        : name(allocator), helpMessage(allocator) {
    }

    String name;
    CollisionEnabled collisionEnabled = CollisionEnabled::NoCollision;
    CollisionChannel objectType = CollisionChannel::WorldStatic;
    CollisionResponses responses;
    String helpMessage;
    bool canModify = true;
};

// 충돌 profile 표다. 이동만 된다. 기본 생성(빈 표)은 엔진 채널 이름·종류와 기본 응답(전부 Block,
// gizmo만 Ignore)만 갖고 profile이 없다 — 모든 이름이 profileMissing으로 해석된다.
class ROCKS_COLLISION_API CollisionProfileTable {
public:
    explicit CollisionProfileTable(Allocator& allocator) noexcept;
    CollisionProfileTable(CollisionProfileTable&&) noexcept = default;
    CollisionProfileTable& operator=(CollisionProfileTable&&) noexcept = default;
    CollisionProfileTable(const CollisionProfileTable&) = delete;
    CollisionProfileTable& operator=(const CollisionProfileTable&) = delete;

    // 이름의 profile이다. redirect(같은 옛 이름이 여럿이면 마지막 것)를 먼저 보고, 없으면 이름
    // 그대로 찾는다(UE ReadConfig — CollisionProfile.cpp:190-191). 없으면 null이다. 포인터는 표가
    // 살아 있는 동안 유효하다.
    const CollisionProfile* FindProfile(StringView name) const noexcept;

    // 설정 순서(중복 이름은 마지막 것만)의 profile이다.
    Span<const CollisionProfile> Profiles() const noexcept {
        return profiles_.View();
    }

    // 표시 이름의 채널이다. 없으면 redirect(같은 옛 이름이 여럿이면 마지막 것)를 본다(UE
    // ReturnContainerIndexFromChannelName). 이름 없는 사용자 채널은 enum 이름으로 찾는다.
    Optional<CollisionChannel> FindChannel(StringView name) const noexcept;

    // 채널의 표시 이름이다(사용자 이름이 없으면 enum 이름).
    StringView ChannelName(CollisionChannel channel) const noexcept;

    CollisionChannelKind ChannelKind(CollisionChannel channel) const noexcept {
        return channelInfo_[CollisionChannelIndex(channel)].kind;
    }

    // 정적 object 묶음(정적 object 질의)에 드는지다. WorldStatic과 staticObject 사용자 채널이다.
    bool IsStaticObjectChannel(CollisionChannel channel) const noexcept {
        return channelInfo_[CollisionChannelIndex(channel)].staticObject;
    }

    // 모든 profile·사용자 지정 응답이 시작하는 묶음이다.
    const CollisionResponses& DefaultResponses() const noexcept {
        return defaultResponses_;
    }

    // 만들 때 빠진 항목이다(호출자가 로그로 남긴다).
    Span<const CollisionProfileDiagnostic> Diagnostics() const noexcept {
        return diagnostics_.View();
    }

private:
    friend struct CollisionProfileTableBuilder;

    struct ChannelInfo {
        CollisionChannelKind kind = CollisionChannelKind::Unused;
        bool staticObject = false;
    };

    struct Redirect {
        explicit Redirect(Allocator& allocator) noexcept : oldName(allocator), newName(allocator) {
        }

        String oldName;
        String newName;
    };

    const CollisionProfile* FindProfileDirect(StringView name) const noexcept;
    Optional<CollisionChannel> FindChannelDirect(StringView name) const noexcept;

    ChannelInfo channelInfo_[CollisionChannelCount];
    // 사용자 채널 이름이다. 비었거나(빈 표) 칸이 빈 문자열이면 enum 이름을 쓴다.
    Array<String> channelNames_;
    CollisionResponses defaultResponses_;
    Array<CollisionProfile> profiles_;
    Array<Redirect> profileRedirects_;
    Array<Redirect> channelRedirects_;
    Array<CollisionProfileDiagnostic> diagnostics_;
};

// 설정에서 표를 만든다(4절 순서). 할당 실패만 오류이고, 의미 오류는 항목을 빼고 진단을 남긴다.
ROCKS_COLLISION_API Result<CollisionProfileTable>
BuildCollisionProfileTable(const CollisionSettings& settings, Allocator& allocator) noexcept;

// 실효 충돌 값이다(5절).
struct ResolvedCollision {
    CollisionEnabled collisionEnabled = CollisionEnabled::NoCollision;
    CollisionChannel objectType = CollisionChannel::WorldStatic;
    CollisionResponses responses;
    // 표의 profile 값이면 참이다(거짓 = BodySettings 사용자 지정 값).
    bool fromProfile = false;
    // 이름이 "Custom"이 아닌데 표에 없었으면 참이다(호출자가 경고를 남긴다).
    bool profileMissing = false;
    // fromProfile이면 실효 profile 이름(redirect 뒤)이다. 표가 살아 있는 동안 유효하다.
    StringView profileName;
};

// profile 이름과 body 값에서 실효 충돌 값을 계산한다. 이름이 "Custom"·빈 값이거나 표에 없으면
// body의 사용자 지정 값을 쓴다(응답 = 기본 묶음 + customResponses, 없는 채널 이름은 건너뜀).
ROCKS_COLLISION_API ResolvedCollision ResolveCollision(const CollisionProfileTable& table,
                                                       StringView profileName,
                                                       const BodySettings& body) noexcept;

// 실효 값을 body의 사용자 지정 필드에 옮긴다(profile에서 "Custom"으로 바꾸는 setter가 쓴다).
// customResponses는 기본 묶음과 다른 칸만, 채널 번호 순으로 채운다. 실패(할당)하면 body는 그대로다.
ROCKS_COLLISION_API Result<void> MakeCustomCollision(const CollisionProfileTable& table,
                                                     const ResolvedCollision& resolved,
                                                     BodySettings& body) noexcept;

} // namespace rocks
