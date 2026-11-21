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

} // namespace rocks::asset_fixture
