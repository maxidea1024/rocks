#pragma once

#include <Compression/Compression.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Memory/SharedPtr.h>

// 압축 사전이다(Docs/Design/Compression.ko.md 11절). 작은 입력(수십 byte–수십 KiB)이 많을 때 공통
// 내용을 미리 알려 압축률을 크게 높인다. 지금은 zstd만 지원한다(LZ4·LZ4 frame·zlib·Brotli 사전은
// Unsupported).
//
// 사용 예(쿠킹: 작은 에셋 여러 개를 같은 사전으로 압축):
//   auto settings = CompressionSettings::TryMake(CompressionFormat::Zstd, 19).Value();
//   auto dictionary = CompressDictionary::TryCreate(settings, dictionaryBytes, allocator);
//   auto compressor = Compressor::TryCreate(dictionary.Value(), allocator);
//   for (auto& asset : assets) {
//       compressor.Value().TryCompressAppend(asset, packed);
//   }
//
// 사용 예(로더: 같은 사전으로 해제. 사전 byte는 패키지·매니페스트가 Digest로 식별한다):
//   auto dictionary = DecompressDictionary::TryCreate(CompressionFormat::Zstd, bytes, allocator);
//   auto decompressor = Decompressor::TryCreate(dictionary.Value(), allocator);
//   auto written = decompressor.Value().TryDecompress(entry, destination);
//
// 공통 계약:
// - 내용: 사전 byte가 zstd 사전 magic(0xEC30A437)으로 시작하면 zstd 사전 형식(엔트로피 표·반복
//   offset·내용·사전 ID)으로, 아니면 raw 내용으로 읽는다(ZSTD_dct_auto). 그래서 magic으로 시작하는
//   raw 내용은 형식 사전으로 해석되어 손상(InvalidArgument)으로 거부된다. 형식 사전의 사전 ID가
//   0이면 frame에 ID를 기록할 수 없어 InvalidArgument다(raw 사전과 구분되지 않는다).
// - 크기: MinDictionaryBytes ≤ 크기 ≤ MaxDictionaryBytes. zstd는 8byte 이하 내용을 색인하지 않아
//   효과가 없고, CDict는 사전 끝의 일부(수준에 따라 128KiB–수 MiB)만 색인하므로 큰 사전은 이득이
//   없다(실수로 큰 버퍼를 넘기는 것을 막는다).
// - 사전 byte는 복사한다(호출자 버퍼 수명과 무관). 복사본이 내용을 가져야 출력이 메모리 배치와
//   무관하다(byRef 사전은 원본이 사전 바로 뒤에 놓이면 결과 byte가 바뀐다).
// - 불변이며 여러 스레드·여러 문맥이 동시에 써도 된다. 복사·이동 가능한 공유 handle(원자 참조
//   계수)이며 사전을 쓰는 문맥·스트림·블록 설정·reader·비동기 작업이 복사본을 가져 수명을 보장한다.
// - allocator는 이 사전을 쓰는 모든 handle·문맥·reader·설정이 소멸하고 비동기 작업이 terminal이 될
//   때까지 살아 있어야 한다. 마지막 handle을 놓는 스레드(비동기 worker 포함)가 해제하므로 다른
//   스레드의 해제를 허용하는 할당기여야 한다(System·Binned·Budget·Pool·External·Debug 가능,
//   Arena·Stack 불가).
// - 빈 handle(기본 생성·이동된 뒤)의 조회는 Fatal이다. 빈 handle을 문맥 생성·압축 함수에 넘기면
//   StaleHandle이다.
// - 식별: Digest()는 사전 byte 전체의 SHA-256이다. 쿠킹 빌드 키·패키지의 사전 참조에 쓴다. Id()는
//   zstd 사전 ID(형식 사전) 또는 0(raw)이며 frame 헤더에 기록되는 값이다.
// - 오류: zstd가 아닌 형식 Unsupported, 모르는 형식 값·크기 밖·형식 사전 손상·사전 ID 0
//   InvalidArgument, 할당 실패 OutOfMemory.
namespace rocks {

namespace detail {
struct DictionaryState;
struct DictionaryAccess;
} // namespace detail

inline constexpr usize MinDictionaryBytes = 9;
inline constexpr usize MaxDictionaryBytes = usize{32} << 20;

// 압축용 사전(zstd CDict)이다. 압축 수준이 사전에 고정된다(settings의 형식·수준). 결과는 zstd 표준
// 경로(ZSTD_createCDict(L) + refCDict)와 같은 매개변수를 고른다: 작은 입력은 사전 준비 표를 쓰고,
// 큰 입력(128KiB 이상이고 사전의 6배 이상)은 그 수준의 전체 표로 사전을 다시 적재한다.
class CompressDictionary {
public:
    CompressDictionary() noexcept = default;

    static Result<CompressDictionary> TryCreate(const CompressionSettings& settings,
                                                Span<const uint8> bytes,
                                                Allocator& allocator) noexcept;

    bool IsValid() const noexcept {
        return static_cast<bool>(state_);
    }

    CompressionSettings Settings() const noexcept;
    uint32 Id() const noexcept;
    const Sha256Digest& Digest() const noexcept;
    usize Size() const noexcept;

    // 준비된 사전(CDict)과 상태가 차지하는 byte 수다(사전 byte 복사본 포함).
    usize MemoryBytes() const noexcept;

private:
    friend struct detail::DictionaryAccess;

    SharedPtr<detail::DictionaryState> state_;
};

// 해제용 사전(zstd DDict)이다. 계약은 CompressDictionary와 같다.
// - 사전 ID가 있는(형식) 사전으로 만든 해제 문맥은 사전 ID가 다른 frame과 사전 ID가 없는 frame을
//   Unsupported로 거부한다(zstd는 ID 없는 frame을 검사 없이 사전으로 풀어 틀린 결과를 낼 수 있다).
// - raw 사전(ID 0)은 frame에서 사전 불일치를 알 수 없다. checksum이 있는 frame이면 checksum 불일치
//   (InvalidArgument)로 걸린다.
class DecompressDictionary {
public:
    DecompressDictionary() noexcept = default;

    static Result<DecompressDictionary> TryCreate(CompressionFormat format, Span<const uint8> bytes,
                                                  Allocator& allocator) noexcept;

    bool IsValid() const noexcept {
        return static_cast<bool>(state_);
    }

    CompressionFormat Format() const noexcept;
    uint32 Id() const noexcept;
    const Sha256Digest& Digest() const noexcept;
    usize Size() const noexcept;
    usize MemoryBytes() const noexcept;

private:
    friend struct detail::DictionaryAccess;

    SharedPtr<detail::DictionaryState> state_;
};

// zstd frame 헤더에 기록된 사전 ID를 읽는다(0은 기록 없음). 로더·수신 측이 맞는 사전을 고를 때
// 쓴다. frame이 zstd frame이 아니거나 헤더가 불완전·손상이면 InvalidArgument, window가 구현
// 상한(2^31)을 넘으면 해제와 같이 Unsupported다. skippable frame은 0이다.
Result<uint32> TryGetZstdFrameDictionaryId(Span<const uint8> frame) noexcept;

} // namespace rocks
