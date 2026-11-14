#pragma once

#include <Compression/Compression.h>
#include <Compression/CompressionDictionary.h>
#include <Core/StreamProgress.h>

// 조각 단위 스트리밍 압축·해제다(Godot StreamPeerGZIP 대응, Docs/Design/Compression.ko.md 10·12절).
// 형식은 Zstd·Deflate·Zlib·Gzip·Lz4Frame이며 Lz4(raw block)는 스트림 형식이 아니라 Unsupported다.
// Brotli는 해제 전용이다(DecompressStream만 되고 CompressStream은 Unsupported). 사용자 등록
// 형식(CompressionCodec.h)은 스트림을 지원하지 않는다(Unsupported).
// 빠른 스트림(로그·네트워크 메시지)은 Lz4Frame, 압축률이 중요하면 Zstd를 쓴다.
//
// 사용 예(LZ4 frame으로 메시지를 보내며 메시지마다 flush):
//   auto stream = CompressStream::TryCreate(
//       CompressionSettings::TryDefault(CompressionFormat::Lz4Frame).Value(), allocator);
//   stream.Value().TryWriteAppend(message, wire);   // 64KiB 블록이 차면 압축해 붙인다
//   stream.Value().TryFlushAppend(wire);            // 여기까지 받은 쪽이 모두 해제할 수 있다
//   stream.Value().TryFinishAppend(wire);           // endmark·content checksum
//
// 사용 예(네트워크에서 받은 gzip 조각을 해제):
//   DecompressStreamLimits limits;
//   limits.maxOutputBytes = 64 << 20;
//   auto stream = DecompressStream::TryCreate(CompressionFormat::Gzip, limits, allocator);
//   for (each received chunk) {
//       auto appended = stream.Value().TryWriteAppend(chunk, body);   // body 뒤에 붙인다
//   }
//   auto finished = stream.Value().TryFinish();   // 절단·체크섬 미확인이면 InvalidArgument
//
// 공통 계약:
// - TryProcess는 역압이 있는 기본 API다. 출력이 모자라면 오류가 아니라 입력 일부만
//   소비하고 돌아온다(StreamProgress.consumed·produced). 작은 입력 하나가 큰 출력을
//   한꺼번에 만들지 않는다.
// - Append 편의 함수는 입력을 모두 소비하고 output 뒤에 붙인다. 출력은 1MiB 단위로
//   늘린다(0 채우기 없음). 해제 Append는 output 길이를 호출 전 길이 + 남은 상한보다 길게
//   늘리지 않는다(capacity는 Array의 성장 규칙을 따르므로 그보다 클 수 있다).
// - 실패: 코덱 실패(손상 InvalidArgument, 사전이 필요한 입력 Unsupported, OutOfMemory,
//   DependencyFailed), 출력 성장 실패(OutOfMemory·Overflow), 출력 상한 초과(CapacityExceeded),
//   진행하지 않는 코덱(DependencyFailed)은 Reset 전까지 같은 오류를 유지한다(코덱 상태가 이미
//   입력을 소비했다). 코덱을 건드리기 전의 거부(입력·출력 겹침, 지금 단계에서 받지 않는 요청은
//   InvalidArgument)는 상태를 바꾸지 않는다. Append 편의 함수는 실패한 호출에서 붙인 길이를
//   되돌린다(일관성일 뿐 무결성 보장은 아니다). 실패한 TryProcess의 output 내용은 미정이다.
// - 출력 공간이 0인 호출은 코덱을 부르지 않고 {0, 0, false}다. 단, 진행할 것이 없는 호출(빈
//   입력의 TryProcess, 끝난 스트림의 TryFinish)은 출력 공간과 무관하게 {0, 0, true}다.
// - 이동 전용이다. 코덱 상태는 heap에 고정하고 기본 생성·이동된 객체의 호출은 StaleHandle이다.
//   한 객체는 한 번에 한 스레드만 쓴다. 할당기는 객체보다 오래 살아야 한다.
namespace rocks {

namespace detail {
struct StreamState;
} // namespace detail

// 한 번의 스트림 호출 결과는 Core StreamProgress(Core/StreamProgress.h)다. Crypto 스트림과 같은
// 타입이다. complete의 뜻은 호출마다 다르다(각 함수 설명 참고).

// 압축 스트림이다. 결과 byte는 같은 호출 순서(입력 조각·출력 용량)일 때만 같다. 일회성
// API와 byte가 같다고 약속하지 않는다(해제 결과는 같다). Deflate 수준 0은 출력 용량에 따라
// stored block 경계가 바뀐다. Lz4Frame은 더 강해서 입력 내용과 TryFlush 위치만 같으면 입력 조각
// 나누기·출력 창 크기와 무관하게 같은 byte다(frame에 원본 크기를 기록하지 않는다).
// 메모리: zlib deflate 약 256KiB, zstd는 크기를 모르므로 수준별 표를 최대로 쓴다(수준 19는
// 약 90MiB), Lz4Frame은 수준 1 약 270KiB·HC 약 580KiB(블록 버퍼 192KiB와 작은 창용 64KiB 포함).
class CompressStream {
public:
    CompressStream() noexcept = default;
    ~CompressStream() noexcept;
    CompressStream(CompressStream&& other) noexcept;
    CompressStream& operator=(CompressStream&& other) noexcept;
    CompressStream(const CompressStream&) = delete;
    CompressStream& operator=(const CompressStream&) = delete;

    // sizeHint는 전체 입력 크기 추정치(0 = 모름)다. zstd가 매개변수를 고르는 데만 쓴다(frame에
    // 기록하지 않는다. Lz4Frame·zlib 계열은 쓰지 않는다). 실패: Lz4·사용자 등록 형식은 Unsupported,
    // OutOfMemory, DependencyFailed.
    static Result<CompressStream> TryCreate(const CompressionSettings& settings,
                                            Allocator& allocator, uint64 sizeHint = 0) noexcept;

    // 사전 스트림이다(CompressionDictionary.h). 형식·수준은 사전이 정하고 frame에 사전 ID(있으면)를
    // 기록한다. 원본 크기를 모르므로 사전을 붙여(attach) 쓴다. Reset 뒤에도 같은 사전을 쓴다. 빈
    // handle은 StaleHandle이다.
    static Result<CompressStream> TryCreate(const CompressDictionary& dictionary,
                                            Allocator& allocator, uint64 sizeHint = 0) noexcept;

    // input을 소비하고 만들어진 출력을 output 앞부분에 쓴다. complete는 input을 모두 소비했는지다
    // (코덱이 내보내지 않은 출력은 다음 호출이 먼저 내보낸다).
    // 실패: 미완료 flush·finish 중·끝난 스트림·겹침은 InvalidArgument, 그 밖은 공통 계약.
    Result<StreamProgress> TryProcess(Span<const uint8> input, Span<uint8> output) noexcept;
    // 지금까지 받은 입력을 해제 가능한 지점까지 내보낸다(네트워크용, 압축률이 조금 떨어진다).
    // complete가 될 때까지 출력 공간을 주며 반복한다. 출력 창은 1byte여도 된다(작은 창은 내부
    // 버퍼를 거친다). 코덱을 부른 flush가 complete 전이면 TryFlush만 받는다(다른 요청은
    // InvalidArgument, 상태는 그대로다. zlib이 미완료 flush를 같은 모드로 다시 부르기를 요구한다).
    // 출력 창 크기에 따라 빈 flush marker가 더 붙을 수 있다(해제 결과는 같다).
    // 실패: 미완료 finish 중·끝난 스트림은 InvalidArgument, 그 밖은 공통 계약.
    Result<StreamProgress> TryFlush(Span<uint8> output) noexcept;
    // 스트림을 끝낸다(남은 출력·체크섬). complete가 될 때까지 반복한다. 코덱을 부른 finish가
    // complete 전이면 TryFinish만 받는다. 끝난 뒤의 TryFinish는 {0, 0, true},
    // Process·Flush는 InvalidArgument다(Reset으로 새 스트림).
    // 실패: 미완료 flush 중은 InvalidArgument, 그 밖은 공통 계약.
    Result<StreamProgress> TryFinish(Span<uint8> output) noexcept;

    // Append 편의 함수다. 붙인 byte 수를 반환한다. 각각 TryProcess·TryFlush·TryFinish를
    // complete까지 반복한 것과 같고 오류도 같다. input과 output 저장소(capacity 전체)가 겹치면
    // InvalidArgument다.
    Result<usize> TryWriteAppend(Span<const uint8> input, Array<uint8>& output) noexcept;
    Result<usize> TryFlushAppend(Array<uint8>& output) noexcept;
    Result<usize> TryFinishAppend(Array<uint8>& output) noexcept;

    // 같은 설정으로 새 스트림을 시작한다(실패 상태도 지운다). 이동된 객체에는 아무것도 하지 않는다.
    void Reset() noexcept;

private:
    explicit CompressStream(detail::StreamState* state) noexcept : state_(state) {
    }

    static Result<CompressStream>
    TryCreateWith(CompressionFormat format, int32 level, Allocator& allocator, uint64 sizeHint,
                  const SharedPtr<detail::DictionaryState>& dictionary) noexcept;

    detail::StreamState* state_ = nullptr;
};

// DecompressStreamLimits::maxWindowLog의 허용 범위다(zstd ZSTD_WINDOWLOG_ABSOLUTEMIN–64bit MAX).
inline constexpr uint32 MinZstdWindowLog = 10;
inline constexpr uint32 MaxZstdWindowLog = 31;

// 해제 한계다. maxOutputBytes는 필수(0이면 TryCreate가 InvalidArgument)이며 Create·Reset 이후 모든
// frame·member 출력의 누계 상한이다(넘으면 CapacityExceeded, 정확히 상한이면 성공). maxWindowLog는
// zstd window 상한(작업 메모리 ≈ 2^maxWindowLog)이며 MinZstdWindowLog–MaxZstdWindowLog다. Brotli는
// 스트림 첫 byte의 WBITS(window 2^WBITS - 16, 10–24)를 이 값과 비교한다(작업 메모리 최고 약 1.5 ×
// 2^WBITS + Huffman 표 약 2.6MiB). zlib 계열은 window가 32KiB로 고정이고, Lz4Frame은 frame 헤더의
// 블록 크기(64KiB–4MiB)가 작업 메모리를 정하므로(최대 약 8.1MiB) 범위 검사만 하고 쓰지 않는다.
struct DecompressStreamLimits {
    usize maxOutputBytes = 0;
    uint32 maxWindowLog = 27;
};

// 해제 스트림이다. 해제 결과는 입력 조각 나누기와 무관하다. 출력은 frame·member 끝의 체크섬을
// 확인하기 전까지 미검증이다(검증 시점은 IsAtStreamBoundary·TryFinish 성공).
// - 스트림이 끝난 뒤의 byte: Gzip은 다음 member, Zstd·Lz4Frame은 다음 frame(skippable 포함),
//   Deflate·Zlib·Brotli는 InvalidArgument다(Brotli 스트림은 연결할 수 없다). Zstd·Lz4Frame은 frame
//   magic이 아닌 byte를 받은 호출에서 바로 거부한다.
// - zstd frame의 window(single segment frame은 원본 크기)가 2^31을 넘으면 Unsupported(구현
//   상한), 2^maxWindowLog만 넘으면 CapacityExceeded(호출자 상한)다. 사전이 필요한 zstd frame·
//   zlib FDICT·사전 ID를 기록한 LZ4 frame·LZ4 legacy frame(`lz4 -l`)은 Unsupported다. LZ4 frame
//   헤더(FLG·BD·헤더 checksum)는 사전 ID를 보기 전에 검증하므로 손상된 헤더는 InvalidArgument다.
// - Brotli: 첫 byte의 WBITS가 maxWindowLog를 넘으면 CapacityExceeded, large window 확장
//   스트림(비표준, 첫 byte 0x11)은 Unsupported다. checksum이 없어 손상 검출은 형식 오류에 한정된다.
// - 무결성: LZ4 frame은 content checksum이 있으면 frame 끝에서, 블록 checksum이 있으면 블록마다
//   확인한다. 원본 크기가 기록된 frame은 frame 끝에서 크기도 확인한다.
// - 메모리: zlib inflate 약 40KiB, zstd는 window + 블록 버퍼, Lz4Frame은 블록 버퍼(64KiB 블록 약
//   256KiB, 4MiB 블록 약 8.1MiB). 이 셋은 Reset으로 해제되지 않고 객체 수명 동안 유지한다.
//   Brotli는 ring buffer(출력에 맞춰 2배씩 늘며 최대 window 16MiB)와 metablock마다의 Huffman
//   표(최악 약 2.6MiB)를 쓰고, ring buffer를 늘리는 순간 이전 것과 함께 있어 최고 약 1.5 × window
//   + 표다. 해제기 상태를 다시 쓸 수 없어 Reset이 이전 상태를 반환하고 새로 만든다(이때
//   OutOfMemory가 날 수 있다).
class DecompressStream {
public:
    DecompressStream() noexcept = default;
    ~DecompressStream() noexcept;
    DecompressStream(DecompressStream&& other) noexcept;
    DecompressStream& operator=(DecompressStream&& other) noexcept;
    DecompressStream(const DecompressStream&) = delete;
    DecompressStream& operator=(const DecompressStream&) = delete;

    // 실패: Lz4·사용자 등록 형식은 Unsupported, maxOutputBytes 0·maxWindowLog 범위 밖·
    // 모르는 형식 InvalidArgument, OutOfMemory, DependencyFailed.
    static Result<DecompressStream> TryCreate(CompressionFormat format,
                                              const DecompressStreamLimits& limits,
                                              Allocator& allocator) noexcept;

    // 사전 스트림이다(DecompressDictionary의 frame 검사 계약). 사전 메모리(DDict)는 window 상한과
    // 별개다. 빈 handle은 StaleHandle이다.
    static Result<DecompressStream> TryCreate(const DecompressDictionary& dictionary,
                                              const DecompressStreamLimits& limits,
                                              Allocator& allocator) noexcept;

    // input을 소비하고 만들어진 출력을 output 앞부분에 쓴다. complete는 input을 모두 소비했고 코덱
    // 안에 내보낼 출력이 남지 않았는지다(아니면 출력 공간을 더 주며 다시 부른다. 빈 input도 된다).
    // 상한에 정확히 닿은 뒤에도 블록 끝·체크섬을 처리하려면 1byte 이상의 출력 창이 필요하다.
    // 실패: 손상·Deflate·Zlib·Brotli 스트림 뒤 byte·겹침은 InvalidArgument, 상한 초과는
    // CapacityExceeded(이때 output에는 상한보다 1byte 많이 써 있을 수 있다. output 범위 안이다), 그
    // 밖은 공통 계약.
    Result<StreamProgress> TryProcess(Span<const uint8> input, Span<uint8> output) noexcept;

    // input을 모두 소비하고 출력을 output 뒤에 붙인다. 붙인 byte 수를 반환한다. 오류는 TryProcess와
    // 같고, input과 output 저장소(capacity 전체)가 겹치면 InvalidArgument다.
    Result<usize> TryWriteAppend(Span<const uint8> input, Array<uint8>& output) noexcept;

    // 지금까지 받은 입력이 완결된 스트림(들)인지다(frame·member가 끝났고 그 뒤로 받은 byte가 없다).
    bool IsAtStreamBoundary() const noexcept;

    // 스트림이 경계에서 끝났는지 확인한다. 경계가 아니거나(절단) 스트림을 하나도 받지 않았으면
    // InvalidArgument다(skippable frame만 받았으면 성공). 실패 상태면 그 오류다.
    Result<void> TryFinish() const noexcept;

    // 새 스트림을 시작한다(누계·실패 상태도 지운다). Brotli는 해제기 상태를 새로 만들며, 할당에
    // 실패하면 OutOfMemory가 다음 Reset까지 실패 상태로 남는다.
    void Reset() noexcept;

private:
    explicit DecompressStream(detail::StreamState* state) noexcept : state_(state) {
    }

    static Result<DecompressStream>
    TryCreateWith(CompressionFormat format, const DecompressStreamLimits& limits,
                  Allocator& allocator,
                  const SharedPtr<detail::DictionaryState>& dictionary) noexcept;

    detail::StreamState* state_ = nullptr;
};

} // namespace rocks
