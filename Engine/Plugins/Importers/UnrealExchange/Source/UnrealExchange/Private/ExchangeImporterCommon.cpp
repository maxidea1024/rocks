#include "ExchangeImporterCommon.h"

#include <Core/Text/StringBuilder.h>
#include <UnrealExchange/UnrealExchangeFormat.h>

// 교환 A2f 임포터 공용 검사 단계다(Docs/Design/UnrealExchange 10.6절 5–7번).

namespace rocks::exchange_detail {

Result<void> FailImport(AssetImportContext& context, AssetDiagnosticCode code, ErrorCode error,
                        uint64 detail) noexcept {
    if (error == ErrorCode::OutOfMemory) {
        return Result<void>::Failure(error);
    }

    return context.Fail(code, error, detail);
}

Result<void> ReadImportDocument(AssetImportContext& context, ExchangeClass expected,
                                ExchangeAsset& asset,
                                const UnrealExchangeImportOptions*& options) noexcept {
    // 1. 문서(고정 역할 asset — 문맥이 정확히 하나를 준다).
    const AssetImportSource* document = nullptr;
    for (const AssetImportSource& source : context.Sources()) {
        document = source.role == UnrealExchangeAssetRole ? &source : document;
    }

    if (document == nullptr) {
        return context.Fail(AssetDiagnosticCode::MissingSource, ErrorCode::NotFound);
    }

    ExchangeDiagnostics diagnostics(asset.blobs.GetAllocator());
    if (auto read = ReadExchangeAssetDocument(document->bytes, asset.blobs.GetAllocator(), asset,
                                              diagnostics);
        !read) {
        // 마지막 error 진단의 코드다(플러그인 error 진단은 InvalidSource — 없으면 CorruptBundle).
        AssetDiagnosticCode code = AssetDiagnosticCode::CorruptBundle;
        for (const ExchangeDiagnostic& item : diagnostics.Items()) {
            code = item.severity == ExchangeSeverity::Error ? item.code : code;
        }

        return FailImport(context, code, read.Error());
    }

    // 2·3. 클래스와 옵션 objectPath.
    if (asset.exchangeClass != expected) {
        return context.Fail(AssetDiagnosticCode::InvalidSource, ErrorCode::InvalidArgument);
    }

    options = static_cast<const UnrealExchangeImportOptions*>(context.Options());
    if (options == nullptr || options->objectPath.View() != asset.objectPath) {
        return context.Fail(AssetDiagnosticCode::InvalidImportOption, ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

Result<void> CollectImportBlobs(AssetImportContext& context, const ExchangeAsset& asset,
                                Array<Span<const uint8>>& out) noexcept {
    const Span<const AssetImportSource> sources = context.Sources();
    Allocator& allocator = out.GetAllocator();
    Array<uint8> used(allocator);
    if (auto resized = used.TryResize(sources.Size()); !resized) {
        return resized;
    }

    if (auto resized = out.TryResize(asset.blobs.Size()); !resized) {
        return resized;
    }

    for (usize index = 0; index < asset.blobs.Size(); ++index) {
        char hex[Sha256Digest::HexBytes + 1];
        asset.blobs[index].digest.ToHex(hex);
        StringBuilder<UnrealExchangeBlobRolePrefix.ByteCount() + Sha256Digest::HexBytes + 1> role(
            allocator);
        if (auto built = role.TryAppend(UnrealExchangeBlobRolePrefix); !built) {
            return built;
        }

        if (auto built = role.TryAppend(StringView(hex, Sha256Digest::HexBytes)); !built) {
            return built;
        }

        bool found = false;
        for (usize source = 0; source < sources.Size() && !found; ++source) {
            if (sources[source].role == role.View()) {
                out[index] = sources[source].bytes;
                used[source] = 1;
                found = true;
            }
        }

        if (!found) {
            return context.Fail(AssetDiagnosticCode::MissingSource, ErrorCode::NotFound, index);
        }
    }

    for (usize source = 0; source < sources.Size(); ++source) {
        const bool blobRole = sources[source].role.StartsWith(UnrealExchangeBlobRolePrefix,
                                                              StringComparison::CaseSensitive);
        if (used[source] == 0 && blobRole) {
            return context.Fail(AssetDiagnosticCode::InvalidImportOption,
                                ErrorCode::InvalidArgument, source);
        }
    }

    for (usize index = 0; index < asset.blobs.Size(); ++index) {
        if (auto verified = VerifyExchangeBlob(asset.blobs[index], out[index]); !verified) {
            return context.Fail(AssetDiagnosticCode::CorruptBundle, verified.Error(), index);
        }
    }

    return Result<void>::Success();
}

Result<void> AddExchangeTags(AssetImportContext& context,
                             const UnrealExchangeImportOptions& options) noexcept {
    const StringView objectPath = options.objectPath.View();
    if (auto added = context.AddTag(UnrealExchangeObjectPathTag, objectPath); !added) {
        return added;
    }

    for (const String& former : options.formerPaths) {
        if (former.View() == objectPath) {
            continue;
        }

        if (auto added = context.AddTag(UnrealExchangeFormerPathsTag, former.View()); !added) {
            return added;
        }
    }

    for (const ImportTag& tag : context.PreviousTags()) {
        // 지금 objectPath는 옛 경로가 아니다(이름을 되돌린 경우 — 10.6절 7번).
        const bool kept = tag.key.View() == UnrealExchangeFormerPathsTag ||
                          tag.key.View() == UnrealExchangeObjectPathTag;
        if (kept && tag.value.View() != objectPath) {
            if (auto added = context.AddTag(UnrealExchangeFormerPathsTag, tag.value.View());
                !added) {
                return added;
            }
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::exchange_detail
