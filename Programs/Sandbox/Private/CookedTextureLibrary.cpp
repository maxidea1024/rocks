#include "CookedTextureLibrary.h"

#include <AssetRuntime/Package.generated.h>
#include <AssetRuntime/Package.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Move.h>
#include <Meta/GeneratedType.h>
#include <Object/ObjectRegistry.h>
#include <Texture/Texture.generated.h>
#include <Texture/Texture.h>

namespace sandbox {

namespace {

inline constexpr LogCategory LogSandboxTextures{"Sandbox.Textures"};

// 패키지 bulk reader 문맥이다(패키지는 Borrow 동안 산다).
struct PackageReader {
    const Package* package = nullptr;
};

// 열린 텍스처 하나다(Borrow 범위 안).
struct OpenTexture {
    const Texture* texture = nullptr;
    PackageReader reader;
};

// lease의 대표 텍스처와 패키지를 열어 fn(OpenTexture)을 부른다.
template <class Fn>
Result<void> WithOpenTexture(AssetManager& manager, const AssetLease& lease, Fn&& fn) noexcept {
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

        OpenTexture open;
        open.texture = textureValue.Value();
        open.reader.package = packageValue.Value();
        fn(open);
    });
    if (!opened) {
        return opened;
    }

    return result;
}

} // namespace

CookedTextureLibrary::CookedTextureLibrary(Allocator& allocator, AssetManager& manager,
                                           const CookedManifest& manifest,
                                           TypeRegistry& types) noexcept
    : allocator_(allocator), manager_(manager), manifest_(manifest), types_(types),
      entries_(allocator) {
}

CookedTextureLibrary::~CookedTextureLibrary() noexcept {
    Release();
}

Result<Span<const uint8>> CookedTextureLibrary::ReadPackageBulk(void* context,
                                                                const BulkDataRef& ref) noexcept {
    const auto* reader = static_cast<const PackageReader*>(context);
    return reader->package->BulkBytes(ref);
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

        AssetRequest request;
        request.id = source.asset;
        request.expectedType = textureType;
        auto ticket = manager_.RequestAsset(request);
        if (!ticket) {
            Log::Warning(LogSandboxTextures, "Texture {Path} could not be requested: {Error}",
                         entry.path.View(), ErrorCodeName(ticket.Error()));
            continue;
        }

        entry.ticket = Move(ticket.Value());
        if (auto pushed = entries_.TryPushBack(Move(entry)); !pushed) {
            return Result<uint32>::Failure(pushed.Error());
        }

        const uint32 slot = static_cast<uint32>(entries_.Size() - 1);
        auto callback = AssetReadyCallback::TryCreate(
            allocator_, [this, slot](Result<AssetLease> lease, const AssetDiagnostic&) noexcept {
                // Release 뒤의 취소·종료 통지는 무시한다(티켓을 놓으면 Cancelled가 한 번 온다).
                if (released_ || slot >= entries_.Size()) {
                    return;
                }

                Entry& ready = entries_[slot];
                if (!lease) {
                    ready.state = State::Failed;
                    ready.error = lease.Error();
                    ++failed_;
                    Log::Warning(LogSandboxTextures, "Texture {Path} could not be loaded: {Error}",
                                 ready.path.View(), ErrorCodeName(lease.Error()));
                    return;
                }

                ready.lease = Move(lease.Value());
                ready.state = State::Ready;
            });
        if (!callback) {
            return Result<uint32>::Failure(callback.Error());
        }

        if (auto registered =
                manager_.OnReady(entries_[slot].ticket, TaskLane::Game, Move(callback.Value()));
            !registered) {
            return Result<uint32>::Failure(registered.Error());
        }
    }

    Log::Info(LogSandboxTextures, "Requested {Count} cooked textures", entries_.Size());
    return Result<uint32>::Success(static_cast<uint32>(entries_.Size()));
}

void CookedTextureLibrary::Upload(Entry& entry, screen::ScreenHost& screen) noexcept {
    render::CookedTextureIssue issue = render::CookedTextureIssue::None;
    Result<uint32> added = Result<uint32>::Failure(ErrorCode::NotReady);
    auto opened = WithOpenTexture(manager_, entry.lease, [&](OpenTexture& open) noexcept {
        // UE와 같이 CPU Availability는 GPU에 쿠킹 mip을 두지 않는다(6.5절 — 대체 표시).
        const bool cpu = open.texture->availability == TextureAvailability::CPU;
        added = screen.AddTexture(entry.path.View(), open.texture->platformData, &ReadPackageBulk,
                                  &open.reader, cpu, &issue);
    });
    if (!opened || !added) {
        entry.state = State::Failed;
        entry.error = !opened ? opened.Error() : added.Error();
        ++failed_;
        Log::Warning(LogSandboxTextures, "Texture {Path} could not be uploaded: {Error} ({Issue})",
                     entry.path.View(), ErrorCodeName(!opened ? opened.Error() : added.Error()),
                     render::CookedTextureIssueName(issue));
        return;
    }

    entry.inspector = added.Value();
    entry.state = State::Uploaded;
    ++uploaded_;
}

void CookedTextureLibrary::Update(screen::ScreenHost& screen) noexcept {
    if (released_ || !screen.IsRunning()) {
        return;
    }

    for (Entry& entry : entries_) {
        if (entry.state == State::Ready) {
            Upload(entry, screen);
        }
    }
}

void CookedTextureLibrary::Release() noexcept {
    if (released_) {
        return;
    }

    released_ = true;
    // 티켓·lease 소멸은 해제 큐로 간다(다음 Pump — 어느 스레드에서나 된다).
    entries_.Clear();
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

Result<void> CookedTextureLibrary::InspectCookedData(uint32 request, CookedDataVisitor visit,
                                                     void* context) noexcept {
    if (request >= entries_.Size() || !entries_[request].lease.IsValid()) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    return WithOpenTexture(manager_, entries_[request].lease, [&](OpenTexture& open) noexcept {
        visit(context, open.texture->platformData, &ReadPackageBulk, &open.reader);
    });
}

} // namespace sandbox
