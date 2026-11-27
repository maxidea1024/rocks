#include "ExchangeTestSupport.h"

#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// 에셋 JSON byte 읽기·blob byte 검사·blob 경로 시험이다(Docs/Design/UnrealExchange 10.6절 5번 —
// A2f 임포터가 원본을 다시 볼 때). 묶음 리더와 같은 검사는 기존 묶음 시험이 그대로 통과하는 것으로
// 보고, 여기서는 byte 입력의 차이(문서에서 채우는 objectPath·class, bundle.json 대조 없음, 실패
// 상태)를 본다.
using namespace rocks;
using namespace rocks::exchange_test;
namespace exchange_document_tests_file {

namespace {

constexpr StringView ValidDocument =
    R"({"objectPath": "/Game/T/Rock.Rock", "class": "Texture2D", "ueClass": "/Script/Engine.Texture2D",
"dependencies": [{"objectPath": "/Game/T/Other.Other", "class": "Texture2D", "kind": "hard"}],
"diagnostics": [{"code": "LossyOrigin", "severity": "info", "message": "jpeg"}],
"settings": {"SRGB": true},
"data": {"source": {"pixels": {"blob": "4bf5122f344554c53bde2ebb8cd2b7e3d1600ad631c385a5d7cce23c7785459a",
"bytes": 1, "layout": "u8", "components": 1, "count": 1}}}})";

Span<const uint8> Bytes(StringView text) noexcept {
    return Span<const uint8>(reinterpret_cast<const uint8*>(text.Data()), text.ByteCount());
}

// 거부 하나의 기대다.
struct RejectCase {
    StringView text;
    ExchangeIssue issue;
    ErrorCode error;
    AssetDiagnosticCode code = AssetDiagnosticCode::CorruptBundle;
};

} // namespace

// 정상 문서: Ready, objectPath·class·교환 클래스는 문서 값, 의존 target은 없음, blob 참조 수집,
// settings·data 객체. 모르는 클래스는 Unsupported로 채우고 성공한다(클래스 대조는 임포터가 한다).
TEST(UnrealExchange_DocumentFromBytes) {
    SystemAllocator allocator;
    {
        ExchangeAsset asset(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadExchangeAssetDocument(Bytes(ValidDocument), allocator, asset, diagnostics));
        CHECK(asset.state == ExchangeAssetState::Ready && asset.objectPath == "/Game/T/Rock.Rock" &&
              asset.className == "Texture2D" && asset.exchangeClass == ExchangeClass::Texture2D &&
              asset.ueClass == "/Script/Engine.Texture2D" && asset.file.IsEmpty());
        REQUIRE(asset.dependencies.Size() == 1);
        CHECK(asset.dependencies[0].target == ExchangeNoIndex &&
              asset.dependencies[0].targetClass == ExchangeClass::Texture2D);
        CHECK(asset.blobs.Size() == 1 && asset.blobs[0].bytes == 1 &&
              asset.settings.Find("SRGB").GetBool() && asset.data.IsObject());
        CHECK(diagnostics.Items().IsEmpty());

        // 다시 읽으면 앞 결과를 버리고 새 문서를 쓴다(모르는 클래스).
        constexpr StringView Unknown =
            R"({"objectPath": "/Game/T/Map.Map", "class": "World", "ueClass": "/Script/Engine.World",
"dependencies": [], "diagnostics": [], "settings": {}, "data": {}})";
        REQUIRE(ReadExchangeAssetDocument(Bytes(Unknown), allocator, asset, diagnostics));
        CHECK(asset.state == ExchangeAssetState::Ready &&
              asset.exchangeClass == ExchangeClass::Unsupported && asset.className == "World" &&
              asset.dependencies.IsEmpty() && asset.blobs.IsEmpty());

        // 모르는 키는 경고만 내고 읽는다.
        constexpr StringView Extra =
            R"({"objectPath": "/Game/T/Rock.Rock", "class": "Texture2D", "ueClass": "/Script/Engine.Texture2D",
"dependencies": [], "diagnostics": [], "settings": {}, "data": {}, "note": 1})";
        REQUIRE(ReadExchangeAssetDocument(Bytes(Extra), allocator, asset, diagnostics));
        CHECK(diagnostics.Items().Size() == 1 &&
              diagnostics.Items()[0].issue == ExchangeIssue::UnknownKey &&
              diagnostics.Items()[0].asset == ExchangeNoIndex &&
              diagnostics.Items()[0].file == ExchangeFile::AssetJson);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 거부: 구문·BOM·루트 종류·필수 키·objectPath 문법·의존 형식·플러그인 error 진단(InvalidSource)·
// settings 종류·blob 참조 형식(그 밖은 CorruptBundle). 실패하면 out은 Failed·빈 값이고 진단은 asset
// 번호 없이 AssetJson 파일이다.
TEST(UnrealExchange_DocumentFromBytesRejects) {
    SystemAllocator allocator;
    {
        const RejectCase cases[] = {
            {"{\"objectPath\": ", ExchangeIssue::JsonSyntax, ErrorCode::CorruptData},
            {"\xEF\xBB\xBF{}", ExchangeIssue::ByteOrderMark, ErrorCode::CorruptData},
            {"[]", ExchangeIssue::WrongType, ErrorCode::CorruptData},
            {"{\"class\": \"Texture2D\"}", ExchangeIssue::MissingKey, ErrorCode::CorruptData},
            {R"({"objectPath": "Rock", "class": "Texture2D"})", ExchangeIssue::ObjectPathSyntax,
             ErrorCode::CorruptData},
            {R"({"objectPath": "/Game/T/Rock.Rock", "class": 3})", ExchangeIssue::WrongType,
             ErrorCode::CorruptData},
            {R"({"objectPath": "/Game/T/Rock.Rock", "class": "Texture2D", "ueClass": "/Script/Engine.Texture2D",
"dependencies": [{"objectPath": "/Game/T/A.A", "class": "Texture2D", "kind": "weak"}], "diagnostics": [],
"settings": {}, "data": {}})",
             ExchangeIssue::InvalidValue, ErrorCode::CorruptData},
            {R"({"objectPath": "/Game/T/Rock.Rock", "class": "Texture2D", "ueClass": "/Script/Engine.Texture2D",
"dependencies": [], "diagnostics": [{"code": "NoSourceData", "severity": "error", "message": "x"}],
"settings": {}, "data": {}})",
             ExchangeIssue::ExporterError, ErrorCode::InvalidArgument,
             AssetDiagnosticCode::InvalidSource},
            {R"({"objectPath": "/Game/T/Rock.Rock", "class": "Texture2D", "ueClass": "/Script/Engine.Texture2D",
"dependencies": [], "diagnostics": [], "settings": [], "data": {}})",
             ExchangeIssue::WrongType, ErrorCode::CorruptData},
            {R"({"objectPath": "/Game/T/Rock.Rock", "class": "Texture2D", "ueClass": "/Script/Engine.Texture2D",
"dependencies": [], "diagnostics": [], "settings": {}, "data": {"p": {"blob": "XY", "bytes": 1,
"layout": "u8", "components": 1, "count": 1}}})",
             ExchangeIssue::InvalidValue, ErrorCode::CorruptData},
        };
        for (const RejectCase& item : cases) {
            // 앞서 읽은 문서가 있어도 실패하면 비운다.
            ExchangeAsset asset(allocator);
            ExchangeDiagnostics prefill(allocator);
            REQUIRE(ReadExchangeAssetDocument(Bytes(ValidDocument), allocator, asset, prefill));
            ExchangeDiagnostics diagnostics(allocator);
            auto read = ReadExchangeAssetDocument(Bytes(item.text), allocator, asset, diagnostics);
            CHECK(!read && read.Error() == item.error);
            CHECK(asset.state == ExchangeAssetState::Failed && asset.objectPath.IsEmpty() &&
                  asset.className.IsEmpty() && asset.exchangeClass == ExchangeClass::Unsupported &&
                  asset.dependencies.IsEmpty() && asset.blobs.IsEmpty() &&
                  asset.document.Get() == nullptr);
            REQUIRE(!diagnostics.Items().IsEmpty());
            const ExchangeDiagnostic& last = diagnostics.Items()[diagnostics.Items().Size() - 1];
            CHECK(last.issue == item.issue && last.asset == ExchangeNoIndex &&
                  last.file == ExchangeFile::AssetJson && last.code == item.code);
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 크기 상한(UnrealExchangeMaxJsonBytes 초과 — 파싱 전에 거부, LimitExceeded / CapacityExceeded).
TEST(UnrealExchange_DocumentFromBytesTooLarge) {
    SystemAllocator allocator;
    {
        Array<uint8> large(allocator);
        REQUIRE(large.TryResize(static_cast<usize>(UnrealExchangeMaxJsonBytes) + 1));
        ExchangeAsset asset(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        CHECK(ReadExchangeAssetDocument(large.View(), allocator, asset, diagnostics).Error() ==
              ErrorCode::CapacityExceeded);
        REQUIRE(diagnostics.Items().Size() == 1);
        CHECK(diagnostics.Items()[0].issue == ExchangeIssue::LimitExceeded &&
              asset.state == ExchangeAssetState::Failed);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 할당 실패: n번째 할당마다 OutOfMemory이고(진단 없이) out은 빈 값이다(문서를 가리키는 view가 남지
// 않는다). 실패가 닿지 않으면 성공한다.
TEST(UnrealExchange_DocumentFromBytesAllocationFailure) {
    SystemAllocator allocator;
    {
        test::NFailAllocator failing(allocator);
        bool succeeded = false;
        for (uint64 attempt = 1; !succeeded; ++attempt) {
            ExchangeAsset asset(failing);
            ExchangeDiagnostics diagnostics(allocator);
            failing.FailOnAttempt(attempt);
            auto read =
                ReadExchangeAssetDocument(Bytes(ValidDocument), failing, asset, diagnostics);
            failing.DisableFailure();
            if (read) {
                succeeded = true;
                CHECK(asset.state == ExchangeAssetState::Ready);
                continue;
            }

            CHECK(read.Error() == ErrorCode::OutOfMemory && diagnostics.Items().IsEmpty());
            CHECK(asset.state == ExchangeAssetState::Failed && asset.objectPath.IsEmpty() &&
                  asset.className.IsEmpty() && asset.exchangeClass == ExchangeClass::Unsupported &&
                  asset.document.Get() == nullptr);
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// blob byte 검사: 정상, 크기·해시·가변 배치 실패(세부 규칙), issue null.
TEST(UnrealExchange_VerifyBlob) {
    const uint8 raw[] = {1, 2, 3, 4};
    ExchangeBlobRef blob;
    blob.digest = Sha256::Hash(Span<const uint8>(raw));
    blob.bytes = sizeof(raw);
    blob.count = sizeof(raw);
    blob.components = 1;
    blob.layout = ExchangeBlobLayout::U8;
    ExchangeIssue issue = ExchangeIssue::None;
    CHECK(VerifyExchangeBlob(blob, Span<const uint8>(raw), &issue) && issue == ExchangeIssue::None);

    CHECK(VerifyExchangeBlob(blob, Span<const uint8>(raw, 3), &issue).Error() ==
              ErrorCode::CorruptData &&
          issue == ExchangeIssue::BlobSize);

    const uint8 changed[] = {1, 2, 3, 5};
    CHECK(!VerifyExchangeBlob(blob, Span<const uint8>(changed), &issue) &&
          issue == ExchangeIssue::BlobHash);
    CHECK(!VerifyExchangeBlob(blob, Span<const uint8>(changed)));

    // names 배치: 길이 word(4) 하나가 남은 byte보다 길다.
    const uint8 names[] = {9, 0, 0, 0};
    ExchangeBlobRef nameBlob = blob;
    nameBlob.digest = Sha256::Hash(Span<const uint8>(names));
    nameBlob.layout = ExchangeBlobLayout::Names;
    nameBlob.count = 1;
    CHECK(!VerifyExchangeBlob(nameBlob, Span<const uint8>(names), &issue) &&
          issue == ExchangeIssue::BlobLayout);
}

// blob 경로: `<directory>/blobs/<앞 2자>/<64자>.bin`, 끝 구분자가 있는 디렉터리.
TEST(UnrealExchange_BlobPath) {
    SystemAllocator allocator;
    {
        const uint8 raw[] = {7};
        const Sha256Digest digest = Sha256::Hash(Span<const uint8>(raw));
        char hex[Sha256Digest::HexBytes + 1];
        digest.ToHex(hex);
        StringBuilder<256> expected(allocator);
        REQUIRE(expected.TryAppend("D:/Out/A.rxbundle/blobs/") &&
                expected.TryAppend(StringView(hex, 2)) && expected.TryAppendAscii('/') &&
                expected.TryAppend(StringView(hex, Sha256Digest::HexBytes)) &&
                expected.TryAppend(".bin"));
        StringBuilder<256> path(allocator);
        REQUIRE(AppendExchangeBlobPath("D:/Out/A.rxbundle", digest, path));
        CHECK(path.View() == expected.View());
        StringBuilder<256> slashed(allocator);
        REQUIRE(AppendExchangeBlobPath("D:/Out/A.rxbundle/", digest, slashed));
        CHECK(slashed.View() == expected.View());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace exchange_document_tests_file
