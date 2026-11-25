#include <Base/InitializerList.h>
#include <Core/Move.h>
#include <Collision/CollisionProfileTable.h>

// 충돌 profile 표 생성·조회·해석이다(Docs/Design/CollisionData.ko.md 4·5절). 생성 순서는 UE
// UCollisionProfile::LoadProfileConfig·FillProfileData·LoadCustomResponses
// (E/Private/Collision/CollisionProfile.cpp)와 같다.
namespace rocks {

namespace {

// enum 이름이다(UE ECollisionChannel의 ECC_ 접두를 뗀 표시 이름).
constexpr StringView ChannelEnumNames[CollisionChannelCount] = {
    "WorldStatic",
    "WorldDynamic",
    "Pawn",
    "Visibility",
    "Camera",
    "PhysicsBody",
    "Vehicle",
    "Destructible",
    "EngineTraceChannel1",
    "EngineTraceChannel2",
    "EngineTraceChannel3",
    "EngineTraceChannel4",
    "EngineTraceChannel5",
    "EngineTraceChannel6",
    "GameTraceChannel1",
    "GameTraceChannel2",
    "GameTraceChannel3",
    "GameTraceChannel4",
    "GameTraceChannel5",
    "GameTraceChannel6",
    "GameTraceChannel7",
    "GameTraceChannel8",
    "GameTraceChannel9",
    "GameTraceChannel10",
    "GameTraceChannel11",
    "GameTraceChannel12",
    "GameTraceChannel13",
    "GameTraceChannel14",
    "GameTraceChannel15",
    "GameTraceChannel16",
    "GameTraceChannel17",
    "GameTraceChannel18",
    "GameTraceChannel19",
    "GameTraceChannel20",
    "GameTraceChannel21",
    "GameTraceChannel22",
    "GameTraceChannel23",
    "GameTraceChannel24",
    "GameTraceChannel25",
    "GameTraceChannel26",
    "GameTraceChannel27",
    "GameTraceChannel28",
    "GameTraceChannel29",
    "GameTraceChannel30",
    "GameTraceChannel31",
    "GameTraceChannel32",
    "GameTraceChannel33",
    "GameTraceChannel34",
    "GameTraceChannel35",
    "GameTraceChannel36",
    "GameTraceChannel37",
    "GameTraceChannel38",
    "GameTraceChannel39",
    "GameTraceChannel40",
    "GameTraceChannel41",
    "GameTraceChannel42",
    "GameTraceChannel43",
    "GameTraceChannel44",
    "GameTraceChannel45",
    "GameTraceChannel46",
    "GameTraceChannel47",
    "GameTraceChannel48",
    "GameTraceChannel49",
    "GameTraceChannel50",
};

bool SameName(StringView a, StringView b) noexcept {
    return EqualsIgnoringAsciiCase(a, b);
}

} // namespace

// 표 생성의 private 접근 창이다.
struct CollisionProfileTableBuilder {
    CollisionProfileTable& table;
    const CollisionSettings& settings;
    Allocator& allocator;

    static Result<CollisionProfileTable> Build(const CollisionSettings& settings,
                                               Allocator& allocator) noexcept;

    Result<void> Diagnose(CollisionProfileDiagnosticKind kind, StringView subject,
                          StringView detail) noexcept {
        CollisionProfileDiagnostic diagnostic(allocator);
        diagnostic.kind = kind;
        if (auto assigned = diagnostic.subject.TryAssign(subject); !assigned) {
            return assigned;
        }

        if (auto assigned = diagnostic.detail.TryAssign(detail); !assigned) {
            return assigned;
        }

        return table.diagnostics_.TryPushBack(Move(diagnostic));
    }

    // 1단계 앞부분: 엔진 채널 종류와 빈 사용자 이름 칸 64개.
    Result<void> InitializeChannels() noexcept {
        if (auto reserved = table.channelNames_.TryReserve(CollisionChannelCount); !reserved) {
            return reserved;
        }

        for (uint32 index = 0; index < CollisionChannelCount; ++index) {
            if (auto added = table.channelNames_.TryEmplaceBack(allocator); !added) {
                return Result<void>::Failure(added.Error());
            }
        }

        return Result<void>::Success();
    }

    // 1·2단계: 사용자 채널 이름·종류·기본 응답. channel 순으로(같은 channel은 설정 순서) 처리한다.
    Result<void> ApplyChannels() noexcept {
        const Span<const CollisionChannelSetup> setups = settings.channels.View();
        Array<usize> order(allocator);
        if (auto reserved = order.TryReserve(setups.Size()); !reserved) {
            return reserved;
        }

        for (usize index = 0; index < setups.Size(); ++index) {
            (void)order.TryPushBack(index);
        }

        // 안정 삽입 정렬(항목 수가 적다).
        for (usize index = 1; index < order.Size(); ++index) {
            const usize value = order[index];
            usize position = index;
            while (position > 0 && setups[order[position - 1]].channel > setups[value].channel) {
                order[position] = order[position - 1];
                --position;
            }

            order[position] = value;
        }

        for (const usize index : order.View()) {
            const CollisionChannelSetup& setup = setups[index];
            const StringView channelName = ChannelEnumNames[CollisionChannelIndex(setup.channel)];
            if (setup.channel < FirstConfigurableCollisionChannel) {
                if (auto noted = Diagnose(CollisionProfileDiagnosticKind::ChannelNotConfigurable,
                                          setup.name.View(), channelName);
                    !noted) {
                    return noted;
                }

                continue;
            }

            const uint32 slot = CollisionChannelIndex(setup.channel);
            if (setup.name.IsEmpty()) {
                if (auto noted = Diagnose(CollisionProfileDiagnosticKind::UnnamedChannel,
                                          StringView(), channelName);
                    !noted) {
                    return noted;
                }
            } else {
                if (table.channelInfo_[slot].kind != CollisionChannelKind::Unused) {
                    if (auto noted = Diagnose(CollisionProfileDiagnosticKind::ChannelAlreadyNamed,
                                              setup.name.View(), channelName);
                        !noted) {
                        return noted;
                    }

                    continue;
                }

                // 다른 채널의 표시 이름·enum 이름과 겹치면 이름만 무시하고 기본 응답은
                // 적용한다(UE에는 이 검사가 없어 이름과 기본 응답을 둘 다 받는다 — 이름 조회가
                // 모호해지는 것만 막는다).
                bool nameInUse = false;
                if (const Optional<CollisionChannel> existing =
                        table.FindChannelDirect(setup.name.View());
                    existing.HasValue() && *existing != setup.channel) {
                    if (auto noted = Diagnose(CollisionProfileDiagnosticKind::ChannelNameInUse,
                                              setup.name.View(), channelName);
                        !noted) {
                        return noted;
                    }

                    nameInUse = true;
                }

                if (!nameInUse) {
                    if (auto assigned = table.channelNames_[slot].TryAssign(setup.name.View());
                        !assigned) {
                        return assigned;
                    }

                    table.channelInfo_[slot].kind = setup.traceType ? CollisionChannelKind::Trace
                                                                    : CollisionChannelKind::Object;
                    table.channelInfo_[slot].staticObject = !setup.traceType && setup.staticObject;
                }
            }

            (void)table.defaultResponses_.Set(setup.channel, setup.defaultResponse);
        }

        return Result<void>::Success();
    }

    // 3·5단계 공통: redirect 목록을 받는다. 두 이름이 있고 새 이름의 대상이 있어야 한다.
    template <class TargetExists>
    Result<void> ApplyRedirects(Span<const CollisionNameRedirect> redirects,
                                Array<CollisionProfileTable::Redirect>& out,
                                TargetExists targetExists) noexcept {
        for (const CollisionNameRedirect& redirect : redirects) {
            if (redirect.oldName.IsEmpty() || redirect.newName.IsEmpty() ||
                !targetExists(redirect.newName.View())) {
                if (auto noted = Diagnose(CollisionProfileDiagnosticKind::InvalidRedirect,
                                          redirect.oldName.View(), redirect.newName.View());
                    !noted) {
                    return noted;
                }

                continue;
            }

            CollisionProfileTable::Redirect entry(allocator);
            if (auto assigned = entry.oldName.TryAssign(redirect.oldName.View()); !assigned) {
                return assigned;
            }

            if (auto assigned = entry.newName.TryAssign(redirect.newName.View()); !assigned) {
                return assigned;
            }

            if (auto added = out.TryPushBack(Move(entry)); !added) {
                return added;
            }
        }

        return Result<void>::Success();
    }

    // 응답 칸 목록을 차례로 적용한다. 없는 채널 이름은 그 칸만 건너뛴다.
    Result<void> ApplyResponses(StringView profileName, Span<const ChannelResponse> responses,
                                CollisionResponses& target) noexcept {
        for (const ChannelResponse& response : responses) {
            const Optional<CollisionChannel> channel = table.FindChannel(response.channel.View());
            if (!channel.HasValue()) {
                if (auto noted = Diagnose(CollisionProfileDiagnosticKind::UnknownResponseChannel,
                                          profileName, response.channel.View());
                    !noted) {
                    return noted;
                }

                continue;
            }

            (void)target.Set(*channel, response.response);
        }

        return Result<void>::Success();
    }

    // 4단계: profile.
    Result<void> ApplyProfiles() noexcept {
        const Span<const CollisionProfileSetup> setups = settings.profiles.View();
        for (usize index = 0; index < setups.Size(); ++index) {
            const CollisionProfileSetup& setup = setups[index];
            if (setup.name.IsEmpty() || SameName(setup.name.View(), CustomCollisionProfileName)) {
                if (auto noted = Diagnose(CollisionProfileDiagnosticKind::InvalidProfileName,
                                          setup.name.View(), StringView());
                    !noted) {
                    return noted;
                }

                continue;
            }

            // 같은 이름이 뒤에 있으면 뒤 것만 남긴다(진단 없음 — 층 병합의 정상 결과일 수 있다).
            bool superseded = false;
            for (usize later = index + 1; later < setups.Size() && !superseded; ++later) {
                superseded = SameName(setups[later].name.View(), setup.name.View());
            }

            if (superseded) {
                continue;
            }

            CollisionChannel objectType = CollisionChannel::WorldStatic;
            if (!setup.objectTypeName.IsEmpty()) {
                const Optional<CollisionChannel> channel =
                    table.FindChannel(setup.objectTypeName.View());
                if (!channel.HasValue()) {
                    if (auto noted = Diagnose(CollisionProfileDiagnosticKind::UnknownObjectType,
                                              setup.name.View(), setup.objectTypeName.View());
                        !noted) {
                        return noted;
                    }

                    continue;
                }

                if (table.ChannelKind(*channel) != CollisionChannelKind::Object) {
                    if (auto noted =
                            Diagnose(CollisionProfileDiagnosticKind::ObjectTypeNotObjectChannel,
                                     setup.name.View(), setup.objectTypeName.View());
                        !noted) {
                        return noted;
                    }

                    continue;
                }

                objectType = *channel;
            }

            CollisionProfile profile(allocator);
            profile.collisionEnabled = setup.collisionEnabled;
            profile.objectType = objectType;
            profile.canModify = setup.canModify;
            profile.responses = table.defaultResponses_;
            if (auto assigned = profile.name.TryAssign(setup.name.View()); !assigned) {
                return assigned;
            }

            if (auto assigned = profile.helpMessage.TryAssign(setup.helpMessage.View());
                !assigned) {
                return assigned;
            }

            if (auto applied = ApplyResponses(setup.name.View(), setup.customResponses.View(),
                                              profile.responses);
                !applied) {
                return applied;
            }

            for (const CollisionProfileEdit& edit : settings.profileEdits.View()) {
                if (SameName(edit.name.View(), setup.name.View())) {
                    if (auto applied = ApplyResponses(
                            setup.name.View(), edit.customResponses.View(), profile.responses);
                        !applied) {
                        return applied;
                    }

                    break;
                }
            }

            if (auto added = table.profiles_.TryPushBack(Move(profile)); !added) {
                return added;
            }
        }

        return Result<void>::Success();
    }
};

CollisionProfileTable::CollisionProfileTable(Allocator& allocator) noexcept
    : channelNames_(allocator), profiles_(allocator), profileRedirects_(allocator),
      channelRedirects_(allocator), diagnostics_(allocator) {
    for (const CollisionChannel channel :
         {CollisionChannel::WorldStatic, CollisionChannel::WorldDynamic, CollisionChannel::Pawn,
          CollisionChannel::PhysicsBody, CollisionChannel::Vehicle,
          CollisionChannel::Destructible}) {
        channelInfo_[CollisionChannelIndex(channel)].kind = CollisionChannelKind::Object;
    }

    channelInfo_[CollisionChannelIndex(CollisionChannel::Visibility)].kind =
        CollisionChannelKind::Trace;
    channelInfo_[CollisionChannelIndex(CollisionChannel::Camera)].kind =
        CollisionChannelKind::Trace;
    channelInfo_[CollisionChannelIndex(CollisionChannel::WorldStatic)].staticObject = true;
    (void)defaultResponses_.Set(GizmoCollisionChannel, CollisionResponse::Ignore);
}

StringView CollisionProfileTable::ChannelName(CollisionChannel channel) const noexcept {
    const uint32 index = CollisionChannelIndex(channel);
    if (index < channelNames_.Size() && !channelNames_[index].IsEmpty()) {
        return channelNames_[index].View();
    }

    return ChannelEnumNames[index];
}

const CollisionProfile* CollisionProfileTable::FindProfileDirect(StringView name) const noexcept {
    for (const CollisionProfile& profile : profiles_.View()) {
        if (SameName(profile.name.View(), name)) {
            return &profile;
        }
    }

    return nullptr;
}

const CollisionProfile* CollisionProfileTable::FindProfile(StringView name) const noexcept {
    // redirect가 먼저다(UE ReadConfig — CollisionProfile.cpp:190-191). 같은 옛 이름이 여럿이면
    // 마지막 것이 이긴다(UE ProfileRedirectsMap은 TMap이라 뒤 항목이 덮어쓴다).
    for (usize index = profileRedirects_.Size(); index > 0; --index) {
        const Redirect& redirect = profileRedirects_[index - 1];
        if (SameName(redirect.oldName.View(), name)) {
            return FindProfileDirect(redirect.newName.View());
        }
    }

    return FindProfileDirect(name);
}

Optional<CollisionChannel>
CollisionProfileTable::FindChannelDirect(StringView name) const noexcept {
    for (uint32 index = 0; index < CollisionChannelCount; ++index) {
        const CollisionChannel channel = static_cast<CollisionChannel>(index);
        if (SameName(ChannelName(channel), name)) {
            return channel;
        }
    }

    return NullOpt;
}

Optional<CollisionChannel> CollisionProfileTable::FindChannel(StringView name) const noexcept {
    if (const Optional<CollisionChannel> channel = FindChannelDirect(name); channel.HasValue()) {
        return channel;
    }

    // 같은 옛 이름이 여럿이면 마지막 것이 이긴다(UE CollisionChannelRedirectsMap — TMap).
    for (usize index = channelRedirects_.Size(); index > 0; --index) {
        const Redirect& redirect = channelRedirects_[index - 1];
        if (SameName(redirect.oldName.View(), name)) {
            return FindChannelDirect(redirect.newName.View());
        }
    }

    return NullOpt;
}

Result<CollisionProfileTable> CollisionProfileTableBuilder::Build(const CollisionSettings& settings,
                                                                  Allocator& allocator) noexcept {
    CollisionProfileTable table(allocator);
    CollisionProfileTableBuilder builder{table, settings, allocator};
    if (auto initialized = builder.InitializeChannels(); !initialized) {
        return Result<CollisionProfileTable>::Failure(initialized.Error());
    }

    if (auto applied = builder.ApplyChannels(); !applied) {
        return Result<CollisionProfileTable>::Failure(applied.Error());
    }

    if (auto applied =
            builder.ApplyRedirects(settings.channelRedirects.View(), table.channelRedirects_,
                                   [&table](StringView name) noexcept {
                                       return table.FindChannelDirect(name).HasValue();
                                   });
        !applied) {
        return Result<CollisionProfileTable>::Failure(applied.Error());
    }

    if (auto applied = builder.ApplyProfiles(); !applied) {
        return Result<CollisionProfileTable>::Failure(applied.Error());
    }

    if (auto applied =
            builder.ApplyRedirects(settings.profileRedirects.View(), table.profileRedirects_,
                                   [&table](StringView name) noexcept {
                                       return table.FindProfileDirect(name) != nullptr;
                                   });
        !applied) {
        return Result<CollisionProfileTable>::Failure(applied.Error());
    }

    return Result<CollisionProfileTable>::Success(Move(table));
}

Result<CollisionProfileTable> BuildCollisionProfileTable(const CollisionSettings& settings,
                                                         Allocator& allocator) noexcept {
    return CollisionProfileTableBuilder::Build(settings, allocator);
}

ResolvedCollision ResolveCollision(const CollisionProfileTable& table, StringView profileName,
                                   const BodySettings& body) noexcept {
    ResolvedCollision resolved;
    if (!profileName.IsEmpty() && !SameName(profileName, CustomCollisionProfileName)) {
        if (const CollisionProfile* profile = table.FindProfile(profileName)) {
            resolved.collisionEnabled = profile->collisionEnabled;
            resolved.objectType = profile->objectType;
            resolved.responses = profile->responses;
            resolved.fromProfile = true;
            resolved.profileName = profile->name.View();
            return resolved;
        }

        resolved.profileMissing = true;
    }

    resolved.collisionEnabled = body.collisionEnabled;
    resolved.objectType = body.objectType;
    resolved.responses = table.DefaultResponses();
    for (const ChannelResponse& response : body.customResponses.View()) {
        if (const Optional<CollisionChannel> channel = table.FindChannel(response.channel.View());
            channel.HasValue()) {
            (void)resolved.responses.Set(*channel, response.response);
        }
    }

    return resolved;
}

Result<void> MakeCustomCollision(const CollisionProfileTable& table,
                                 const ResolvedCollision& resolved, BodySettings& body) noexcept {
    Allocator& allocator = body.customResponses.GetAllocator();
    Array<ChannelResponse> responses(allocator);
    const CollisionResponses& defaults = table.DefaultResponses();
    for (uint32 index = 0; index < CollisionChannelCount; ++index) {
        const CollisionChannel channel = static_cast<CollisionChannel>(index);
        if (resolved.responses.Get(channel) == defaults.Get(channel)) {
            continue;
        }

        ChannelResponse response(allocator);
        response.response = resolved.responses.Get(channel);
        if (auto assigned = response.channel.TryAssign(table.ChannelName(channel)); !assigned) {
            return assigned;
        }

        if (auto added = responses.TryPushBack(Move(response)); !added) {
            return added;
        }
    }

    body.collisionEnabled = resolved.collisionEnabled;
    body.objectType = resolved.objectType;
    body.customResponses = Move(responses);
    return Result<void>::Success();
}

} // namespace rocks
