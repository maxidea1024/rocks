#pragma once

#include <Base/Compiler.h>
#include <AssetRuntime/Package.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/OrderedMap.h>
#include <Meta/Annotations.h>
#include <Object/InstancedStruct.h>
#include <Object/SoftObjectRef.h>

// 에셋 bundle·EditorOnly 클래스 시험 에셋이다(Docs/Design/AssetSystem 10.2절 "A4e 구현
// 세부" — A4e2): 필드 지정자 AssetBundles(단일·배열·struct 사슬·Map 키·InstancedStruct 내용·지정
// 없음·EditorOnly 경로)와 클래스 표지 EditorOnly(에셋·하위 객체).
namespace rocks::asset_fixture {

// bundle 슬롯이다(사슬 제한 — 소프트 참조의 실효 bundle은 바깥 필드 목록과의 교집합이다). icon은
// 자기 목록 {UI, Game}과 바깥 목록의 교집합, any는 바깥 목록 그대로다.
ROCKS_STRUCT()
struct BundleSlot {
    ROCKS_GENERATED_BODY()

    explicit BundleSlot(Allocator& allocator) noexcept : icon(allocator), any(allocator) {
    }

    ROCKS_PROPERTY(AssetBundles = "UI,Game") SoftObjectRef<Asset> icon;
    ROCKS_PROPERTY() SoftObjectRef<Asset> any;
};

// AssetBundles 필드를 가진 에셋이다. portrait는 공백 있는 목록(정규 텍스트 "UI,Game"), levels는
// 배열, slot은 struct 사슬(UI), weights는 Map 키, extra는 InstancedStruct 내용(Game), plain은 지정
// 없음, editorExtra·editorSoft는 EditorOnly 경로(쿠킹 패키지에서 빠지므로 bundle에 들지 않는다).
ROCKS_CLASS()
struct BundleAsset final : Asset {
    ROCKS_GENERATED_BODY()

    explicit BundleAsset(Allocator& allocator) noexcept
        : portrait(allocator), levels(allocator), slot(allocator), plain(allocator),
          weights(allocator)
#if ROCKS_WITH_EDITOR_DATA
          ,
          editorSoft(allocator)
#endif
    {
    }

    ROCKS_PROPERTY(AssetBundles = " UI , Game ") SoftObjectRef<Asset> portrait;
    ROCKS_PROPERTY(AssetBundles = "Game") Array<SoftObjectRef<Asset>> levels;
    ROCKS_PROPERTY(AssetBundles = "UI") BundleSlot slot;
    ROCKS_PROPERTY() SoftObjectRef<Asset> plain;
    ROCKS_PROPERTY(AssetBundles = "Audio") OrderedMap<SoftObjectRef<Asset>, int32> weights;
    ROCKS_PROPERTY(AssetBundles = "Game") InstancedStruct<> extra;
#if ROCKS_WITH_EDITOR_DATA
    ROCKS_PROPERTY(EditorOnly) InstancedStruct<> editorExtra;
    ROCKS_PROPERTY(EditorOnly) SoftObjectRef<Asset> editorSoft;
#endif
};

// 편집기 전용 에셋 클래스다(쿠킹 대상이 아니다 — NeverCook과 같은 규칙).
ROCKS_CLASS(EditorOnly)
struct EditorToolAsset final : Asset {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() int32 value = 0;
};

// 편집기 전용 하위 객체 클래스다(쿠킹 패키지 쓰기가 부분 트리째 빼고 참조를 null로 쓴다).
ROCKS_CLASS(EditorOnly)
struct EditorNote final : Object {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() int32 value = 0;
};

} // namespace rocks::asset_fixture
