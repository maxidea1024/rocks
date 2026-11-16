#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Math/Transform.h>
#include <Core/Optional.h>
#include <Meta/Annotations.h>
#include <Object/Object.h>
#include <Object/ObjectRefs.h>
#include <Object/SoftObjectRef.h>

namespace rocks::reflection_fixture {

// 리플렉션 B9h1 필드 플래그 fixture다(Docs/Design/ReflectionExtensions.ko.md 9.1절). FlagSlot은
// 멤버마다 SaveGame을 다르게 단 값 struct(포함 필터가 멤버마다 자기 플래그를 본다), FlagAtom은
// 멤버에 플래그가 없는 Atomic struct(통째 — 포함 필터를 멤버에 적용하지 않는다)다. FlagHolder는
// 복제 제외 플래그(DuplicateTransient·NonPIEDuplicateTransient)·텍스트
// 제외(TextExportTransient)·SaveGame 필드, SaveGame struct·struct 원소 배열·Atomic
// struct·Transform(미러 멤버 SaveGame) 필드, 포함 필터 밖의 Required 필드를 담고 복제 정책별 저장
// 집합·includeFlags 저장·로드 대칭·Required 실효 값 검사로 시험한다(FieldFlagGraphTests).
ROCKS_STRUCT()
struct FlagSlot {
    ROCKS_PROPERTY(SaveGame) int32 kept = 0;
    ROCKS_PROPERTY() int32 dropped = 0;
};

ROCKS_STRUCT(Atomic)
struct FlagAtom {
    ROCKS_PROPERTY() int32 a = 0;
    ROCKS_PROPERTY() int32 b = 0;
};

ROCKS_CLASS()
struct FlagHolder final : Object {
    explicit FlagHolder(Allocator& allocator) noexcept : slots(allocator), source(allocator) {
    }

    ROCKS_PROPERTY(DuplicateTransient) int32 duplicateOnly = 0;
    ROCKS_PROPERTY(NonPIEDuplicateTransient) int32 instance = 0;
    ROCKS_PROPERTY(TextExportTransient) int32 textOnly = 0;
    ROCKS_PROPERTY(SaveGame) int32 score = 0;
    ROCKS_PROPERTY() int32 plain = 0;
    ROCKS_PROPERTY(SaveGame) FlagSlot slot;
    ROCKS_PROPERTY(SaveGame) Array<FlagSlot> slots;
    ROCKS_PROPERTY(SaveGame) FlagAtom atom;
    ROCKS_PROPERTY(SaveGame) Transform pose;
    // 포함 필터(SaveGame) 밖의 Required 필드다(세이브게임 로드의 Required 검사에서 빠진다).
    ROCKS_PROPERTY(Required) SoftObjectRef<> source;
};

// 최상위 필드가 모두 SaveGame인 클래스들이다(포함 필터의 멤버 단위 판정 — 전부 저장한 파일을
// SaveGame 포함 필터로 읽으면 SaveGame이 아닌 struct 멤버·원소 멤버 때문에 InvalidArgument, Atomic
// struct는 멤버를 보지 않아 성공).
ROCKS_CLASS()
struct FlagSaveSlot final : Object {
    ROCKS_PROPERTY(SaveGame) FlagSlot slot;
};

ROCKS_CLASS()
struct FlagSaveList final : Object {
    explicit FlagSaveList(Allocator& allocator) noexcept : slots(allocator) {
    }

    ROCKS_PROPERTY(SaveGame) Array<FlagSlot> slots;
};

ROCKS_CLASS()
struct FlagSaveAtom final : Object {
    ROCKS_PROPERTY(SaveGame) FlagAtom atom;
};

// 컨테이너 안 struct 멤버의 포함 필터 경로다(Map 값·Optional 내부·정적 배열 원소 — 멤버마다 자기
// 플래그).
ROCKS_CLASS()
struct FlagContainers final : Object {
    explicit FlagContainers(Allocator& allocator) noexcept : byId(allocator) {
    }

    ROCKS_PROPERTY(SaveGame) OrderedMap<int32, FlagSlot> byId;
    ROCKS_PROPERTY(SaveGame) Optional<FlagSlot> maybe;
    ROCKS_PROPERTY(SaveGame) FlagSlot fixed[2];
};

} // namespace rocks::reflection_fixture
