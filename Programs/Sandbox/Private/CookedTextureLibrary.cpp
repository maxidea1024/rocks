#include "CookedTextureLibrary.h"

#include <Core/Diagnostics/Log.h>
#include <Core/Move.h>
#include <Meta/GeneratedType.h>
#include <Texture/Texture.generated.h>
#include <Texture/Texture.h>

namespace sandbox {

namespace {

inline constexpr LogCategory LogSandboxTextures{"Sandbox.Textures"};

} // namespace

CookedTextureLibrary::CookedTextureLibrary(Allocator& allocator, AssetManager& manager,
                                           const CookedManifest& manifest, TypeRegistry& types,
                                           screen::ScreenHost& screen) noexcept
    : allocator_(allocator), manifest_(manifest), types_(types), screen_(screen),
      cache_(allocator, manager, screen.Lane(), screen.Device()), entries_(allocator) {
    screen_.AttachTextureCache(&cache_);
}

CookedTextureLibrary::~CookedTextureLibrary() noexcept {
    Release();
    screen_.AttachTextureCache(nullptr);
}

Result<uint32> CookedTextureLibrary::RequestAll() noexcept {
    const TypeId textureType = GeneratedType<Texture>::Id();
    // 경로 순서로 요청한다(매니페스트는 AssetId 순 — 표시 목록 순서를 사람이 읽기 쉽게).
    Array<uint32> order(allocator_);
    for (uint32 index = 0; index < manifest_.entries.Size(); ++index) {
        auto type = types_.Acquire(manifest_.entries[index].type);
        if (type && type.Value().IsA(textureType)) {
            if (auto pushed = order.TryPushBack(index); !pushed) {
                return Result<uint32>::Failure(pushed.Error());
            }
        }
    }

    for (usize i = 1; i < order.Size(); ++i) {
        for (usize j = i; j > 0; --j) {
            const StringView left = manifest_.entries[order[j - 1]].pathHint.View();
            const StringView right = manifest_.entries[order[j]].pathHint.View();
            if (StringView::CompareBytes(right, left) >= 0) {
                break;
            }

            const uint32 swap = order[j - 1];
            order[j - 1] = order[j];
            order[j] = swap;
        }
    }

    for (const uint32 index : order) {
        const CookedManifestEntry& source = manifest_.entries[index];
        Entry entry(allocator_);
        entry.asset = source.asset;
        if (auto copied = entry.path.TryAppend(source.pathHint.View()); !copied) {
            return Result<uint32>::Failure(copied.Error());
        }

        if (auto pushed = entries_.TryReserve(entries_.Size() + 1); !pushed) {
            return Result<uint32>::Failure(pushed.Error());
        }

        if (auto acquired = cache_.Acquire(source.asset); !acquired) {
            Log::Warning(LogSandboxTextures, "Texture {Path} could not be requested: {Error}",
                         entry.path.View(), ErrorCodeName(acquired.Error()));
            continue;
        }

        (void)entries_.TryPushBack(Move(entry));
    }

    Log::Info(LogSandboxTextures, "Requested {Count} cooked textures", entries_.Size());
    return Result<uint32>::Success(static_cast<uint32>(entries_.Size()));
}

void CookedTextureLibrary::Show(Entry& entry, const TextureRenderInfo& info) noexcept {
    // 업로드 전 실패(로드·변환·검사·게시 — handle 없음)는 표시하지 않는다.
    if (info.state == TextureRenderState::Failed && info.handle.IsNull()) {
        entry.state = State::Failed;
        entry.error = info.error;
        ++failed_;
        Log::Warning(LogSandboxTextures, "Texture {Path} could not be uploaded: {Error} ({Issue})",
                     entry.path.View(), ErrorCodeName(info.error),
                     render::CookedTextureIssueName(info.issue));
        return;
    }

    auto added = screen_.AddCachedTexture(entry.path.View(), info);
    if (!added) {
        entry.state = State::Failed;
        entry.error = added.Error();
        ++failed_;
        Log::Warning(LogSandboxTextures, "Texture {Path} could not be listed: {Error}",
                     entry.path.View(), ErrorCodeName(added.Error()));
        return;
    }

    entry.inspector = added.Value();
    entry.state = State::Uploaded;
    ++uploaded_;
}

void CookedTextureLibrary::Update() noexcept {
    if (released_ || !screen_.IsRunning()) {
        return;
    }

    (void)cache_.Update();
    for (Entry& entry : entries_) {
        TextureRenderInfo info;
        if (entry.state != State::Requested || !cache_.Find(entry.asset, info) ||
            info.state == TextureRenderState::Loading) {
            continue;
        }

        Show(entry, info);
    }
}

void CookedTextureLibrary::Release() noexcept {
    if (released_) {
        return;
    }

    released_ = true;
    for (const Entry& entry : entries_) {
        (void)cache_.Release(entry.asset);
    }

    // 남은 handle을 Retire하고 lease·티켓을 놓는다(lease 소멸은 해제 큐 — 다음 Pump).
    cache_.Shutdown();
}

uint32 CookedTextureLibrary::EntryOf(uint32 request) const noexcept {
    return request < entries_.Size() ? entries_[request].inspector
                                     : screen::ScreenHost::NoInspectorEntry;
}

ErrorCode CookedTextureLibrary::FailureOf(uint32 request) const noexcept {
    return request < entries_.Size() ? entries_[request].error : ErrorCode::None;
}

StringView CookedTextureLibrary::PathOf(uint32 request) const noexcept {
    return request < entries_.Size() ? entries_[request].path.View() : StringView();
}

Result<void> CookedTextureLibrary::InspectCookedData(uint32 request,
                                                     TextureRenderCache::PlatformDataVisitor visit,
                                                     void* context) noexcept {
    if (request >= entries_.Size()) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    return cache_.WithPlatformData(entries_[request].asset, visit, context);
}

} // namespace sandbox
