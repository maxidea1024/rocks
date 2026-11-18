#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Optional.h>
#include <Core/Text/String.h>
#include <Meta/Annotations.h>

// 기기 프로필 설정 struct다(Docs/Design/ConfigSystem.ko.md 6.2절 — 설정 CF4). 정의는 설정 계층의
// 값이다: branch `DeviceProfiles`(`Engine/Config/DeviceProfiles.json` 등)이고 층마다 병합된다(Map은
// 키별 병합 — 프로젝트가 프로필 하나의 값 하나만 고칠 수 있다). 값 묶음의 대상은 콘솔 설정 이름
// (`render.shadow.size`) 또는 `<Branch>.<Section>.<필드 경로>`이고 값은 리플렉션 명령행 표기다(설정
// 대입과 같다 — 3.2절). 등록: RegisterDeviceProfileSettings(Config/DeviceProfiles.h). 품질 묶음
// 정의는 여기에 두지 않는다(코드 `CVarGroup` — Docs/Design/CVarSystem.ko.md 11절).
//
// 예(DeviceProfiles.json):
//   { "DeviceProfiles": {
//       "memoryBuckets": [ { "name": "Large", "minimumMiB": 16384 }, { "name": "Small" } ],
//       "platformDefaults": { "Windows": "Windows" },
//       "profiles": {
//         "Windows": { "platforms": ["Windows"], "values": { "quality.shadow": "3" },
//                      "memoryValues": { "Small": { "values": { "quality.shadow": "1" } } } },
//         "Windows_Integrated": { "parent": "Windows", "matchPriority": 10,
//                                 "match": [ { "trait": "gpu.memory_mib", "below": 2048 } ],
//                                 "values": { "quality.shadow": "2" } } } } }
namespace rocks::config {

// 기기 특성(DeviceTraits — Config/DeviceTraits.h) 조건 하나다. 적힌 조건이 모두 참이어야 참이다
// (조건이 하나도 없으면 특성이 있기만 하면 참). 특성이 없으면 거짓이다.
ROCKS_STRUCT()
struct DeviceCondition {
    explicit DeviceCondition(Allocator& allocator) noexcept
        : trait(allocator), equals(allocator), contains(allocator) {
    }

    // 특성 이름(DeviceTraitNames — `gpu.memory_mib`·`platform` 등).
    ROCKS_PROPERTY() String trait;
    // 문자열 특성이 이것과 같다(ASCII 대소문자 무시). 비면 보지 않는다.
    ROCKS_PROPERTY() String equals;
    // 문자열 특성이 이것을 담는다(ASCII 대소문자 무시). 비면 보지 않는다.
    ROCKS_PROPERTY() String contains;
    // 수 특성 ≥ atLeast.
    ROCKS_PROPERTY() Optional<double> atLeast;
    // 수 특성 < below.
    ROCKS_PROPERTY() Optional<double> below;
};

// 대상 → 값 표기 묶음이다(기기 프로필의 메모리 등급별 값).
ROCKS_STRUCT()
struct ConfigValueSet {
    explicit ConfigValueSet(Allocator& allocator) noexcept : values(allocator) {
    }

    ROCKS_PROPERTY() OrderedMap<String, String> values;
};

// 기기 프로필 하나의 정의다(UE UDeviceProfile에 대응 — 콘솔 변수 대신 설정 값 묶음 전체, 결정 D4).
ROCKS_STRUCT()
struct DeviceProfileDefinition {
    explicit DeviceProfileDefinition(Allocator& allocator) noexcept
        : parent(allocator), platforms(allocator), match(allocator), values(allocator),
          memoryValues(allocator) {
    }

    // 부모 프로필 이름이다(비면 없음). 자식 값이 이긴다(부모는 아직 정해지지 않은 대상만 채운다).
    ROCKS_PROPERTY() String parent;
    // 적용 대상 플랫폼이다(비면 모든 플랫폼). 선택 규칙은 이 플랫폼에서만 본다.
    ROCKS_PROPERTY() Array<String> platforms;
    // 선택 규칙이다: 조건이 모두 참이면 후보다(비면 규칙으로 고르지 않는다).
    ROCKS_PROPERTY() Array<DeviceCondition> match;
    // 후보가 여럿이면 큰 것이 이긴다(같으면 이름 byte 순서로 앞선 것).
    ROCKS_PROPERTY() int32 matchPriority = 0;
    // 값(대상 → 값 표기).
    ROCKS_PROPERTY() OrderedMap<String, String> values;
    // 메모리 등급 이름 → 그 등급에서 더 적용할 값. 같은 프로필의 values보다 강하다(UE
    // `CVars_<Bucket>`).
    ROCKS_PROPERTY() OrderedMap<String, ConfigValueSet> memoryValues;
};

// 메모리 등급 하나다.
ROCKS_STRUCT()
struct MemoryBucketDefinition {
    explicit MemoryBucketDefinition(Allocator& allocator) noexcept : name(allocator) {
    }

    ROCKS_PROPERTY() String name;
    // 시스템 메모리(`memory.system_mib`)가 이 값 이상이면 이 등급이다.
    ROCKS_PROPERTY() int64 minimumMiB = 0;
};

// 기기 프로필 정의와 선택이다(섹션 `DeviceProfiles`).
ROCKS_STRUCT(Settings(Branch = "DeviceProfiles", Section = "DeviceProfiles"))
struct DeviceProfileSettings {
    explicit DeviceProfileSettings(Allocator& allocator) noexcept
        : profiles(allocator), profile(allocator), platformDefaults(allocator),
          memoryBuckets(allocator) {
    }

    // 프로필 이름 → 정의.
    ROCKS_PROPERTY() OrderedMap<String, DeviceProfileDefinition> profiles;
    // 프로젝트가 고른 프로필이다(선택 순서 2 — 비면 없음).
    ROCKS_PROPERTY() String profile;
    // 플랫폼 이름 → 기본 프로필(선택 순서 4). 없으면 플랫폼 이름과 같은 프로필이다.
    ROCKS_PROPERTY() OrderedMap<String, String> platformDefaults;
    // 메모리 등급(큰 것부터). 시스템 메모리가 minimumMiB 이상인 첫 등급이 기기의 등급이다.
    ROCKS_PROPERTY() Array<MemoryBucketDefinition> memoryBuckets;
};

} // namespace rocks::config
