#pragma once

#include <AssetRuntime/PackageReader.h>
#include <AssetRuntime/PackageTags.h>
#include <Core/Containers/Array.h>
#include <Object/ObjectRegistry.h>

// 에셋 타입 hook 표다(Docs/Design/AssetSystem.ko.md 7.1절 15, 16.2절 — A2c1). 에디터 저장이 대표
// 에셋에 hook을 불러 Tags 섹션(태그 — UE UObject::GetAssetRegistryTags 대응)과 Build 의존(16.2절
// DependencyDeclarer)을 만든다. 런타임은 쓰지 않는다(쿠커 A4가 Build hook을 쓴다).
//
// 조회: 대표 에셋 타입에서 base 사슬(TypeSchema::baseType)을 따라 가장 가까운 등록 hook 하나를
// 부르고, hook은 CallBase로 그 위의 등록 hook을 부를 수 있다(UE Super::GetAssetRegistryTags —
// 하위 타입이 부모 태그를 끄거나 바꿀 수 있다).
//
// 사용 예(타입 모듈 등록 — 등록 handle을 모듈 수명 동안 쥔다):
//   Result<void> TextureTags(const AssetTypeHookContext& context, AssetTagSink& sink) noexcept {
//       if (auto base = context.CallBaseTags(sink); !base) { return base; }
//       return sink.Add("Texture.Format", "BC7");
//   }
//   auto registration = table.Register(GeneratedType<Texture2D>::Id(), {&TextureTags, nullptr});
//
// 스레드: 등록·조회는 한 스레드에서 한다(에디터 Registry 소유 스레드).
namespace rocks {

class AssetTypeHookTable;
class TypeRegistry;

// 태그를 모은다. (키, 값) UTF-8 byte 순으로 두고 같은 쌍은 하나로 합친다. Write가 Tags Record
// byte(PackageTags 값 스키마)를 만든다.
class AssetTagSink {
public:
    explicit AssetTagSink(Allocator& allocator) noexcept : tags_(allocator) {
    }

    // 오류: 빈 키·키 256 byte 초과·값 4 KiB 초과·잘못된 UTF-8 InvalidArgument, 할당 실패.
    Result<void> Add(StringView key, StringView value) noexcept;

    Span<const PackageTag> Tags() const noexcept {
        return tags_.View();
    }

    // 모은 태그의 Tags Record byte다(없으면 빈 byte). 오류: WritePackageTags의 오류.
    Result<Array<uint8>> Write(Allocator& allocator) const noexcept;

private:
    Array<PackageTag> tags_;
};

// Build 의존을 모은다. AssetId 순으로 정렬하고 같은 id는 하나로 합친다.
class AssetDependencySink {
public:
    explicit AssetDependencySink(Allocator& allocator) noexcept : dependencies_(allocator) {
    }

    // 오류: null id InvalidArgument, 할당 실패.
    Result<void> Add(AssetId asset) noexcept;

    // Dependencies 항목(종류 Build — PackageWriteOptions::extraDependencies에 넣는다)이다.
    Span<const PackageDependencyEntry> Dependencies() const noexcept {
        return dependencies_.View();
    }

private:
    Array<PackageDependencyEntry> dependencies_;
};

// hook에 넘기는 문맥이다(hook 호출 동안만 유효하다).
struct AssetTypeHookContext {
    ObjectRegistry& registry;
    const ObjectAccessContext& access;
    ObjectHandle object;
    // 지금 부른 hook이 등록된 타입이다(CallBase가 그 위부터 찾는다).
    TypeId hookType;
    const AssetTypeHookTable& table;
    TypeRegistry& types;

    // base 사슬에서 hookType 위의 가장 가까운 태그·Build hook을 부른다(없으면 아무것도 하지 않고
    // 성공). 오류: 그 hook의 오류, 타입 조회 오류.
    Result<void> CallBaseTags(AssetTagSink& sink) const noexcept;
    Result<void> CallBaseBuildDependencies(AssetDependencySink& sink) const noexcept;
};

using AssetTagHook = Result<void> (*)(const AssetTypeHookContext& context,
                                      AssetTagSink& sink) noexcept;
using AssetBuildDependencyHook = Result<void> (*)(const AssetTypeHookContext& context,
                                                  AssetDependencySink& sink) noexcept;

// 타입 하나의 hook이다(둘 다 null일 수 있다 — null이면 그 종류는 base 사슬 위로 찾는다).
struct AssetTypeHooks {
    AssetTagHook tags = nullptr;
    AssetBuildDependencyHook buildDependencies = nullptr;
};

// 등록 handle이다. 소멸하거나 Release하면 표에서 그 항목을 지운다(모듈 등록 수명에 묶는다).
class AssetTypeHookRegistration {
public:
    AssetTypeHookRegistration() noexcept = default;
    AssetTypeHookRegistration(AssetTypeHookRegistration&& other) noexcept;
    AssetTypeHookRegistration& operator=(AssetTypeHookRegistration&& other) noexcept;
    AssetTypeHookRegistration(const AssetTypeHookRegistration&) = delete;
    AssetTypeHookRegistration& operator=(const AssetTypeHookRegistration&) = delete;
    ~AssetTypeHookRegistration();

    void Release() noexcept;

    bool IsActive() const noexcept {
        return table_ != nullptr;
    }

private:
    friend class AssetTypeHookTable;

    AssetTypeHookTable* table_ = nullptr;
    TypeId type{};
};

class AssetTypeHookTable {
public:
    AssetTypeHookTable(Allocator& allocator, TypeRegistry& types) noexcept;
    // 모든 등록 handle을 놓은 뒤에 소멸해야 한다(남아 있으면 Fatal — 수명 계약 위반).
    ~AssetTypeHookTable();
    AssetTypeHookTable(const AssetTypeHookTable&) = delete;
    AssetTypeHookTable& operator=(const AssetTypeHookTable&) = delete;

    // type의 hook을 등록한다. 오류: 0 type·둘 다 null인 hooks InvalidArgument, 이미 등록한 type
    // AlreadyExists, 할당 실패. 표는 등록 handle보다 오래 살아야 한다.
    Result<AssetTypeHookRegistration> Register(TypeId type, AssetTypeHooks hooks) noexcept;

    // object(대표 에셋 — 타입 type)의 태그·Build 의존을 sink에 모은다. type에서 base 사슬로 가장
    // 가까운 hook 하나를 부른다(없으면 아무것도 하지 않고 성공). 오류: hook의 오류, 타입 조회
    // 오류(미등록 타입은 그 오류).
    Result<void> CollectTags(ObjectRegistry& registry, const ObjectAccessContext& access,
                             ObjectHandle object, TypeId type, AssetTagSink& sink) const noexcept;
    Result<void> CollectBuildDependencies(ObjectRegistry& registry,
                                          const ObjectAccessContext& access, ObjectHandle object,
                                          TypeId type, AssetDependencySink& sink) const noexcept;

    usize Count() const noexcept {
        return entries_.Size();
    }

private:
    friend struct AssetTypeHookContext;
    friend class AssetTypeHookRegistration;

    struct Entry {
        TypeId type{};
        AssetTypeHooks hooks{};
    };

    // start부터(skipStart면 start의 base부터) base 사슬로 kind hook이 있는 가장 가까운 항목을
    // 찾는다(없으면 null). 오류: 사슬의 타입 조회 오류.
    Result<const Entry*> Find(TypeId start, bool tags, bool skipStart) const noexcept;
    void Remove(TypeId type) noexcept;

    TypeRegistry* types_;
    Array<Entry> entries_;
};

} // namespace rocks
