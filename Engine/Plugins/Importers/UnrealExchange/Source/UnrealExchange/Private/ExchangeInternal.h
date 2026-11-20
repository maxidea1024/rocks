#pragma once

#include <UnrealExchange/ExchangeBundle.h>

// UnrealExchange 모듈 안에서만 쓰는 묶음 접근과 blob 검사다(Docs/Design/UnrealExchange.ko.md 10.4절
// 11·12번).

namespace rocks {

// ExchangeBundle의 private 상태 접근(리더·blob 읽기)이다.
struct ExchangeBundleAccess {
    static platform::FileSystem* Files(const ExchangeBundle& bundle) noexcept {
        return bundle.files_;
    }

    static void SetFiles(ExchangeBundle& bundle, platform::FileSystem* files) noexcept {
        bundle.files_ = files;
    }

    static String& Directory(ExchangeBundle& bundle) noexcept {
        return bundle.directory_;
    }

    static UniquePtr<JsonDocument>& Manifest(ExchangeBundle& bundle) noexcept {
        return bundle.manifest_;
    }

    static Array<ExchangeAsset>& Assets(ExchangeBundle& bundle) noexcept {
        return bundle.assets_;
    }

    static Array<ExchangeRedirect>& Redirects(ExchangeBundle& bundle) noexcept {
        return bundle.redirects_;
    }

    static Allocator& GetAllocator(const ExchangeBundle& bundle) noexcept {
        return *bundle.allocator_;
    }
};

namespace exchange_detail {

// blob 검사 실패 이유다(ReadFailed·BlobSize·BlobHash·BlobLayout과 원본 오류).
struct BlobCheckFailure {
    ExchangeIssue issue = ExchangeIssue::None;
    ErrorCode error = ErrorCode::None;
};

// directory 아래 relative를 `/`로 이어 out 뒤에 붙인다(directory 끝의 `/`·`\`는 겹치지 않는다).
Result<void> AppendBundlePath(StringView directory, StringView relative,
                              StringBuilderBase& out) noexcept;

// blob의 묶음 안 상대 경로 `blobs/<앞 2자>/<64자>.bin`을 out 뒤에 붙인다.
Result<void> AppendBlobRelativePath(const Sha256Digest& digest, StringBuilderBase& out) noexcept;

// blob 파일 하나의 크기·SHA-256·가변 배치를 확인한다. keep이 있으면 파일 내용을 거기 읽고(먼저
// 비운다), 없으면 UnrealExchangeBlobChunkBytes 조각으로 흘려 읽는다. 반환: 통과 true, 검사 실패
// false(failure에 이유), 할당 실패는 Failure다.
Result<bool> CheckBlobFile(platform::FileSystem& files, StringView directory,
                           const ExchangeBlobRef& blob, Allocator& allocator, Array<uint8>* keep,
                           BlobCheckFailure& failure) noexcept;

} // namespace exchange_detail

} // namespace rocks
