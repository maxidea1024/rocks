#pragma once

#include "../../../Runtime/AssetRuntime/Tests/AssetTestSupport.h"
#include "../../../Runtime/AssetRuntime/Tests/PackageEditing.h"
#include "../../AssetDatabase/Tests/MemoryFileSystem.h"

#include <AssetCooker/CookTargets.h>
#include <AssetRuntime/PackageFormat.h>
#include <AssetRuntime/PackageTags.h>
#include <Core/Text/StringBuilder.h>
#include <TestSupport/Test.h>

// AssetCooker 시험 공용 장면이다(Docs/Design/AssetSystem.ko.md 10.2절 — A4b·A4c): 노드 그래프를
// 메모리 Content에 패키지로 쓰고(의존 섹션을 시험이 다시 써서 import 없이 어떤 그래프든) 색인을
// 만든다, 쿠킹 규칙 조립, 대상·진단 대조.
namespace rocks::cook_test {

using namespace rocks::asset_test;
using rocks::asset_database_test::MemoryFault;
using rocks::asset_database_test::MemoryFileSystem;
using rocks::asset_database_test::MemoryRoot;
using C = AssetDiagnosticCode;
using K = PackageDependencyKind;

enum class NodeType {
    Test,
    Base,
    Derived
};

struct Dep {
    uint64 id;
    PackageDependencyKind kind;
};

// 그래프 노드 하나다(Content 상대 파일 이름, id, 대표 타입, 의존, 태그). 같은 id 노드는 앞 노드의
// byte를 그대로 쓴다(같은 id 파일).
struct Node {
    StringView file;
    uint64 id;
    NodeType type = NodeType::Test;
    Span<const Dep> deps{};
    Span<const PackageTagInput> tags{};
};

// id 패키지 byte를 만든다: 대표 에셋 하나와 태그로 쓰고, Dependencies 섹션을 deps로 바꾼다(색인은
// 의존 섹션만 읽으므로 import 없이 어떤 그래프든 만든다 — Hard의 expectedType은 TestAsset).
inline Array<uint8> GraphPackage(AssetEnv& env, ObjectRegistry& registry,
                                 const Node& node) noexcept {
    Array<uint8> none(env.allocator);
    auto made = NewPackage(registry, registry.OwnerAccess(), FixedId(node.id), PackageKind::Editor);
    if (!made) {
        return none;
    }

    RootRef<Package> package = Move(made.Value());
    ObjectHandle asset{};
    if (node.type == NodeType::Test) {
        asset = Make<TestAsset>(registry, IdOf<TestAsset>(), package.Handle(), "");
    } else if (node.type == NodeType::Base) {
        asset = Make<BaseGameAsset>(registry, IdOf<BaseGameAsset>(), package.Handle(), "");
    } else {
        asset = Make<DerivedGameAsset>(registry, IdOf<DerivedGameAsset>(), package.Handle(), "");
    }

    auto tags = WritePackageTags(env.allocator, node.tags);
    if (asset.IsNull() || !tags) {
        return none;
    }

    PackageWriteOptions options = EditorOptions();
    options.tags = tags.Value().View();
    Array<uint8> bytes = Write(env.allocator, registry, package.Handle(), options);
    if (bytes.IsEmpty() || node.deps.IsEmpty()) {
        return bytes;
    }

    // (asset, kind) 순으로 정렬한다(읽기 규칙).
    Array<PackageDependencyEntry> entries(env.allocator);
    for (const Dep& dep : node.deps) {
        const PackageDependencyEntry entry{FixedId(dep.id), dep.kind,
                                           dep.kind == K::Hard ? TestAssetType() : TypeId{}, false};
        usize at = 0;
        while (at < entries.Size() && package_detail::DependencyLess(entries[at], entry)) {
            ++at;
        }

        if (!entries.TryInsert(entry, at)) {
            return none;
        }
    }

    EditablePackage edit(env.allocator);
    if (!edit.Parse(bytes.View())) {
        return none;
    }

    (void)edit.Remove(PackageSectionKind::Dependencies);
    const Array<uint8> section =
        DependenciesBytes(env.allocator, entries.View(), static_cast<uint32>(entries.Size()));
    if (edit.Insert(static_cast<uint32>(PackageSectionKind::Dependencies),
                    PackageSectionRequiredFlags(PackageSectionKind::Dependencies),
                    section.View()) == nullptr) {
        return none;
    }

    return edit.Build();
}

// 표준 문자열 id 표다(만든 뒤 자라지 않는다 — 태그 값 StringView가 가리킨다).
struct IdTexts {
    IdTexts(Allocator& allocator, Span<const uint64> ids) noexcept : texts(allocator) {
        ready = static_cast<bool>(texts.TryReserve(ids.Size()));
        for (uint64 id : ids) {
            auto text = TryFormat(allocator, "{}", FixedId(id));
            ready = ready && text && texts.TryPushBack(Move(text.Value()));
        }

        keys = ids;
    }

    StringView operator()(uint64 id) const noexcept {
        for (usize index = 0; index < keys.Size(); ++index) {
            if (keys[index] == id) {
                return texts[index].View();
            }
        }

        CHECK(false);
        return {};
    }

    Array<String> texts;
    Span<const uint64> keys;
    bool ready = false;
};

struct CookScene {
    explicit CookScene(AssetEnv& sceneEnv) noexcept
        : env(sceneEnv), layout(env.allocator), files(env.allocator, Initialized(layout)),
          index(env.allocator, layout), settings(env.allocator) {
    }

    static StringView Initialized(ProjectLayout& layout) noexcept {
        return layout.Initialize(MemoryRoot) ? layout.ContentDirectory() : StringView(MemoryRoot);
    }

    // 노드를 파일로 쓰고(부모 디렉터리를 만든다) 색인을 만든다.
    bool Build(Span<const Node> nodes) noexcept {
        if (!files.Ready()) {
            return false;
        }

        Array<Array<uint8>> made(env.allocator);
        for (usize at = 0; at < nodes.Size(); ++at) {
            const Node& node = nodes[at];
            Array<uint8> bytes(env.allocator);
            for (usize before = 0; before < at; ++before) {
                if (nodes[before].id == node.id && bytes.IsEmpty()) {
                    (void)bytes.TryAppend(made[before].View());
                }
            }

            if (bytes.IsEmpty()) {
                bytes = GraphPackage(env, *env.source, node);
            }

            if (bytes.IsEmpty() || !AddParents(node.file) ||
                !files.AddFile(node.file, bytes.View()) || !made.TryPushBack(Move(bytes))) {
                return false;
            }
        }

        return static_cast<bool>(index.Rebuild(files, PackageReadOptions{}));
    }

    bool AddParents(StringView file) noexcept {
        for (usize at = 0; at < file.ByteCount(); ++at) {
            if (file.Data()[at] != '/') {
                continue;
            }

            const StringView parent(file.Data(), at);
            if (!files.Has(parent) && !files.AddDirectory(parent)) {
                return false;
            }
        }

        return true;
    }

    // file을 node의 새 byte로 바꾸고(같은 id는 원본 Registry에 다시 만들 수 없어 target Registry로
    // 쓴다) 수정 시각을 옮긴 뒤 색인을 다시 만든다(내용이 바뀐 파일).
    bool Replace(const Node& node) noexcept {
        Array<uint8> bytes = GraphPackage(env, *env.target, node);
        files.Remove(node.file);
        ++files.modifiedShift;
        return !bytes.IsEmpty() && files.AddFile(node.file, bytes.View()) &&
               static_cast<bool>(index.Rebuild(files, PackageReadOptions{}));
    }

    CookTargetRequest Request(Span<const StringView> paths = {},
                              const CookedManifest* existing = nullptr) noexcept {
        CookTargetRequest request;
        request.index = &index;
        request.types = &env.types;
        request.settings = &settings;
        request.paths = paths;
        request.existing = existing;
        return request;
    }

    void ResetSettings() noexcept {
        settings.alwaysCook.Clear();
        settings.neverCook.Clear();
        settings.primaryAssets.Clear();
        settings.cookSoftReferences = false;
    }

    AssetEnv& env;
    ProjectLayout layout;
    MemoryFileSystem files;
    AssetIndex index;
    CookSettings settings;
};

inline bool Push(Array<String>& list, StringView text) noexcept {
    String value(list.GetAllocator());
    return value.TryAssign(text) && list.TryPushBack(Move(value));
}

inline bool AddRule(CookSettings& settings, StringView type,
                    Span<const StringView> directories) noexcept {
    PrimaryAssetRule rule(settings.primaryAssets.GetAllocator());
    if (!rule.type.TryAssign(type)) {
        return false;
    }

    for (StringView directory : directories) {
        if (!Push(rule.directories, directory)) {
            return false;
        }
    }

    return static_cast<bool>(settings.primaryAssets.TryPushBack(Move(rule)));
}

inline AssetId IdOrNull(uint64 id) noexcept {
    return id == 0 ? AssetId{} : FixedId(id);
}

inline bool SameTargets(const CookTargetSet& set, Span<const uint64> ids) noexcept {
    if (set.targets.Size() != ids.Size()) {
        return false;
    }

    for (usize index = 0; index < ids.Size(); ++index) {
        if (!(set.targets[index].id == FixedId(ids[index]))) {
            return false;
        }
    }

    return true;
}

inline bool SameIds(const Array<AssetId>& list, Span<const uint64> ids) noexcept {
    if (list.Size() != ids.Size()) {
        return false;
    }

    for (usize index = 0; index < ids.Size(); ++index) {
        if (!(list[index] == FixedId(ids[index]))) {
            return false;
        }
    }

    return true;
}

inline const CookTarget* TargetOf(const CookTargetSet& set, uint64 id) noexcept {
    for (const CookTarget& target : set.targets) {
        if (target.id == FixedId(id)) {
            return &target;
        }
    }

    return nullptr;
}

// 기대 진단이다(asset·related 0은 null).
struct ExpectedDiagnostic {
    AssetDiagnosticCode code;
    bool warning;
    uint64 asset;
    uint64 related;
    uint32 setting;
    ErrorCode error = ErrorCode::None;
};

// 시험 쪽 순서다((asset, code, related, setting, 오류 먼저, 원인 오류) — 구현과 따로 정의한다).
inline bool DiagnosticBefore(const ExpectedDiagnostic& left,
                             const ExpectedDiagnostic& right) noexcept {
    const AssetId leftAsset = IdOrNull(left.asset);
    const AssetId rightAsset = IdOrNull(right.asset);
    if (!(leftAsset == rightAsset)) {
        return leftAsset.value.high != rightAsset.value.high
                   ? leftAsset.value.high < rightAsset.value.high
                   : leftAsset.value.low < rightAsset.value.low;
    }

    if (left.code != right.code) {
        return static_cast<uint32>(left.code) < static_cast<uint32>(right.code);
    }

    if (left.related != right.related) {
        return left.related < right.related;
    }

    if (left.setting != right.setting) {
        return left.setting < right.setting;
    }

    if (left.warning != right.warning) {
        return !left.warning;
    }

    return static_cast<uint32>(left.error) < static_cast<uint32>(right.error);
}

// diagnostics가 expected(순서 무관 — 시험 쪽 순서로 정렬해 대조)와 같은지 본다.
inline bool SameDiagnostics(const Array<CookDiagnostic>& diagnostics,
                            Span<const ExpectedDiagnostic> expected) noexcept {
    if (diagnostics.Size() != expected.Size()) {
        return false;
    }

    Array<ExpectedDiagnostic> sorted(diagnostics.GetAllocator());
    for (const ExpectedDiagnostic& item : expected) {
        usize at = 0;
        while (at < sorted.Size() && !DiagnosticBefore(item, sorted[at])) {
            ++at;
        }

        if (!sorted.TryInsert(item, at)) {
            return false;
        }
    }

    for (usize index = 0; index < sorted.Size(); ++index) {
        const CookDiagnostic& actual = diagnostics[index];
        const ExpectedDiagnostic& item = sorted[index];
        if (actual.code != item.code || actual.warning != item.warning ||
            !(actual.asset == IdOrNull(item.asset)) ||
            !(actual.related == IdOrNull(item.related)) || actual.setting != item.setting ||
            actual.error != item.error) {
            return false;
        }
    }

    return true;
}

} // namespace rocks::cook_test
