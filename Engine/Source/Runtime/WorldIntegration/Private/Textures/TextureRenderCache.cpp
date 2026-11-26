#include <WorldIntegration/Textures/TextureRenderCache.h>

#include <AssetRuntime/Package.generated.h>
#include <AssetRuntime/Package.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Move.h>
#include <Meta/GeneratedType.h>
#include <Object/ObjectRegistry.h>
#include <Texture/Texture.generated.h>
#include <Texture/Texture.h>

namespace rocks {

namespace {

inline constexpr LogCategory LogTextureRenderCache{"WorldIntegration.Textures"};

// 패키지 bulk reader 문맥이다(패키지는 With 범위 동안 산다).
struct CachePackageReader {
    const Package* package = nullptr;
};

Result<Span<const uint8>> ReadCachePackageBulk(void* context, const BulkDataRef& ref) noexcept {
    const auto* reader = static_cast<const CachePackageReader*>(context);
    return reader->package->BulkBytes(ref);
}

// lease의 대표 텍스처와 그 패키지를 열어 fn(texture, reader)을 부른다(With 범위 안).
template <class Fn>
Result<void> WithLeasedTexture(AssetManager& manager, const AssetLease& lease, Fn&& fn) noexcept {
    Result<void> result = Result<void>::Success();
    auto opened = manager.With(lease, [&](ObjectHandle object) noexcept {
        ObjectRegistry& registry = manager.Registry();
        const ObjectAccessContext access = registry.OwnerAccess();
        auto texture = registry.Resolve<Texture>(access, object);
        auto package = registry.Resolve<Package>(access, lease.Package());
        if (!texture || !package) {
            result = Result<void>::Failure(!texture ? texture.Error() : package.Error());
            return;
        }

        auto textureValue = texture.Value().Get();
        auto packageValue = package.Value().Get();
        if (!textureValue || !packageValue) {
            result =
                Result<void>::Failure(!textureValue ? textureValue.Error() : packageValue.Error());
            return;
        }

        CachePackageReader reader;
        reader.package = packageValue.Value();
        fn(*textureValue.Value(), reader);
    });
    if (!opened) {
        return opened;
    }

    return result;
}

} // namespace

const char* TextureRenderStateName(TextureRenderState state) noexcept {
    switch (state) {
    case TextureRenderState::Loading:
        return "Loading";
    case TextureRenderState::Uploading:
        return "Uploading";
    case TextureRenderState::Ready:
        return "Ready";
    case TextureRenderState::Failed:
        return "Failed";
    case TextureRenderState::CpuPlaceholder:
        return "CpuPlaceholder";
    }

    return "Unknown";
}

TextureRenderCache::TextureRenderCache(Allocator& allocator, AssetManager& manager,
                                       render::RenderLane& lane,
                                       const graphics::Device& device) noexcept
    : allocator_(allocator), manager_(manager), lane_(lane), device_(device), entries_(allocator),
      handles_(allocator) {
}

TextureRenderCache::~TextureRenderCache() noexcept {
    Shutdown();
}

Result<void> TextureRenderCache::Acquire(AssetId texture) noexcept {
    if (texture.IsNull()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (shutDown_) {
        return Result<void>::Failure(ErrorCode::ShuttingDown);
    }

    if (Entry* existing = entries_.Find(texture)) {
        ++existing->users;
        return Result<void>::Success();
    }

    AssetRequest request;
    request.id = texture;
    request.expectedType = GeneratedType<Texture>::Id();
    auto ticket = manager_.RequestAsset(request);
    if (!ticket) {
        return Result<void>::Failure(ticket.Error());
    }

    Entry entry;
    entry.serial = nextSerial_++;
    entry.users = 1;
    entry.ticket = Move(ticket.Value());
    const uint64 serial = entry.serial;
    if (auto inserted = entries_.Insert(texture, Move(entry)); !inserted) {
        return Result<void>::Failure(inserted.Error());
    }

    Entry* stored = entries_.Find(texture);
    auto callback = AssetReadyCallback::TryCreate(
        allocator_,
        [this, texture, serial](Result<AssetLease> lease, const AssetDiagnostic&) noexcept {
            OnReady(texture, serial, Move(lease));
        });
    if (!callback) {
        (void)entries_.Erase(texture);
        return Result<void>::Failure(callback.Error());
    }

    // 등록이 실패하면 callback은 등록되지 않으므로 항목(티켓)을 지워도 늦은 통지가 없다.
    if (auto registered = manager_.OnReady(stored->ticket, TaskLane::Game, Move(callback.Value()));
        !registered) {
        (void)entries_.Erase(texture);
        return registered;
    }

    return Result<void>::Success();
}

Result<void> TextureRenderCache::Release(AssetId texture) noexcept {
    Entry* entry = entries_.Find(texture);
    if (entry == nullptr) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    if (--entry->users != 0) {
        return Result<void>::Success();
    }

    // handle Retire → lease·티켓 놓기 순서다(RenderBase 6.5절 — 항목 소멸이 lease를 놓는다).
    RetireHandle(texture, *entry);
    (void)entries_.Erase(texture);
    return Result<void>::Success();
}

void TextureRenderCache::OnReady(AssetId texture, uint64 serial,
                                 Result<AssetLease> lease) noexcept {
    // Release·Shutdown 뒤 늦게 온 callback(놓은 티켓의 Cancelled, 종료의 ShuttingDown)은 무시한다.
    if (shutDown_) {
        return;
    }

    Entry* entry = entries_.Find(texture);
    if (entry == nullptr || entry->serial != serial) {
        return;
    }

    if (!lease) {
        entry->info.state = TextureRenderState::Failed;
        entry->info.error = lease.Error();
        Log::Warning(LogTextureRenderCache, "Texture {Asset} could not be loaded: {Error}", texture,
                     ErrorCodeName(lease.Error()));
        return;
    }

    entry->lease = Move(lease.Value());
}

uint32 TextureRenderCache::Update() noexcept {
    if (shutDown_) {
        return 0;
    }

    uint32 processed = 0;
    entries_.ForEach([this, &processed](const AssetId& texture, Entry& entry) noexcept {
        if (entry.info.state != TextureRenderState::Loading || !entry.lease.IsValid()) {
            return;
        }

        Upload(texture, entry);
        ++processed;
    });
    return processed;
}

void TextureRenderCache::Upload(AssetId texture, Entry& entry) noexcept {
    render::CookedTextureIssue issue = render::CookedTextureIssue::None;
    ErrorCode error = ErrorCode::None;
    auto opened = WithLeasedTexture(
        manager_, entry.lease, [&](const Texture& asset, CachePackageReader& reader) noexcept {
            // UE와 같이 CPU Availability는 GPU에 쿠킹 mip을 두지 않는다(RenderBase 6.5절 — 그리는
            // 쪽이 Black2D로 대체한다).
            if (asset.availability == TextureAvailability::CPU) {
                entry.info.state = TextureRenderState::CpuPlaceholder;
                return;
            }

            PlatformTextureSource source(allocator_);
            if (auto made = MakePlatformTextureSource(asset.platformData, &ReadCachePackageBulk,
                                                      &reader, source);
                !made) {
                error = made.Error();
                return;
            }

            auto handle = lane_.ReserveHandle(render::RenderResourceKind::Texture);
            if (!handle) {
                error = handle.Error();
                return;
            }

            // 뒤 단계가 실패하면 Fail이 이 handle을 Retire한다.
            entry.info.handle = handle.Value();
            auto upload = render::PrepareCookedTexture(allocator_, source.source, device_,
                                                       handle.Value(), &issue);
            if (!upload) {
                error = upload.Error();
                return;
            }

            if (auto mapped = handles_.Insert(HandleKey(handle.Value()), texture); !mapped) {
                error = mapped.Error();
                return;
            }

            if (auto posted =
                    lane_.PostResource(Move(upload.Value()), &render::CreateCookedTextureOnLane);
                !posted) {
                error = posted.Error();
                return;
            }

            TextureRenderInfo& info = entry.info;
            info.state = TextureRenderState::Uploading;
            info.dimension = source.source.dimension;
            info.format = source.source.format;
            info.swizzle = source.swizzle;
            info.width = source.source.width;
            info.height = source.source.height;
            info.depth = source.source.depth;
            info.slices = source.source.slices;
            info.mips = static_cast<uint32>(source.source.mips.Size());
        });
    if (!opened) {
        error = opened.Error();
    }

    if (error != ErrorCode::None) {
        Fail(texture, entry, error, issue);
    }
}

void TextureRenderCache::Fail(AssetId texture, Entry& entry, ErrorCode error,
                              render::CookedTextureIssue issue) noexcept {
    RetireHandle(texture, entry);
    entry.info.state = TextureRenderState::Failed;
    entry.info.error = error;
    entry.info.issue = issue;
    Log::Warning(LogTextureRenderCache, "Texture {Asset} could not be uploaded: {Error} ({Issue})",
                 texture, ErrorCodeName(error), render::CookedTextureIssueName(issue));
}

void TextureRenderCache::RetireHandle(AssetId texture, Entry& entry) noexcept {
    if (entry.info.handle.IsNull()) {
        return;
    }

    (void)handles_.Erase(HandleKey(entry.info.handle));
    if (auto retired = lane_.Retire(entry.info.handle); !retired) {
        Log::Warning(LogTextureRenderCache, "Texture {Asset} handle could not be retired: {Error}",
                     texture, ErrorCodeName(retired.Error()));
    }

    entry.info.handle = render::RenderHandle{};
}

uint32 TextureRenderCache::HandleNotices(Span<const render::RenderNotice> notices) noexcept {
    uint32 changed = 0;
    for (const render::RenderNotice& notice : notices) {
        if (notice.kind != render::RenderNoticeKind::ResourceReady &&
            notice.kind != render::RenderNoticeKind::ResourceFailed) {
            continue;
        }

        const AssetId* texture = handles_.Find(HandleKey(notice.handle));
        Entry* entry = texture != nullptr ? entries_.Find(*texture) : nullptr;
        if (entry == nullptr || !(entry->info.handle == notice.handle) ||
            entry->info.state != TextureRenderState::Uploading) {
            continue;
        }

        ++changed;
        if (notice.kind == render::RenderNoticeKind::ResourceReady) {
            entry->info.state = TextureRenderState::Ready;
            continue;
        }

        // handle은 그대로 둔다(그리면 typed 대체 — 마지막 Release가 Retire한다).
        entry->info.state = TextureRenderState::Failed;
        entry->info.error = notice.error;
        Log::Warning(LogTextureRenderCache,
                     "Texture {Asset} device texture could not be created: {Error}", *texture,
                     ErrorCodeName(notice.error));
    }

    return changed;
}

bool TextureRenderCache::Find(AssetId texture, TextureRenderInfo& out) const noexcept {
    const Entry* entry = entries_.Find(texture);
    if (entry == nullptr) {
        return false;
    }

    out = entry->info;
    return true;
}

Result<void> TextureRenderCache::WithPlatformData(AssetId texture, PlatformDataVisitor visit,
                                                  void* context) noexcept {
    const Entry* entry = entries_.Find(texture);
    if (entry == nullptr) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    if (!entry->lease.IsValid()) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    return WithLeasedTexture(manager_, entry->lease,
                             [&](const Texture& asset, CachePackageReader& reader) noexcept {
                                 visit(context, asset.platformData, &ReadCachePackageBulk, &reader);
                             });
}

void TextureRenderCache::Shutdown() noexcept {
    if (shutDown_) {
        return;
    }

    shutDown_ = true;
    // 모든 handle을 먼저 Retire하고, 그다음 항목을 지워 lease·티켓을 놓는다(RenderBase 6.5절).
    entries_.ForEach(
        [this](const AssetId& texture, Entry& entry) noexcept { RetireHandle(texture, entry); });
    entries_.Clear();
    handles_.Clear();
}

uint32 TextureRenderCache::CountIn(TextureRenderState state) const noexcept {
    uint32 count = 0;
    entries_.ForEach([state, &count](const AssetId&, const Entry& entry) noexcept {
        count += entry.info.state == state ? 1u : 0u;
    });
    return count;
}

uint32 TextureRenderCache::UsersOf(AssetId texture) const noexcept {
    const Entry* entry = entries_.Find(texture);
    return entry != nullptr ? entry->users : 0u;
}

} // namespace rocks
