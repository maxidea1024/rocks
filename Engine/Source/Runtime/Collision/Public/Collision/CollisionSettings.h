#pragma once

#include <Core/Containers/Array.h>
#include <Core/Text/String.h>
#include <Collision/CollisionTypes.h>
#include <Meta/Annotations.h>
#include <Object/ModuleRegistration.h>

// 충돌 profile 설정이다(Docs/Design/CollisionData.ko.md 3절, UE UCollisionProfile의 globalconfig
// 필드 — E/Classes/Engine/CollisionProfile.h:167-184). 엔진 기본 데이터는 Engine/Config/Engine.json
// 섹션 "Collision"이고(UE BaseEngine.ini [/Script/Engine.CollisionProfile]와 같은 profile), 코드
// 기본값은 빈 배열이다. 프로젝트는 자기 Config/Engine.json에서 배열 연산으로 더한다:
//   "Collision": {
//     "channels": { "$add": [ { "channel": "GameTraceChannel1", "name": "Weapon",
//                               "defaultResponse": "Overlap", "traceType": true } ] },
//     "profiles": { "$add": [ { "name": "Projectile", "collisionEnabled": "QueryOnly",
//                               "objectTypeName": "WorldDynamic" } ] }
//   }
// 이름은 모두 ASCII 대소문자를 무시하고 비교한다. 설정에서 표를 만드는 것은
// BuildCollisionProfileTable(CollisionProfileTable.h)이다.
namespace rocks {

// 채널 이름과 응답 한 칸이다(UE FResponseChannel — EngineTypes.h:1415). 채널은 번호가 아니라 표시
// 이름으로 적는다(사용자 채널 번호가 바뀌어도 이름으로 따라간다).
ROCKS_STRUCT()
struct ChannelResponse {
    ROCKS_GENERATED_BODY()

    explicit ChannelResponse(Allocator& allocator) noexcept : channel(allocator) {
    }

    ROCKS_PROPERTY() String channel;
    ROCKS_PROPERTY() CollisionResponse response = CollisionResponse::Block;
};

// profile 하나의 정의다(UE FCollisionResponseTemplate — CollisionProfile.h:44, 기본값
// CollisionProfile.cpp:20-27).
ROCKS_STRUCT()
struct CollisionProfileSetup {
    ROCKS_GENERATED_BODY()

    explicit CollisionProfileSetup(Allocator& allocator) noexcept
        : name(allocator), objectTypeName(allocator), customResponses(allocator),
          helpMessage(allocator) {
    }

    // profile 이름이다. "Custom"은 예약이다(컴포넌트 값 직접 지정).
    ROCKS_PROPERTY() String name;
    ROCKS_PROPERTY() CollisionEnabled collisionEnabled = CollisionEnabled::NoCollision;
    // object type 채널의 표시 이름이다. 비면 WorldStatic이다. object 채널이어야 한다.
    ROCKS_PROPERTY() String objectTypeName;
    // 기본 응답 묶음에서 바꿀 칸이다(차례로 적용).
    ROCKS_PROPERTY() Array<ChannelResponse> customResponses;
    ROCKS_PROPERTY() String helpMessage;
    // 편집기에서 바꿀 수 있는지다(엔진 profile은 거짓).
    ROCKS_PROPERTY() bool canModify = true;
};

// 사용자 채널(8–63) 하나의 정의다(UE FCustomChannelSetup — CollisionProfile.h:96).
ROCKS_STRUCT()
struct CollisionChannelSetup {
    ROCKS_GENERATED_BODY()

    explicit CollisionChannelSetup(Allocator& allocator) noexcept : name(allocator) {
    }

    // 기본값 WorldStatic은 설정할 수 없는 채널이라, channel을 빠뜨린 항목은 진단 후 무시된다(UE
    // FCustomChannelSetup 기본값 ECC_WorldStatic과 같다).
    ROCKS_PROPERTY() CollisionChannel channel = CollisionChannel::WorldStatic;
    // 표시 이름이다. 비면 이름·종류는 그대로 두고 기본 응답만 정한다(진단 UnnamedChannel).
    ROCKS_PROPERTY() String name;
    // 모든 profile·body 응답 묶음에서 이 채널의 기본 응답이다.
    ROCKS_PROPERTY() CollisionResponse defaultResponse = CollisionResponse::Block;
    // 참이면 trace 채널(질의 채널), 거짓이면 object 채널(body의 object type이 될 수 있다).
    ROCKS_PROPERTY() bool traceType = false;
    // object 채널일 때 정적 object 묶음에 드는지다(정적/동적 object 질의).
    ROCKS_PROPERTY() bool staticObject = false;
};

// 이미 있는 profile의 응답을 덧붙여 고친다(UE FCustomProfile — CollisionProfile.h:142).
ROCKS_STRUCT()
struct CollisionProfileEdit {
    ROCKS_GENERATED_BODY()

    explicit CollisionProfileEdit(Allocator& allocator) noexcept
        : name(allocator), customResponses(allocator) {
    }

    ROCKS_PROPERTY() String name;
    ROCKS_PROPERTY() Array<ChannelResponse> customResponses;
};

// 이름 바꾸기다(UE FRedirector). 옛 이름으로 찾으면 새 이름을 쓴다.
ROCKS_STRUCT()
struct CollisionNameRedirect {
    ROCKS_GENERATED_BODY()

    explicit CollisionNameRedirect(Allocator& allocator) noexcept
        : oldName(allocator), newName(allocator) {
    }

    ROCKS_PROPERTY() String oldName;
    ROCKS_PROPERTY() String newName;
};

// 충돌 profile 설정 섹션이다(branch Engine, 섹션 Collision). 등록은 Collision 모듈 진입점의
// RegisterSettings가 한다.
ROCKS_STRUCT(Settings(Branch = "Engine", Section = "Collision"))
struct CollisionSettings {
    ROCKS_GENERATED_BODY()

    explicit CollisionSettings(Allocator& allocator) noexcept
        : profiles(allocator), channels(allocator), profileEdits(allocator),
          profileRedirects(allocator), channelRedirects(allocator) {
    }

    ROCKS_PROPERTY() Array<CollisionProfileSetup> profiles;
    ROCKS_PROPERTY() Array<CollisionChannelSetup> channels;
    ROCKS_PROPERTY() Array<CollisionProfileEdit> profileEdits;
    ROCKS_PROPERTY() Array<CollisionNameRedirect> profileRedirects;
    ROCKS_PROPERTY() Array<CollisionNameRedirect> channelRedirects;
};

// Collision 모듈의 리플렉션 타입(채널·응답·충돌 켬 enum, 설정 struct, BodySettings)을 등록한다(생성
// 집계 함수 generated::Collision::RegisterModuleTypes — 의존 모듈 타입도 함께). 이 모듈에 의존하는
// 모듈(World·Mesh)의 등록 함수가 자기 타입 전에 함께 등록하므로, 도구·시험이 이 모듈만 쓸 때 직접
// 부른다.
ROCKS_COLLISION_API Result<void> RegisterCollisionTypes(ModuleRegistration& registration) noexcept;

} // namespace rocks
