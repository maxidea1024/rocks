#pragma once

#include <AssetRuntime/BulkData.h>
#include <AssetRuntime/Package.h>
#include <Core/Text/String.h>
#include <Meta/Annotations.h>
#include <Object/ObjectRefArray.h>
#include <Object/ObjectRefs.h>
#include <Object/SoftObjectRef.h>

namespace rocks::asset_fixture {

// 에셋 패키지 시험의 대표 에셋이다(Docs/Design/AssetSystem.ko.md 14절). 값·문자열·bulk·다른
// 패키지로의 Hard 참조(단일·배열·TestAsset 기대)·Soft 참조와 EditorOnly 필드(값·bulk·Hard 참조)를
// 담는다.
ROCKS_CLASS()
struct TestAsset final : Asset {
    ROCKS_GENERATED_BODY()

    explicit TestAsset(Allocator& allocator) noexcept
        : label(allocator), hardList(allocator), soft(allocator), note(allocator) {
    }

    ROCKS_PROPERTY() int32 value = 0;
    ROCKS_PROPERTY() String label;
    ROCKS_PROPERTY() BulkDataRef pixels;
    ROCKS_PROPERTY() ObjectRef<Asset> hard;
    ROCKS_PROPERTY() ObjectRefArray<Object> hardList;
    ROCKS_PROPERTY() SoftObjectRef<Asset> soft;
    ROCKS_PROPERTY(EditorOnly) String note;
    ROCKS_PROPERTY(EditorOnly) BulkDataRef source;
    ROCKS_PROPERTY(EditorOnly) ObjectRef<Asset> editorRef;
    // 기대 타입이 TestAsset인 Hard 참조다(import 선언 타입이 기대 타입보다 넓은 Asset일 때 연결
    // 단계의 타입 대조 시험).
    ROCKS_PROPERTY() ObjectRef<TestAsset> typed;
};

// 복제 제외 멤버를 가진 struct다(struct 필드·원소 배열 안의 제외 — DuplicableAsset).
ROCKS_STRUCT()
struct DuplicableSlot {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() int32 kept = 0;
    ROCKS_PROPERTY(DuplicateTransient) int32 cache = 0;
};

// 복제 제외 필드와 Soft 참조를 가진 에셋이다(writer 복제 옵션 시험 — AssetSystem.ko.md 7.1절 17
// A2e3b): cache·instance는 복제가 쓰지 않고(클래스 기본값), self는 자기 패키지를 가리켜 사본 id로
// 바뀌고, other는 그대로다.
ROCKS_CLASS()
struct DuplicableAsset final : Asset {
    ROCKS_GENERATED_BODY()

    explicit DuplicableAsset(Allocator& allocator) noexcept
        : self(allocator), other(allocator), slots(allocator) {
    }

    ROCKS_PROPERTY() int32 kept = 0;
    ROCKS_PROPERTY(DuplicateTransient) int32 cache = 0;
    ROCKS_PROPERTY(NonTryoutDuplicateTransient) int32 instance = 0;
    ROCKS_PROPERTY() SoftObjectRef<Asset> self;
    ROCKS_PROPERTY() SoftObjectRef<Asset> other;
    ROCKS_PROPERTY() DuplicableSlot slot;
    ROCKS_PROPERTY() Array<DuplicableSlot> slots;
};

// 하위 객체다(대표 에셋 아래 이름 있는 객체 — import 경로 "Body.Socket" 시험).
ROCKS_CLASS()
struct TestPart final : Object {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() int32 index = 0;
    ROCKS_PROPERTY() ObjectRef<TestPart> sibling;
    ROCKS_PROPERTY() BulkDataRef data;
};

// 저장 불가 클래스다(Transient — export에서 그 부분 트리째 빠진다).
ROCKS_CLASS(Transient)
struct TestScratch final : Object {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() int32 cache = 0;
};

// Required Hard 참조를 가진 에셋이다(쿠킹 저장의 Required 거부·로드 누락 시험).
ROCKS_CLASS()
struct TestRequiredAsset final : Asset {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY(Required) ObjectRef<Asset> target;
};

// TestHookAsset의 생성자·소멸자가 부르는 시험 hook이다(null이면 부르지 않는다). 시험이 걸고 끝나기
// 전에 푼다 — 모듈이 EngineTests 하나에 정적으로 링크되므로 ActiveTestHookAssetHooks()의 저장소는
// 하나다.
struct TestHookAssetHooks {
    void (*constructed)(void* context) noexcept = nullptr;
    void (*destroyed)(void* context) noexcept = nullptr;
    void* context = nullptr;
};

// 지금 hook이다. 저장소는 Private/FixtureState.cpp다(공개 헤더에 쓰기 가능한 정적 저장소를 두지
// 않는다 — BuildTargets BD29).
TestHookAssetHooks& ActiveTestHookAssetHooks() noexcept;

// 생성자·소멸자가 시험 hook을 부르는 에셋이다(로더 공개 트랜잭션 안의 사용자 코드가 로더에
// 재진입하는 시험 — Docs/Design/AssetSystem.ko.md 11.6절 18). typed는 기대 타입이 TestAsset인 Hard
// 참조다(오래된 의존 쿠킹 결과를 가리켜 공개를 실패시키면 롤백이 소멸자를 부른다).
ROCKS_CLASS()
struct TestHookAsset final : Asset {
    ROCKS_GENERATED_BODY()

    TestHookAsset() noexcept {
        if (ActiveTestHookAssetHooks().constructed != nullptr) {
            ActiveTestHookAssetHooks().constructed(ActiveTestHookAssetHooks().context);
        }
    }

    ~TestHookAsset() {
        if (ActiveTestHookAssetHooks().destroyed != nullptr) {
            ActiveTestHookAssetHooks().destroyed(ActiveTestHookAssetHooks().context);
        }
    }

    TestHookAsset(const TestHookAsset&) = delete;
    TestHookAsset& operator=(const TestHookAsset&) = delete;

    ROCKS_PROPERTY() ObjectRef<TestAsset> typed;
};

// 시험 임포터의 옵션이다(AssetSystem.ko.md 7.1절 18 — A2f): scale은 값 배율, failAt은 0이 아니면
// 시험 임포터가 그 단계에서 실패한다.
ROCKS_STRUCT()
struct TestImportOptions {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() int32 scale = 1;
    ROCKS_PROPERTY() int32 failAt = 0;
};

// 주 출력(TestAsset)의 속성 이름과 겹치는 옵션이다(도구 이름 Value — value와 대소문자만 다르다,
// 등록 거부 시험).
ROCKS_STRUCT()
struct CollidingImportOptions {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY(Name = "Value") int32 conflicting = 0;
};

// 명령 예약 이름과 겹치는 옵션이다(등록 거부 시험).
ROCKS_STRUCT()
struct ReservedImportOptions {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() int32 source = 0;
};

} // namespace rocks::asset_fixture
