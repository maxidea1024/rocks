#pragma once

#include <Compression/Compression.h>
#include <Compression/CompressionDictionary.h>

// 블록 컨테이너 RCBK v2다(Godot FileAccessCompressed, UE FCompressedBuffer 대응,
// Docs/Design/Compression.ko.md 9절). 큰 데이터를 고정 크기 블록으로 나눠 각각 압축하고, 임의
// 위치를 필요한 블록만 해제해 읽는다. 블록이 독립이라 병렬 압축·해제가 되고, 블록마다 저장 byte의
// CRC-32로 무결성을 확인한다(체크섬이 없는 LZ4·Deflate raw도 보호한다).
//
// 사용 예(메모리 전체 압축과 임의 위치 읽기):
//   auto settings = CompressedBlocksSettings::TryMake(
//       CompressionSettings::TryDefault(CompressionFormat::Zstd).Value()).Value();
//   Array<uint8> container(allocator);
//   TryCompressBlocksAppend(settings, original, container, allocator);
//   auto reader = CompressedBlocksReader::TryOpen(container.View(), allocator);
//   auto context = CompressedBlocksReadContext::TryCreate(reader.Value(), allocator);
//   reader.Value().TryRead(offset, destination, container.View(), context.Value());
//
// 사용 예(파일: 인덱스만 먼저 읽고 필요한 블록 byte만 읽는다):
//   uint8 header[CompressedBlocksHeaderBytes];          // 파일 앞 48byte
//   auto indexBytes = CompressedBlocksReader::TryGetIndexBytes(header, fileSize);
//   ...                                                 // 파일 앞 indexBytes를 index로 읽는다
//   auto reader = CompressedBlocksReader::TryOpen(index, fileSize, allocator);
//   auto where = reader.Value().TryLocate(offset, destination.Size());
//   ...                                    // [storedOffset, +storedBytes)를 stored로 읽는다
//   reader.Value().TryRead(offset, destination, stored, where.Value().storedOffset,
//                          context.Value());
//
// 사용 예(사전: 작은 블록이 많을 때 압축률을 높인다. 쓰는 쪽과 읽는 쪽이 같은 사전 byte를 쓴다):
//   auto settings = CompressedBlocksSettings::TryMake(compressDictionary, 14).Value();
//   ...
//   DecompressDictionary candidates[] = {decompressDictionary};
//   auto reader = CompressedBlocksReader::TryOpen(container.View(), candidates, allocator);
//
// 형식(모든 정수 little-endian):
// - 헤더 48byte: magic "RCBK", version 2(u16), codec ID(u8: 1 Lz4, 2 Zstd, 3 Deflate),
//   blockSizeLog2(u8, 12–24), rawSize(u64), blockCount(u32), flags(u32: bit0 = 사전),
//   payloadSize(u64), dictionaryFingerprint(8byte: 사전 SHA-256 앞 8byte, 사전 없으면 0),
//   reserved(u32, 0), indexCrc32(u32: 헤더 앞 44byte와 블록 표의 CRC-32).
// - 블록 표: 블록마다 {storedSize u32, storedCrc32 u32}. 그 뒤 payload는 블록 저장 byte를 순서대로
//   이어 붙인다. storedSize가 블록 원본 크기와 같으면 raw 저장, 작으면 압축이다.
// - 블록 코덱 데이터: Lz4 raw block, Zstd frame(원본 크기 포함, 체크섬 없음, 형식 사전 컨테이너는
//   사전 ID 포함), Deflate raw.
//
// 오류: 형식 위반·길이·CRC 불일치·블록 해제 결과 크기 불일치·사전 없는 컨테이너의 사전 블록은
// InvalidArgument, 모르는 version·flags·codec ID와 사전 bit + Zstd 아닌 codec, 컨테이너가 쓴 사전이
// 후보에 없음은 Unsupported, 할당 실패는 OutOfMemory다.
// 보안: CRC는 우발 손상만 잡는다(다시 맞춘 의도적 변조는 거르지 못한다). rawSize는 컨테이너 길이의
// 최대 약 186만 배일 수 있으므로 RawSize()로 출력을 잡기 전에 호출자 상한과 비교한다.
// 결정성: 같은 라이브러리 버전·설정·입력이면 결과 byte가 같고 병렬 압축도 순차와 같은 byte다.
namespace rocks {

class WorkerScheduler;

inline constexpr uint32 MinCompressedBlockSizeLog2 = 12;     // 4KiB
inline constexpr uint32 MaxCompressedBlockSizeLog2 = 24;     // 16MiB
inline constexpr uint32 DefaultCompressedBlockSizeLog2 = 18; // 256KiB
inline constexpr usize CompressedBlocksHeaderBytes = 48;

// 블록 컨테이너 설정이다. 형식은 Lz4·Zstd·Deflate만 쓴다(Zlib·Gzip 래퍼와 Lz4Frame frame은
// 컨테이너와 중복이다. LZ4 블록은 Lz4 raw block으로 저장한다).
class CompressedBlocksSettings {
public:
    // 실패: Zlib·Gzip·Lz4Frame·모르는 형식, blockSizeLog2가 12–24 밖이면 InvalidArgument다.
    static Result<CompressedBlocksSettings>
    TryMake(const CompressionSettings& compression,
            uint32 blockSizeLog2 = DefaultCompressedBlockSizeLog2) noexcept;

    // 사전 블록 설정이다(Zstd만). 형식·수준은 사전이 정하고 설정이 사전 handle 복사본을 가진다.
    // 헤더에 사전 지문을 쓰고, 형식 사전이면 블록 frame에 사전 ID도 기록한다(raw 사전은 ID가 0이라
    // 지문만으로 맞춘다). 사전은 작은 블록(64KiB 이하)에서 효과가 크다. 빈 handle은 StaleHandle,
    // blockSizeLog2가 범위 밖이면 InvalidArgument다. 복사는 사전 handle을 공유한다(이동도 복사다).
    static Result<CompressedBlocksSettings>
    TryMake(const CompressDictionary& dictionary,
            uint32 blockSizeLog2 = DefaultCompressedBlockSizeLog2) noexcept;

    // 복사만 선언해 이동도 복사로 동작한다(이동된 설정이 사전 없이 조용히 다른 결과를 내지 않는다).
    CompressedBlocksSettings(const CompressedBlocksSettings&) noexcept = default;
    CompressedBlocksSettings& operator=(const CompressedBlocksSettings&) noexcept = default;

    bool UsesDictionary() const noexcept {
        return static_cast<bool>(dictionary_);
    }

    const CompressionSettings& Compression() const noexcept {
        return compression_;
    }

    uint32 BlockSizeLog2() const noexcept {
        return blockSizeLog2_;
    }

    usize BlockSize() const noexcept {
        return usize{1} << blockSizeLog2_;
    }

private:
    friend struct CompressedBlocksSettingsAccess;

    CompressedBlocksSettings(const CompressionSettings& compression, uint32 blockSizeLog2,
                             const SharedPtr<detail::DictionaryState>& dictionary) noexcept
        : compression_(compression), blockSizeLog2_(blockSizeLog2), dictionary_(dictionary) {
    }

    CompressionSettings compression_;
    uint32 blockSizeLog2_;
    SharedPtr<detail::DictionaryState> dictionary_;
};

// rawSize byte를 담은 컨테이너의 최대 크기(헤더 + 블록 표 + rawSize)다. 블록은 원본보다 커지지
// 않는다. 실패: blockSizeLog2 범위 밖·블록 수가 u32를 넘으면 InvalidArgument, 크기 overflow는
// Overflow다.
Result<usize> TryGetCompressedBlocksBound(uint32 blockSizeLog2, uint64 rawSize) noexcept;

// source 전체를 컨테이너 하나로 압축해 output 뒤에 붙이고 붙인 byte 수를 반환한다. 코덱 작업
// 메모리는 scratch에서 할당한다. 실패하면 output 길이를 되돌린다(capacity는 늘 수 있다). source와
// output 저장소가 겹치면 InvalidArgument다.
Result<usize> TryCompressBlocksAppend(const CompressedBlocksSettings& settings,
                                      Span<const uint8> source, Array<uint8>& output,
                                      Allocator& scratch) noexcept;

// 병렬 압축이다. 결과 byte는 순차와 같다.
// - 연속 블록 범위 job을 worker와 호출 스레드가 함께 가져간다. 작업 제출이 거부되거나 scheduler가
//   Shutdown(Cancel)로 body를 건너뛰어도 호출 스레드가 남은 job을 처리하므로 결과는 완성된다.
// - 반환 시점: 호출 스레드가 job을 다 처리하면 아직 시작하지 않은 helper 작업은 회수하고
//   (WorkerTaskHandle::TryRevoke), 실행 중인 helper가 가져간 job(16MiB 이하)을 마칠 때까지만
//   기다린다. 그래서 worker가 다른 일로 바쁘거나 호출 스레드가 쥔 lock을 기다려도 끝난다.
// - job 결과를 모은 뒤 output을 한 번만 늘려 인덱스와 결과를 순서대로 복사한다. 실패하면 남은 job을
//   건너뛰고 가장 앞 블록의 오류를 돌려준다. helper가 코덱 문맥을 만들지 못하면 그 helper만
//   빠진다(호출 전체를 실패시키지 않는다).
// - scratch는 여러 스레드가 동시에 할당하고 다른 스레드가 해제해도 되는 할당기여야 한다
//   (System·Binned 등, Arena·Stack 금지).
// - 메모리: 참여 스레드(helper 최대 worker 수 + 호출 스레드)마다 코덱 문맥 하나다. zstd 고수준·큰
//   블록은 문맥이 크므로(수준 19·16MiB 블록은 약 81MiB) worker 수로 조절한다.
// - 블록 4개 이하이거나 1MiB 미만인 입력은 순차로 처리한다. helper 수는 원본 512KiB당 하나 이하다.
// - 호출 스레드가 그 scheduler의 worker면 WrongThread다(자기 worker를 기다리면 교착한다).
Result<usize> TryCompressBlocksAppend(WorkerScheduler& scheduler,
                                      const CompressedBlocksSettings& settings,
                                      Span<const uint8> source, Array<uint8>& output,
                                      Allocator& scratch) noexcept;

// 증분 writer다. IO와 무관하게 블록을 하나씩 압축한다. 파일이면 인덱스 자리(IndexBytes)를 비워 두고
// payload를 흘려 쓴 뒤 되돌아가 인덱스를 쓴다.
//
// 사용 예:
//   auto writer = CompressedBlocksWriter::TryBegin(settings, rawSize, allocator);
//   file.Seek(writer.Value().IndexBytes());
//   for (each block of BlockSize bytes, the last may be shorter) {
//       payload.Clear();
//       writer.Value().TryAppendBlock(block, payload);
//       file.Write(payload);
//   }
//   writer.Value().TryFinishIndex(index);   // index는 IndexBytes byte
//   file.Seek(0); file.Write(index);
//
// 이동 전용이며 이동된 객체는 StaleHandle이다. 할당기는 객체보다 오래 살아야 한다.
class CompressedBlocksWriter {
public:
    CompressedBlocksWriter() noexcept = default;
    ~CompressedBlocksWriter() noexcept;
    CompressedBlocksWriter(CompressedBlocksWriter&& other) noexcept;
    CompressedBlocksWriter& operator=(CompressedBlocksWriter&& other) noexcept;
    CompressedBlocksWriter(const CompressedBlocksWriter&) = delete;
    CompressedBlocksWriter& operator=(const CompressedBlocksWriter&) = delete;

    // 블록 표(8byte/블록)를 allocator에서 할당한다. 코덱 문맥은 처음 압축하는 블록에서 만든다(빈
    // 원본·Deflate 수준 0은 만들지 않는다). 실패: 블록 수가 u32를 넘으면 InvalidArgument,
    // OutOfMemory.
    static Result<CompressedBlocksWriter> TryBegin(const CompressedBlocksSettings& settings,
                                                   uint64 rawSize, Allocator& allocator) noexcept;

    // 헤더 + 블록 표 byte 수(48 + 8 × 블록 수)다. 이동된 객체는 0이다.
    usize IndexBytes() const noexcept;
    uint32 BlockCount() const noexcept;

    // 다음 블록을 압축해 payloadOutput 뒤에 붙이고 붙인 byte 수를 반환한다. 블록 크기는 마지막
    // 블록만 짧을 수 있다(정확히 BlockSize, 마지막은 나머지). 실패하면 payloadOutput 길이와 writer
    // 상태가 그대로라 같은 블록을 다시 줄 수 있다.
    // 실패: 크기가 다르거나 블록을 모두 받았거나 rawBlock과 payloadOutput 저장소가 겹치면
    // InvalidArgument, 출력 성장 OutOfMemory·Overflow, 코덱 문맥 생성 OutOfMemory, 코덱 계약 밖의
    // 실패 DependencyFailed, 이동된 객체 StaleHandle.
    Result<usize> TryAppendBlock(Span<const uint8> rawBlock, Array<uint8>& payloadOutput) noexcept;

    // 모든 블록을 받은 뒤 인덱스(헤더 + 블록 표)를 indexOut에 쓴다. indexOut 크기는 정확히
    // IndexBytes여야 한다. 실패: 블록이 남았거나 크기가 다르면 InvalidArgument.
    Result<void> TryFinishIndex(Span<uint8> indexOut) const noexcept;

private:
    struct State;

    explicit CompressedBlocksWriter(State* state) noexcept : state_(state) {
    }

    State* state_ = nullptr;
};

// TryLocate 결과다. 읽기 범위가 걸치는 블록과 그 저장 byte 범위(컨테이너 처음 기준)다.
struct CompressedBlocksLocation {
    uint32 firstBlock = 0;
    uint32 blockCount = 0;
    uint64 storedOffset = 0;
    uint64 storedBytes = 0;
};

class CompressedBlocksReadContext;

// 컨테이너 인덱스를 검사해 연 reader다. 열린 뒤에는 불변이라 여러 스레드가 const로 공유한다(읽기
// 문맥은 스레드마다 따로 둔다). 이동 전용이며 이동된 객체는 StaleHandle이다. 할당기는 객체보다
// 오래 살아야 한다.
class CompressedBlocksReader {
public:
    CompressedBlocksReader() noexcept = default;
    ~CompressedBlocksReader() noexcept;
    CompressedBlocksReader(CompressedBlocksReader&& other) noexcept;
    CompressedBlocksReader& operator=(CompressedBlocksReader&& other) noexcept;
    CompressedBlocksReader(const CompressedBlocksReader&) = delete;
    CompressedBlocksReader& operator=(const CompressedBlocksReader&) = delete;

    // 헤더 48byte만 검사해 인덱스 크기(헤더 + 블록 표)를 돌려준다. 파일은 이만큼만 먼저 읽는다.
    // containerBytes는 컨테이너 전체 길이다. 헤더는 크기 일관성(블록 수·payload 합·전체 길이)까지
    // 검사하므로 악성 헤더로 컨테이너 길이에 비례하지 않는 큰 할당을 유도할 수 없다.
    static Result<usize> TryGetIndexBytes(Span<const uint8> header, uint64 containerBytes) noexcept;

    // index(컨테이너 앞 IndexBytes byte)를 검사해 연다. 표 CRC·블록 저장 크기 규칙·합계를 확인하고
    // 블록별 저장 위치 표(16byte/블록)를 allocator에서 만든다. index가 더 길면 앞부분만 쓴다.
    static Result<CompressedBlocksReader> TryOpen(Span<const uint8> index, uint64 containerBytes,
                                                  Allocator& allocator) noexcept;
    // 메모리에 있는 컨테이너 전체로 연다.
    static Result<CompressedBlocksReader> TryOpen(Span<const uint8> container,
                                                  Allocator& allocator) noexcept;

    // 사전 후보를 주는 변형이다. 컨테이너가 사전을 쓰면 표 CRC 확인 뒤 지문이 같은 후보를 골라
    // handle 복사본을 가진다(없으면 Unsupported, 지문이 같고 내용이 다른 후보가 둘 이상이면
    // InvalidArgument). 사전을 쓰지 않는 컨테이너는 후보를 무시하므로
    // 범용 로더는 항상 패키지의 사전들을 넘겨도 된다. 후보 안의 빈 handle은 StaleHandle이다.
    // reader는 사전을 파싱하거나 만들지 않는다(악성 컨테이너가 사전 할당을 유도할 수 없다).
    static Result<CompressedBlocksReader> TryOpen(Span<const uint8> index, uint64 containerBytes,
                                                  Span<const DecompressDictionary> dictionaries,
                                                  Allocator& allocator) noexcept;
    static Result<CompressedBlocksReader> TryOpen(Span<const uint8> container,
                                                  Span<const DecompressDictionary> dictionaries,
                                                  Allocator& allocator) noexcept;

    bool IsOpen() const noexcept {
        return allocator_ != nullptr;
    }

    CompressionFormat Format() const noexcept {
        return format_;
    }

    uint64 RawSize() const noexcept {
        return rawSize_;
    }

    uint32 BlockCount() const noexcept {
        return blockCount_;
    }

    uint32 BlockSizeLog2() const noexcept {
        return blockSizeLog2_;
    }

    usize IndexBytes() const noexcept {
        return indexBytes_;
    }

    uint64 ContainerBytes() const noexcept {
        return containerBytes_;
    }

    bool UsesDictionary() const noexcept {
        return static_cast<bool>(dictionary_);
    }

    // 컨테이너 헤더의 사전 지문(사전 SHA-256 앞 8byte의 little-endian 값)이다. 진단용이며 사전을
    // 쓰지 않으면 0이다.
    uint64 DictionaryFingerprint() const noexcept {
        return dictionaryFingerprint_;
    }

    // [rawOffset, rawOffset + rawBytes)가 걸치는 블록과 저장 byte 범위다. rawBytes 0은 빈 결과다.
    // 실패: rawSize를 넘는 범위는 InvalidArgument, 이동된 객체는 StaleHandle.
    Result<CompressedBlocksLocation> TryLocate(uint64 rawOffset, uint64 rawBytes) const noexcept;

    // 원본 [rawOffset, rawOffset + destination.Size())를 destination에 해제한다.
    // - stored는 컨테이너의 [storedBase, storedBase + stored.Size()) byte다. 필요한 블록의
    //   저장 byte가 그 안에 있는지 다시 계산해 확인한다(호출자가 준 범위를 믿지 않는다).
    // - verify면 블록 저장 byte의 CRC를 해제 전에 확인한다(기본 켬).
    // - context는 이 reader와 같은 형식, 같거나 큰 블록 크기로 만든 것이어야 한다(아니면
    //   InvalidArgument). 블록 일부만 걸치는 읽기는 context 버퍼로 해제하고, 같은 블록을 이어
    //   읽으면 다시 해제하지 않는다. 블록 전체를 덮는 부분은 destination에 바로 해제한다.
    // - 0byte 읽기는 빈 성공이다. 실패하면 destination 내용은 미정이다.
    // 실패: 범위 밖·stored가 블록을 덮지 않음·CRC 불일치·손상은 InvalidArgument, 할당·코덱 오류.
    Result<void> TryRead(uint64 rawOffset, Span<uint8> destination, Span<const uint8> stored,
                         uint64 storedBase, CompressedBlocksReadContext& context,
                         bool verify = true) const noexcept;
    // 메모리 컨테이너 전체를 stored로 쓰는 편의 함수다(storedBase 0).
    Result<void> TryRead(uint64 rawOffset, Span<uint8> destination, Span<const uint8> container,
                         CompressedBlocksReadContext& context, bool verify = true) const noexcept;

    // 큰 범위 병렬 해제다. 블록은 destination에 바로 해제하고 걸치는 끝 블록만 임시 버퍼를 쓴다.
    // 작업 분배·제출 거부·반환 시점·WrongThread·scratch 조건은 병렬 TryCompressBlocksAppend와 같다
    // (1MiB 미만이거나 블록 4개 이하인 범위는 호출 스레드가 혼자 읽는다). 실패하면 가장 앞 블록의
    // 오류이고 destination 내용은 미정이다.
    Result<void> TryRead(WorkerScheduler& scheduler, uint64 rawOffset, Span<uint8> destination,
                         Span<const uint8> stored, uint64 storedBase, Allocator& scratch,
                         bool verify = true) const noexcept;

    // 해제하지 않고 모든 블록 저장 byte의 CRC를 확인한다. stored는 모든 블록을 덮어야 한다.
    Result<void> TryVerify(Span<const uint8> stored, uint64 storedBase) const noexcept;

private:
    friend class CompressedBlocksReadContext;
    friend struct CompressedBlocksAccess;

    struct Entry {
        uint64 storedOffset; // 컨테이너 처음 기준
        uint32 storedSize;
        uint32 storedCrc;
    };

    void Release() noexcept;

    Allocator* allocator_ = nullptr;
    Entry* entries_ = nullptr;
    uint64 rawSize_ = 0;
    uint64 containerBytes_ = 0;
    usize indexBytes_ = 0;
    uint32 blockCount_ = 0;
    uint32 blockSizeLog2_ = 0;
    CompressionFormat format_ = CompressionFormat::Zstd;
    // TryOpen마다 새로 받는 번호다(0은 쓰지 않는다). 읽기 문맥 캐시의 식별자다.
    uint64 id_ = 0;
    // 컨테이너가 쓰는 사전이다. 읽기 문맥은 사전과 무관하며 블록을 해제할 때마다 이 사전을 건다.
    SharedPtr<detail::DictionaryState> dictionary_;
    uint64 dictionaryFingerprint_ = 0;
};

// 읽기 문맥이다. 스레드마다 하나 둔다. 같은 형식의 Decompressor, 블록 크기 버퍼 하나, 마지막으로
// 해제한 블록 캐시를 가진다. 캐시는 (reader가 열릴 때 받은 고유 번호, 블록)으로 식별하므로 다른
// reader나 다시 연 reader와 섞이지 않는다. 같은 reader로 읽는 저장 byte 자체를 바꿨다면
// Invalidate를 부른다. 이동 전용이다.
class CompressedBlocksReadContext {
public:
    CompressedBlocksReadContext() noexcept = default;
    CompressedBlocksReadContext(CompressedBlocksReadContext&& other) noexcept;
    CompressedBlocksReadContext& operator=(CompressedBlocksReadContext&& other) noexcept;
    CompressedBlocksReadContext(const CompressedBlocksReadContext&) = delete;
    CompressedBlocksReadContext& operator=(const CompressedBlocksReadContext&) = delete;
    ~CompressedBlocksReadContext() noexcept;

    // reader의 형식·블록 크기로 만든다. 실패: 열리지 않은 reader는 StaleHandle, OutOfMemory.
    static Result<CompressedBlocksReadContext> TryCreate(const CompressedBlocksReader& reader,
                                                         Allocator& allocator) noexcept;
    // 형식·블록 크기로 만든다(여러 컨테이너에 같은 문맥을 쓸 때). 실패: Zlib·Gzip·Lz4Frame·모르는
    // 형식·blockSizeLog2 범위 밖은 InvalidArgument, OutOfMemory.
    static Result<CompressedBlocksReadContext>
    TryCreate(CompressionFormat format, uint32 blockSizeLog2, Allocator& allocator) noexcept;

    // 캐시한 블록을 버린다.
    void Invalidate() noexcept {
        cached_ = false;
    }

private:
    friend class CompressedBlocksReader;
    friend struct CompressedBlocksAccess;

    void Release() noexcept;

    Decompressor decompressor_;
    Allocator* allocator_ = nullptr;
    uint8* buffer_ = nullptr;
    CompressionFormat format_ = CompressionFormat::Zstd;
    uint32 blockSizeLog2_ = 0;
    bool cached_ = false;
    // 캐시한 블록의 CRC를 확인했는지다(verify 없이 채운 캐시를 verify 읽기가 쓰면 그때 확인한다).
    bool cachedVerified_ = false;
    uint64 cachedReader_ = 0;
    uint32 cachedBlock_ = 0;
};

} // namespace rocks
