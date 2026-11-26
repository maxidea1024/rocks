#pragma once

#include "../../AssetRuntime/Tests/LoaderTestSupport.h"

#include <AssetRuntime/AssetManager.h>
#include <Core/Text/StringBuilder.h>
#include <InputActions/Haptics/HapticEffectAsset.generated.h>
#include <InputActions/Haptics/HapticEffectAsset.h>
#include <InputActions/InputAction.generated.h>
#include <InputActions/InputAction.h>
#include <InputActions/InputActionsModule.h>
#include <InputActions/InputContext.generated.h>
#include <InputActions/InputContext.h>

// 입력 에셋 쿠킹 장면이다(Docs/Design/InputActions 2.8절 IA6a·5.4절 IA6b 시험). AssetEnv의 source
// Registry에 패키지를 만들어 쿠킹 byte로 쓰고, 메모리 io·매니페스트로 AssetManager를 띄운다.
// InputActions·Gameplay 시험이 같이 쓴다. lease는 장면보다 먼저 놓아야 한다(AssetManager 파괴가
// 남은 lease를 Fatal로 본다).
namespace rocks::input_asset_test {

constexpr uint64 IdHigh = 0x49413641;
constexpr uint64 RegistryContextHigh = 0x494e505554ull;
constexpr usize PathCapacity = 128;

inline Guid Id(uint64 low) noexcept {
    return Guid{IdHigh, low};
}

class InputAssetScene {
public:
    explicit InputAssetScene(asset_test::AssetEnv& env) noexcept
        : manifest(env.allocator), io(env.allocator), worker(env.allocator), env_(env),
          packages_(env.allocator), ids_(env.allocator), assets_(env.allocator),
          files_(env.allocator) {
        registered_ = env.Ready() && input::RegisterInputActionsTypes(env.modules).IsOk();
    }

    InputAssetScene(const InputAssetScene&) = delete;
    InputAssetScene& operator=(const InputAssetScene&) = delete;

    bool Ready() const noexcept {
        return registered_;
    }

    ObjectRegistry& Source() noexcept {
        return *env_.source;
    }

    // id 패키지(쿠킹 종류)와 그 대표 에셋 T를 만든다. 실패하면 null handle이다.
    template <class T>
    ObjectHandle AddAsset(uint64 id) noexcept {
        ObjectRegistry& source = Source();
        auto made =
            NewPackage(source, source.OwnerAccess(), asset_test::FixedId(id), PackageKind::Cooked);
        if (!made) {
            return ObjectHandle{};
        }

        const ObjectHandle asset =
            asset_test::Make<T>(source, TypeIdOf<T>(), made.Value().Handle(), "");
        if (asset.IsNull() || !packages_.TryPushBack(Move(made.Value())) || !ids_.TryPushBack(id) ||
            !assets_.TryPushBack(asset)) {
            return ObjectHandle{};
        }

        return asset;
    }

    // source Registry에서 만든 id 패키지의 대표 에셋이다(없으면 null).
    ObjectHandle SourceAsset(uint64 id) const noexcept {
        for (usize index = 0; index < ids_.Size(); ++index) {
            if (ids_[index] == id) {
                return assets_[index];
            }
        }

        return ObjectHandle{};
    }

    // 액션 에셋 하나를 만든다. actionId가 0이면 실효 id는 패키지 AssetId다(2.8절).
    ObjectHandle AddAction(uint64 id, const Guid& actionId, input::InputValueType type) noexcept {
        return FillAction(AddAsset<input::InputAction>(id), actionId, type);
    }

    // 이미 만든 package의 대표 에셋 아래에 하위 객체 액션(대표가 아님 — assetId null)을
    // 만든다(패키지 바로 아래에는 대표 에셋만 둔다).
    ObjectHandle AddInnerAction(uint64 package, StringView name, const Guid& actionId,
                                input::InputValueType type) noexcept {
        for (usize index = 0; index < ids_.Size(); ++index) {
            if (ids_[index] == package) {
                return FillAction(
                    asset_test::Make<input::InputAction>(Source(), TypeIdOf<input::InputAction>(),
                                                         assets_[index], name),
                    actionId, type);
            }
        }

        return ObjectHandle{};
    }

    // 컨텍스트 에셋 하나를 만든다(actions·bindings는 아래 함수로 채운다).
    ObjectHandle AddContext(uint64 id, const Guid& contextId) noexcept {
        const ObjectHandle asset = AddAsset<input::InputContext>(id);
        input::InputContext* context = asset_test::Pointer<input::InputContext>(Source(), asset);
        if (context == nullptr) {
            return ObjectHandle{};
        }

        context->id = contextId;
        return asset;
    }

    // 컨텍스트의 actions에 액션을 더한다(다른 패키지면 Hard 의존이 된다).
    bool LinkAction(ObjectHandle context, ObjectHandle action) noexcept {
        ObjectRegistry& source = Source();
        return asset_test::WithBorrow<input::InputContext>(
            source, context, [&](input::InputContext& owner) {
                return source.AddReference(source.OwnerAccess(), context, owner.actions, action)
                    .IsOk();
            });
    }

    // 컨텍스트에 바인딩(action ← key)을 더한다.
    bool AddBinding(ObjectHandle context, const Guid& action, const char* key) noexcept {
        input::InputContext* owner = asset_test::Pointer<input::InputContext>(Source(), context);
        if (owner == nullptr) {
            return false;
        }

        input::InputBinding binding(env_.allocator);
        binding.action = action;
        return binding.key.TryAssign(key).IsOk() &&
               owner->data.bindings.TryPushBack(Move(binding)).IsOk();
    }

    // 진동 효과 에셋 하나를 만든다(채널 하나 — 0초 1, duration초 0). effectId가 0이면 실효 id는
    // 패키지 AssetId다.
    ObjectHandle AddHaptic(uint64 id, const Guid& effectId, float duration) noexcept {
        const ObjectHandle asset = AddAsset<input::HapticEffectAsset>(id);
        input::HapticEffectAsset* haptic =
            asset_test::Pointer<input::HapticEffectAsset>(Source(), asset);
        if (haptic == nullptr) {
            return ObjectHandle{};
        }

        haptic->effect.id = effectId;
        input::HapticChannel channel(env_.allocator);
        if (!channel.curve.TryAddKey(0.0f, 1.0f) || !channel.curve.TryAddKey(duration, 0.0f)) {
            return ObjectHandle{};
        }

        return haptic->effect.channels.TryPushBack(Move(channel)).IsOk() ? asset : ObjectHandle{};
    }

    // 만든 패키지를 모두 쿠킹 byte로 써서 매니페스트·io에 넣고 AssetManager를 띄운다. 매니페스트는
    // AssetId 오름차순이라 패키지는 id 오름차순으로 만든다(아니면 실패).
    bool Start() noexcept {
        ObjectRegistry& source = Source();
        asset_test::FillManifestHeader(manifest);
        for (usize index = 0; index < packages_.Size(); ++index) {
            if (index != 0 && ids_[index - 1] >= ids_[index]) {
                return false;
            }

            Array<uint8> file = asset_test::Write(env_.allocator, source, packages_[index].Handle(),
                                                  asset_test::CookedOptions());
            if (file.IsEmpty()) {
                return false;
            }

            auto added = manifest.entries.TryEmplaceBack(env_.allocator);
            if (!added ||
                !asset_test::FillLoaderEntry(env_.allocator, *added.Value(), file, ids_[index]) ||
                !files_.TryPushBack(Move(file))) {
                return false;
            }
        }

        // io는 byte를 Span으로 본다 — files_가 다 찬 뒤에 넣는다.
        for (usize index = 0; index < files_.Size(); ++index) {
            StringBuilder<PathCapacity> path(env_.allocator);
            if (!asset_test::LoaderPackagePath(path, Root, ids_[index],
                                               manifest.entries[index].packageSha256) ||
                !io.Add(path.View(), files_[index].View())) {
                return false;
            }
        }

        if (!ValidateCookedManifest(manifest, env_.allocator)) {
            return false;
        }

        config.loader.allocator = &env_.allocator;
        config.loader.types = &env_.types;
        config.loader.manifest = &manifest;
        config.loader.cookedRoot = Root;
        config.loader.runtimeProfile = "Windows";
        config.loader.io = &io;
        config.loader.worker = &worker;
        config.registryContext = ContextId{RegistryContextHigh, 1};
        config.registryOptions.names = &env_.names;
        auto made = UniquePtr<AssetManager>::TryMake(env_.allocator, config);
        if (!made) {
            return false;
        }

        manager = Move(made.Value());
        return manager->Initialize().IsOk();
    }

    // id 에셋을 동기로 로드한다.
    Result<AssetLease> Load(uint64 id) noexcept {
        AssetRequest request;
        request.id = asset_test::FixedId(id);
        return manager->LoadBlocking(request, "InputAssetScene.Load");
    }

    bool IsLoaded(uint64 id) noexcept {
        return manager->Loader().FindPackage(asset_test::FixedId(id)).IsOk();
    }

    // id 에셋의 상주 단위를 붙잡은 lease 수다(상주가 아니면 0). lease 소멸은 다음 Pump가 반영한다.
    uint32 Leases(uint64 id) noexcept {
        AssetManagerSnapshot snapshot(env_.allocator);
        if (!manager->Describe(snapshot)) {
            return 0;
        }

        for (const AssetManagerUnitInfo& info : snapshot.units) {
            for (uint32 index = 0; index < info.assetCount; ++index) {
                if (snapshot.unitAssets[info.firstAsset + index] == asset_test::FixedId(id)) {
                    return info.leases;
                }
            }
        }

        return 0;
    }

    // Pump로 lease 소멸을 반영하고 id 에셋의 lease 수를 돌려준다(Pump 실패면 Max).
    uint32 LeasesAfterPump(uint64 id) noexcept {
        return manager->Pump() ? Leases(id) : NumericLimits<uint32>::Max();
    }

    static constexpr StringView Root = "cooked";

    CookedManifest manifest;
    asset_test::TestPackageIo io;
    asset_test::TestLoaderWorker worker;
    AssetManagerConfig config;

private:
    ObjectHandle FillAction(ObjectHandle asset, const Guid& actionId,
                            input::InputValueType type) noexcept {
        input::InputAction* action = asset_test::Pointer<input::InputAction>(Source(), asset);
        if (action == nullptr) {
            return ObjectHandle{};
        }

        action->id = actionId;
        action->settings.valueType = type;
        return asset;
    }

    asset_test::AssetEnv& env_;
    bool registered_ = false;
    Array<RootRef<Package>> packages_;
    Array<uint64> ids_;
    Array<ObjectHandle> assets_;
    Array<Array<uint8>> files_;

public:
    // 마지막 멤버라 가장 먼저 파괴된다(io·files_·매니페스트보다 먼저 — 로더가 그것을 본다).
    UniquePtr<AssetManager> manager;
};

} // namespace rocks::input_asset_test
