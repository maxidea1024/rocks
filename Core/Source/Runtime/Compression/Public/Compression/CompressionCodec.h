#pragma once

#include <Compression/Compression.h>
#include <Core/Text/StringView.h>

// 사용자 압축 형식(플러그인) 등록이다. UE 5.6 ICompressionFormat·IModularFeatures
// ("CompressionFormat")에 대응한다. 설계·근거는 Docs/Design/Compression.ko.md 14절을 따른다.
//
// 사용 예(쿠커 플러그인이 외부 코덱을 등록하고 내장 형식과 같은 API로 쓴다):
//   class KrakenCodec final : public CompressionCodec { ... };
//   KrakenCodec kraken; // 해제가 성공할 때까지 살아 있어야 한다
//   auto format = TryRegisterCompressionCodec({"oodle.kraken", 3, {1, 9, 4}}, kraken);
//   auto settings = CompressionSettings::TryDefault(format.Value()).Value();
//   auto packed = TryCompressAppend(settings, original, output);
//   auto info = TryGetCompressionFormatInfo(format.Value()); // 캐시 키: 이름·revision
//   ...
//   // 모듈 언로드 전: 이 형식을 쓰는 문맥·비동기 작업을 모두 끝낸 뒤 해제한다.
//   // Busy면 남은 사용이 끝난 뒤 다시 부른다.
//   auto removed = TryUnregisterCompressionCodec(format.Value());
//
// 지원 범위:
// - 일회성 압축·해제(TryCompress·TryCompressAppend·TryDecompress·TryDecompressAppend),
//   Compressor·Decompressor, 비동기 제출(AsyncCompression.h), 수준 범위·
//   CompressionSettings·TryGetCompressBound.
// - 스트림(CompressStream·DecompressStream)과 사전(CompressionDictionary.h)은 Unsupported,
//   블록 컨테이너(CompressedBlocks.h, codec ID가 파일에 남는 영속 값)는 InvalidArgument다.
// - 크기를 모르는 해제(TryDecompressAppend)는 모듈이 출력을 늘리며 처음부터 다시 해제한다.
//   첫 크기는 64KiB와 입력 × 4 중 큰 값이고(입력 × 4가 상한을 넘으면 상한), 코덱이
//   CapacityExceeded를 보고할 때마다 두 배로 늘린다. 상한까지 늘려도 부족하면
//   CapacityExceeded, 늘리다 할당이 실패하면 OutOfMemory다. 손상 입력도 상한까지 메모리를 쓸
//   수 있으므로 상한은 현실적인 값으로 준다. 원본 크기를 아는 상위 포맷은 TryDecompress를 쓴다.
// - 코덱 호출 하나는 취소 확인 없이 끝까지 실행한다(Lz4와 같다). 비동기 작업은 코덱 호출
//   전마다(재시도 사이 포함) 취소를 확인한다.
// - 등록·해제·조회 함수는 어느 스레드에서나(코덱 안에서도) 부를 수 있다. 표 잠금은 짧게만
//   쥐고, 코덱을 부르는 동안에는 쥐지 않는다.
namespace rocks {

// 사용자 압축 형식의 구현이다. 등록한 뒤에는 여러 스레드가 동시에 부르므로 상태가 없어야
// 한다. 필요한 작업 메모리는 호출마다 scratch에서 받아 반환 전에 모두 돌려준다. 코덱 안에서
// 이 모듈의 함수(다른 형식 압축 등)를 불러도 된다.
//
// 모듈이 호출 전에 보장하는 것: source와 destination이 겹치지 않는다, 빈 Span도 Data()가
// nullptr가 아니다, 압축 수준이 등록한 범위 안이다, 해제 입력이 비지 않았다. 출력 byte의
// 결정성(같은 입력·수준 → 같은 byte)은 코덱 책임이며, 출력 byte가 바뀌는 변경마다 등록
// version을 올린다.
//
// 반환 계약(어기면 호출자에게 DependencyFailed로 보고한다):
// - 성공 반환값은 destination 크기 이하다.
// - TryGetCompressBound 오류는 InvalidArgument·Overflow만 쓴다.
// - TryCompress는 destination이 bound 이상이면 CapacityExceeded로 실패하지 않는다. 오류는
//   InvalidArgument·CapacityExceeded·OutOfMemory·Overflow·DependencyFailed만 쓴다.
// - TryDecompress는 결과가 destination보다 크면 반드시 CapacityExceeded로 실패한다(모듈의
//   두 배 재시도가 이 오류에 기댄다). 오류는 위 다섯 가지와 Unsupported만 쓴다.
class CompressionCodec {
public:
    virtual ~CompressionCodec() = default;

    // sourceBytes를 압축한 결과의 최악 크기다.
    virtual Result<usize> TryGetCompressBound(usize sourceBytes) const noexcept = 0;

    // source를 level로 압축해 destination 앞부분에 쓰고 쓴 byte 수를 반환한다.
    virtual Result<usize> TryCompress(int32 level, Span<const uint8> source,
                                      Span<uint8> destination,
                                      Allocator& scratch) const noexcept = 0;

    // source를 해제해 destination 앞부분에 쓰고 쓴 byte 수를 반환한다. 손상·절단은
    // InvalidArgument다.
    virtual Result<usize> TryDecompress(Span<const uint8> source, Span<uint8> destination,
                                        Allocator& scratch) const noexcept = 0;

protected:
    CompressionCodec() noexcept = default;
    CompressionCodec(const CompressionCodec&) noexcept = default;
    CompressionCodec& operator=(const CompressionCodec&) noexcept = default;
};

// 등록 설명이다.
// - name: 형식 이름. 소문자 ASCII [a-z0-9_-]로 된 비지 않은 조각을 '.'로 둘 이상 이은
//   1–32byte다(예 "oodle.kraken", "studio.rle-v2"). 내장 형식 이름(lz4·zstd·deflate·zlib·
//   gzip·lz4frame·brotli)에는 '.'가 없어 겹치지 않는다. 등록이 복사하므로 호출 뒤 버려도 된다.
// - version: 출력 byte를 바꾸는 변경마다 올린다(TryGetCompressionFormatInfo의 revision,
//   빌드 캐시 키).
// - levels: 수준 범위. minimum ≤ defaultLevel ≤ maximum이어야 한다.
struct CompressionCodecDescription {
    StringView name;
    uint32 version = 0;
    CompressionLevelRange levels;
};

// 동시에 등록할 수 있는 코덱 수다(해제 중인 것 포함).
inline constexpr uint32 MaxCompressionCodecs = 16;

// codec을 등록하고 이 프로세스에서 쓸 형식 값을 돌려준다. 값은 256(0x100)부터 등록 순서대로
// 커지며 해제한 값을 다시 쓰지 않으므로 실행마다 달라질 수 있다(파일·빌드 캐시에는 이름을
// 쓴다). codec은 해제가 성공할 때까지 살아 있어야 한다.
// 오류: 이름 규칙·수준 범위 위반 InvalidArgument, 같은 이름(해제 중 포함) AlreadyExists, 표가
// 가득 찼거나(해제 중 포함) 형식 값을 다 썼으면 CapacityExceeded.
Result<CompressionFormat>
TryRegisterCompressionCodec(const CompressionCodecDescription& description,
                            const CompressionCodec& codec) noexcept;

// 등록을 해제한다.
// - 처음 부른 순간부터 그 값의 새 사용을 막는다. 일회성 호출·Compressor·Decompressor 생성·
//   비동기 제출·TryGetCompressionLevelRange·CompressionSettings 생성·TryGetCompressBound·
//   TryGetCompressionFormatInfo는 InvalidArgument, TryFindCompressionFormat은 NotFound다.
// - 사용권이 남아 있으면 Busy다: 진행 중인 일회성 호출, 살아 있는 Compressor·Decompressor,
//   아직 실행이 끝나지 않은 비동기 작업(대기 중 포함). 남은 사용은 끝까지 동작하고, 모두 끝난
//   뒤 다시 부르면 성공한다. 비동기 작업의 사용권은 실행이 끝나거나 실행 없이 정리될 때 놓이며
//   늦어도 terminal 게시 전이다(TryWaitResult가 결과를 돌려준 뒤에는 놓여 있다).
// - 성공한 뒤에만 codec을 파괴하거나 모듈을 언로드한다. 코덱 안에서 자기 형식을 해제하면
//   Busy다(그 호출이 사용권을 쥐고 있다).
// 오류: 내장 형식·등록되지 않은 값 InvalidArgument, 사용 중 Busy.
Result<void> TryUnregisterCompressionCodec(CompressionFormat format) noexcept;

} // namespace rocks
