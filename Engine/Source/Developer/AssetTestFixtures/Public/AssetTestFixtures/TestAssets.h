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
    ROCKS_PROPERTY() int32 index = 0;
    ROCKS_PROPERTY() ObjectRef<TestPart> sibling;
    ROCKS_PROPERTY() BulkDataRef data;
};

// 저장 불가 클래스다(Transient — export에서 그 부분 트리째 빠진다).
ROCKS_CLASS(Transient)
struct TestScratch final : Object {
    ROCKS_PROPERTY() int32 cache = 0;
};

// Required Hard 참조를 가진 에셋이다(쿠킹 저장의 Required 거부·로드 누락 시험).
ROCKS_CLASS()
struct TestRequiredAsset final : Asset {
    ROCKS_PROPERTY(Required) ObjectRef<Asset> target;
};

} // namespace rocks::asset_fixture
