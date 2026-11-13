#include "CompressionTestSupport.h"

#include <Compression/AsyncCompression.h>
#include <Compression/CompressedBlocks.h>
#include <Compression/CompressionDictionary.h>
#include <Compression/CompressionStream.h>
#include <Core/Encoding/Crc32.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

#include <stdio.h>

// 내부 경계(취소 probe)를 결정적으로 시험한다. 시험은 저장소 루트를 include 경로로 쓴다
// (AsyncCompressionTests와 같은 방식).
#include <Core/Source/Runtime/Compression/Private/CompressionInternal.h>

// LZ4 frame(CompressionFormat::Lz4Frame) 시험이다(Docs/Design/Compression.ko.md 12절). 독립 자료는
// 공식 LZ4 1.11.1 tarball 원본만으로 빌드한 probe(scratchpad, 저장소 밖)가 만들었다:
// - upstream LZ4F_compressFrame의 기본 설정 frame, adapter와 같은 설정·호출 순서로 upstream이 만든
//   frame
// - upstream이 독립 블록·블록 checksum·4MiB 블록·비압축 블록을 섞어 만든 frame
// - 원본 크기 필드만 바꾸고 헤더 checksum(XXH32)을 다시 맞춘 frame, 사전 ID를 기록한 frame
// - 1.5MiB 입력의 일회성(수준 1·2·9·12)·스트림(수준 1·9) 결과 크기·CRC-32. 일회성 값은 upstream
//   LZ4F_compressFrame 결과와 byte 단위로 같고, 스트림 값은 upstream에 1byte–1MiB 조각으로 넣어도
//   같았다.
// probe 호출 순서(LZ4 버전을 올릴 때 같은 순서로 다시 만든다). 할당기는 모두
// LZ4F_create*_advanced로 넘기고 lz4frame.c는 LZ4_USER_MEMORY_FUNCTIONS 감시 함수로 빌드했다.
// - 공통 설정 P(level, contentSize, autoFlush): blockSizeID max64KB, blockLinked, contentChecksum
//   1, blockChecksum 0, favorDecSpeed 0, nbWorkers 0.
// - adapter 일회성: compressBegin(P(L, n, 1)) → 1MiB 조각마다 compressUpdate(stableSrc =
//   LZ4F_srcStable) → compressEnd. 비교 대상 LZ4F_compressFrame(P(L, 1, 0))은 원본 크기를 n으로
//   고치고 autoFlush를 켠다. AdapterLevel1Frame·HugeDeclaredFrame의 원본도 이 순서다.
// - adapter 스트림: compressBegin(P(L, 0, 0)) → 4099byte 조각(각 조각을 65535byte 이하로 다시
//   나눔)마다 compressUpdate(옵션 NULL) → 절반 위치에서 LZ4F_flush → 나머지 조각 → compressEnd.
// - UpstreamDefaultFrame: LZ4F_compressFrame(GoldenText, NULL).
// - DictionaryIdFrame: P(1, 0, 1)에 dictID 7 → compressBegin → compressUpdate("abc") → compressEnd.
// - UpstreamMixedFrame: P(9, 151300, 0)에 blockSizeID max4MB·blockIndependent·blockChecksum 1 →
//   compressBegin → compressUpdate(0 150000byte) → compressUpdate(난수 300byte, uncompressed = 1:
//   모드 전환이 앞 블록을 끝낸다) → compressUpdate(0 1000byte) → compressEnd.
// - Lying*·HugeDeclared: frame의 원본 크기 필드(6–13 byte)를 바꾸고 헤더 checksum(14 byte) =
//   (XXH32(frame + 4, 10, 0) >> 8) & 0xFF로 다시 맞췄다.
namespace rocks::test {
namespace {

using namespace compression;

constexpr CompressionFormat Frame = CompressionFormat::Lz4Frame;

usize Smaller(usize first, usize second) {
    return first < second ? first : second;
}

Span<const uint8> Slice(Span<const uint8> data, usize offset, usize size) {
    return Span<const uint8>(data.Data() + offset, size);
}

Span<const uint8> Text(const char* text) {
    usize length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return Span<const uint8>(reinterpret_cast<const uint8*>(text), length);
}

template <usize Count>
Span<const uint8> Bytes(const uint8 (&values)[Count]) {
    return Span<const uint8>(values, Count);
}

DecompressStreamLimits Limits(usize maxOutputBytes) {
    DecompressStreamLimits limits;
    limits.maxOutputBytes = maxOutputBytes;
    return limits;
}

template <typename T>
ErrorCode ErrorOf(const Result<T>& result) {
    return result ? ErrorCode::None : result.Error();
}

// golden 입력 문장(67byte).
constexpr char GoldenText[] =
    "rocks lz4 frame golden: hello hello hello hello rocks rocks rocks!\n";

// upstream LZ4F_compressFrame(GoldenText, 기본 설정): 체크섬·원본 크기 없음, 블록 하나라 독립 블록.
constexpr uint8 UpstreamDefaultFrame[] = {
    0x04, 0x22, 0x4d, 0x18, 0x60, 0x40, 0x82, 0x2e, 0x00, 0x00, 0x00, 0xff, 0x0e, 0x72, 0x6f, 0x63,
    0x6b, 0x73, 0x20, 0x6c, 0x7a, 0x34, 0x20, 0x66, 0x72, 0x61, 0x6d, 0x65, 0x20, 0x67, 0x6f, 0x6c,
    0x64, 0x65, 0x6e, 0x3a, 0x20, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x06, 0x00, 0x00, 0x02, 0x30, 0x00,
    0x04, 0x06, 0x00, 0x50, 0x63, 0x6b, 0x73, 0x21, 0x0a, 0x00, 0x00, 0x00, 0x00};

// adapter와 같은 설정·호출 순서(수준 1)로 upstream이 만든 GoldenText frame. adapter 결과가 byte
// 단위로 같다.
constexpr uint8 AdapterLevel1Frame[] = {
    0x04, 0x22, 0x4d, 0x18, 0x4c, 0x40, 0x43, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf5,
    0x2e, 0x00, 0x00, 0x00, 0xff, 0x0e, 0x72, 0x6f, 0x63, 0x6b, 0x73, 0x20, 0x6c, 0x7a, 0x34,
    0x20, 0x66, 0x72, 0x61, 0x6d, 0x65, 0x20, 0x67, 0x6f, 0x6c, 0x64, 0x65, 0x6e, 0x3a, 0x20,
    0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x06, 0x00, 0x00, 0x02, 0x30, 0x00, 0x04, 0x06, 0x00, 0x50,
    0x63, 0x6b, 0x73, 0x21, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x57, 0x89, 0x23, 0x9f};

// AdapterLevel1Frame의 원본 크기 필드를 66·68로 바꾸고 헤더 checksum을 다시 맞춘 frame(실제
// 67byte).
constexpr uint8 LyingSmallerFrame[] = {
    0x04, 0x22, 0x4d, 0x18, 0x4c, 0x40, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1d,
    0x2e, 0x00, 0x00, 0x00, 0xff, 0x0e, 0x72, 0x6f, 0x63, 0x6b, 0x73, 0x20, 0x6c, 0x7a, 0x34,
    0x20, 0x66, 0x72, 0x61, 0x6d, 0x65, 0x20, 0x67, 0x6f, 0x6c, 0x64, 0x65, 0x6e, 0x3a, 0x20,
    0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x06, 0x00, 0x00, 0x02, 0x30, 0x00, 0x04, 0x06, 0x00, 0x50,
    0x63, 0x6b, 0x73, 0x21, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x57, 0x89, 0x23, 0x9f};
constexpr uint8 LyingLargerFrame[] = {
    0x04, 0x22, 0x4d, 0x18, 0x4c, 0x40, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4a,
    0x2e, 0x00, 0x00, 0x00, 0xff, 0x0e, 0x72, 0x6f, 0x63, 0x6b, 0x73, 0x20, 0x6c, 0x7a, 0x34,
    0x20, 0x66, 0x72, 0x61, 0x6d, 0x65, 0x20, 0x67, 0x6f, 0x6c, 0x64, 0x65, 0x6e, 0x3a, 0x20,
    0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x06, 0x00, 0x00, 0x02, 0x30, 0x00, 0x04, 0x06, 0x00, 0x50,
    0x63, 0x6b, 0x73, 0x21, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x57, 0x89, 0x23, 0x9f};

// "abc" frame의 원본 크기를 2^40으로 바꾸고 헤더 checksum을 다시 맞춘 frame.
constexpr uint8 HugeDeclaredFrame[] = {0x04, 0x22, 0x4d, 0x18, 0x4c, 0x40, 0x00, 0x00, 0x00, 0x00,
                                       0x00, 0x01, 0x00, 0x00, 0xa2, 0x03, 0x00, 0x00, 0x80, 0x61,
                                       0x62, 0x63, 0x00, 0x00, 0x00, 0x00, 0xff, 0x53, 0xd1, 0x32};

// 사전 ID 7을 기록한 "abc" frame(사전은 실제로 쓰지 않았다).
constexpr uint8 DictionaryIdFrame[] = {0x04, 0x22, 0x4d, 0x18, 0x45, 0x40, 0x07, 0x00, 0x00,
                                       0x00, 0x8c, 0x03, 0x00, 0x00, 0x80, 0x61, 0x62, 0x63,
                                       0x00, 0x00, 0x00, 0x00, 0xff, 0x53, 0xd1, 0x32};

// upstream이 4MiB 블록 크기·독립 블록·블록 checksum·content checksum·원본 크기·수준 9로 만든
// frame이다. 내용은 0 150000byte, 고정 seed 난수 300byte(Fill Random seed 99, 비압축 블록으로
// 저장), 0 1000byte다.
constexpr usize MixedContentBytes = 151300;
constexpr uint8 UpstreamMixedFrame[] = {
    0x04, 0x22, 0x4d, 0x18, 0x7c, 0x70, 0x04, 0x4f, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x57,
    0x02, 0x00, 0x00, 0x1f, 0x00, 0x01, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0x23, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x43, 0x58, 0x17, 0xde, 0x2c, 0x01,
    0x00, 0x80, 0x18, 0x48, 0x82, 0xfe, 0xd3, 0xeb, 0x67, 0x1d, 0x3c, 0xae, 0x22, 0xc1, 0xd1, 0xc6,
    0xb7, 0xd2, 0xcc, 0x22, 0x9f, 0xab, 0x4e, 0x0c, 0x94, 0x49, 0x38, 0xc0, 0x3c, 0xef, 0xe9, 0xb5,
    0xc2, 0xb6, 0xfc, 0xbc, 0x95, 0x9b, 0xfb, 0xb0, 0xe6, 0x1f, 0x83, 0xbb, 0xac, 0x69, 0x24, 0x15,
    0x52, 0xcf, 0x06, 0x0d, 0x9f, 0x21, 0x3e, 0x92, 0xc2, 0xcd, 0xb9, 0x09, 0x95, 0xa5, 0xf5, 0xb4,
    0x62, 0x23, 0xf2, 0xc7, 0xa6, 0x9f, 0x16, 0x82, 0xd1, 0x2e, 0x7b, 0x24, 0x8b, 0x0a, 0x1b, 0x2f,
    0x44, 0x8c, 0x33, 0x69, 0x2a, 0x6f, 0x6c, 0xbb, 0xb5, 0x7c, 0xb0, 0x16, 0xd4, 0x10, 0x60, 0x09,
    0x90, 0x8c, 0x3b, 0x73, 0x82, 0xae, 0x8c, 0x23, 0x99, 0x3b, 0xe6, 0x0f, 0x66, 0x35, 0x62, 0xb9,
    0x3d, 0xcf, 0xf2, 0x35, 0x22, 0x53, 0x11, 0xea, 0xf8, 0x64, 0x4a, 0xd6, 0x14, 0xa5, 0x72, 0x5f,
    0xf1, 0x0d, 0x72, 0x3b, 0xef, 0xd8, 0x29, 0xe3, 0xb0, 0x49, 0x2a, 0xd7, 0x01, 0xa9, 0x35, 0x19,
    0x3d, 0x0a, 0x95, 0x3c, 0xa7, 0xa6, 0x83, 0xaf, 0x45, 0xba, 0xaa, 0x69, 0x18, 0xcd, 0xab, 0xa1,
    0x9a, 0x9a, 0x15, 0x44, 0xf0, 0xf3, 0xb9, 0x27, 0x08, 0x53, 0xca, 0x21, 0xa2, 0x45, 0x6b, 0xa9,
    0xb0, 0xf8, 0xa6, 0x78, 0x64, 0x97, 0x6d, 0xb7, 0x75, 0xb2, 0x86, 0xf9, 0x8f, 0xb2, 0xa8, 0xfa,
    0x21, 0x97, 0xaa, 0xd6, 0xa5, 0x74, 0xba, 0xdb, 0x34, 0x22, 0xab, 0x93, 0x88, 0x42, 0x46, 0x2a,
    0xca, 0x0a, 0x10, 0x01, 0xca, 0xc3, 0x07, 0x43, 0x55, 0x49, 0x98, 0x46, 0x70, 0x82, 0xdf, 0x72,
    0x82, 0x77, 0xeb, 0xb3, 0x72, 0x48, 0x32, 0x4b, 0xcd, 0x71, 0x10, 0x93, 0x2b, 0x00, 0xa8, 0xad,
    0xe0, 0x0e, 0x50, 0x9b, 0x37, 0x20, 0x67, 0x87, 0x5c, 0xc9, 0x8d, 0x7f, 0xb6, 0x84, 0x30, 0xca,
    0x25, 0xeb, 0x3a, 0x85, 0x5d, 0xa6, 0xef, 0xdc, 0x29, 0x9c, 0x01, 0x1b, 0x1a, 0xdc, 0xe4, 0xd6,
    0xd4, 0xfd, 0x9a, 0x7a, 0xbd, 0x39, 0x0e, 0x82, 0x83, 0x9b, 0x5e, 0x9e, 0x78, 0x28, 0x1b, 0x34,
    0xf6, 0x66, 0x16, 0xdd, 0x24, 0xf6, 0xc9, 0xf9, 0x2a, 0x2b, 0x30, 0x3a, 0xc3, 0x00, 0x77, 0x4a,
    0x4a, 0x92, 0x0e, 0x00, 0x00, 0x00, 0x1f, 0x00, 0x01, 0x00, 0xff, 0xff, 0xff, 0xd2, 0x50, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x14, 0x4f, 0xf4, 0xf7, 0x00, 0x00, 0x00, 0x00, 0xb4, 0x4d, 0x61, 0x41,
};

// UpstreamMixedFrame의 원본을 만든다.
void MakeMixedContent(Array<uint8>& content) {
    Fill(content, MixedContentBytes, Pattern::Zeros);
    Array<uint8> noise(content.GetAllocator());
    Fill(noise, 300, Pattern::Random, 99);
    for (usize index = 0; index < 300; ++index) {
        content[150000 + index] = noise[index];
    }
}

// 일회성 Span·Append 두 경로로 해제해 기대값과 같은지 확인한다.
bool DecodesTo(Span<const uint8> encoded, Span<const uint8> expected) {
    SystemAllocator allocator;
    bool ok = true;
    {
        Array<uint8> output(allocator);
        auto appended = TryDecompressAppend(Frame, encoded, expected.Size(), output);
        ok = ok && appended && appended.Value() == expected.Size() && Same(output, expected);
        Array<uint8> exact(allocator);
        ok = ok && exact.TryResize(expected.Size() + 7);
        auto written =
            TryDecompress(Frame, encoded, Span<uint8>(exact.Data(), exact.Size()), allocator);
        ok = ok && written && written.Value() == expected.Size() &&
             Same(Span<const uint8>(exact.Data(), written.Value()), expected);
    }
    return ok && allocator.Stats().liveAllocations == 0;
}

// 일회성 Append·Span 해제와 스트림 해제(1byte·전체 조각)의 오류가 모두 expected인지 확인한다.
// 스트림은 쓰기 또는 TryFinish에서 실패한다.
bool FailsEverywhere(Span<const uint8> encoded, ErrorCode expected,
                     usize maxOutputBytes = 1 << 20) {
    SystemAllocator allocator;
    bool ok = true;
    {
        Array<uint8> output(allocator);
        ok = ok &&
             ErrorOf(TryDecompressAppend(Frame, encoded, maxOutputBytes, output)) == expected &&
             output.Size() == 0;
        Array<uint8> fixed(allocator);
        ok = ok && fixed.TryResize(maxOutputBytes);
        ok = ok && ErrorOf(TryDecompress(Frame, encoded, Span<uint8>(fixed.Data(), fixed.Size()),
                                         allocator)) == expected;
        const usize Chunks[] = {1, encoded.Size()};
        for (usize chunk : Chunks) {
            auto decoder = DecompressStream::TryCreate(Frame, Limits(maxOutputBytes), allocator);
            ok = ok && decoder;
            if (!decoder) {
                break;
            }
            Array<uint8> streamed(allocator);
            ErrorCode error = ErrorCode::None;
            for (usize offset = 0; offset < encoded.Size() && error == ErrorCode::None;
                 offset += chunk) {
                error = ErrorOf(decoder.Value().TryWriteAppend(
                    Slice(encoded, offset, Smaller(chunk, encoded.Size() - offset)), streamed));
            }
            if (error == ErrorCode::None) {
                error = ErrorOf(decoder.Value().TryFinish());
            }
            ok = ok && error == expected;
        }
    }
    return ok && allocator.Stats().liveAllocations == 0;
}

// input을 chunk 단위로 붙여 쓰고(flushAt에서 flush) 스트림을 끝낸다.
bool CompressInChunks(CompressStream& stream, Span<const uint8> input, usize chunk,
                      Array<uint8>& output, usize flushAt = static_cast<usize>(-1)) {
    usize offset = 0;
    while (offset < input.Size()) {
        const usize limit = offset < flushAt && flushAt < input.Size() ? flushAt : input.Size();
        const usize take = Smaller(chunk, limit - offset);
        if (!stream.TryWriteAppend(Slice(input, offset, take), output)) {
            return false;
        }
        offset += take;
        if (offset == flushAt && !stream.TryFlushAppend(output)) {
            return false;
        }
    }
    return stream.TryFinishAppend(output).IsOk();
}

bool DecompressInChunks(DecompressStream& stream, Span<const uint8> input, usize chunk,
                        Array<uint8>& output) {
    for (usize offset = 0; offset < input.Size(); offset += chunk) {
        if (!stream.TryWriteAppend(Slice(input, offset, Smaller(chunk, input.Size() - offset)),
                                   output)) {
            return false;
        }
    }
    return true;
}

// 요청 하나(Flush·Finish)를 출력 창 window로 complete까지 반복한다. 호출 수는 유한해야 한다.
template <typename Request>
bool RepeatUntilComplete(Request&& request, Span<uint8> window, Array<uint8>& output) {
    for (usize call = 0; call < 2000000; ++call) {
        auto step = request(window);
        if (!step || step.Value().produced > window.Size() ||
            !output.TryAppend(Span<const uint8>(window.Data(), step.Value().produced))) {
            return false;
        }
        if (step.Value().complete) {
            return true;
        }
    }
    return false;
}

// TryProcess·TryFlush·TryFinish만으로 압축한다. 입력은 inputChunk, 출력 창은 window byte씩이고
// flushAt에서 flush한다.
bool CompressWithBackpressure(CompressStream& stream, Span<const uint8> input, usize inputChunk,
                              usize window, Array<uint8>& output,
                              usize flushAt = static_cast<usize>(-1)) {
    Array<uint8> scratch(output.GetAllocator());
    if (!scratch.TryResize(window)) {
        return false;
    }

    const Span<uint8> target(scratch.Data(), scratch.Size());
    usize offset = 0;
    bool flushed = false;
    while (offset < input.Size()) {
        if (offset == flushAt && !flushed) {
            flushed = true;
            if (!RepeatUntilComplete([&](Span<uint8> to) { return stream.TryFlush(to); }, target,
                                     output)) {
                return false;
            }
        }
        const usize limit = offset < flushAt && flushAt < input.Size() ? flushAt : input.Size();
        auto step =
            stream.TryProcess(Slice(input, offset, Smaller(inputChunk, limit - offset)), target);
        if (!step || step.Value().produced > window ||
            !output.TryAppend(Span<const uint8>(scratch.Data(), step.Value().produced))) {
            return false;
        }
        offset += step.Value().consumed;
        if (step.Value().consumed == 0 && step.Value().produced == 0) {
            return false;
        }
    }
    return RepeatUntilComplete([&](Span<uint8> to) { return stream.TryFinish(to); }, target,
                               output);
}

// TryProcess만으로 해제한다. 한 호출의 출력은 window를 넘지 않아야 한다(역압).
bool DecompressWithBackpressure(DecompressStream& stream, Span<const uint8> input, usize inputChunk,
                                usize window, Array<uint8>& output) {
    Array<uint8> scratch(output.GetAllocator());
    if (!scratch.TryResize(window)) {
        return false;
    }

    const Span<uint8> target(scratch.Data(), scratch.Size());
    usize offset = 0;
    while (true) {
        auto step = stream.TryProcess(
            Slice(input, offset, Smaller(inputChunk, input.Size() - offset)), target);
        if (!step || step.Value().produced > window ||
            !output.TryAppend(Span<const uint8>(scratch.Data(), step.Value().produced))) {
            return false;
        }
        offset += step.Value().consumed;
        if (offset == input.Size() && step.Value().complete) {
            return true;
        }
        if (step.Value().consumed == 0 && step.Value().produced == 0) {
            return false;
        }
    }
}

// 일회성 해제기와 스트림 해제기(전체·4099byte 조각) 모두로 확인한다.
bool StreamDecodesTo(Span<const uint8> encoded, Span<const uint8> expected) {
    SystemAllocator allocator;
    bool ok = true;
    {
        Array<uint8> once(allocator);
        auto decoded = TryDecompressAppend(Frame, encoded, expected.Size() + 1, once);
        ok = ok && decoded && Same(once, expected);

        const usize Chunks[] = {encoded.Size() == 0 ? 1 : encoded.Size(), 4099};
        for (usize chunk : Chunks) {
            auto stream =
                DecompressStream::TryCreate(Frame, Limits(expected.Size() + 1), allocator);
            Array<uint8> streamed(allocator);
            ok = ok && stream && DecompressInChunks(stream.Value(), encoded, chunk, streamed) &&
                 stream.Value().TryFinish() && stream.Value().IsAtStreamBoundary() &&
                 Same(streamed, expected);
        }
    }
    return ok && allocator.Stats().liveAllocations == 0;
}

// 프레임 헤더의 FLG·원본 크기 필드다.
bool RecordsContentSize(Span<const uint8> frame, uint64 size) {
    if (frame.Size() < 15 || (frame[4] & 0x08) == 0) {
        return false;
    }
    uint64 recorded = 0;
    for (usize index = 0; index < 8; ++index) {
        recorded |= static_cast<uint64>(frame[6 + index]) << (8 * index);
    }
    return recorded == size;
}

// N번째 확인부터 취소를 보고하는 probe다. 확인 횟수도 센다.
struct CountdownProbe {
    uint32 remaining;
    uint32 checks = 0;

    static bool Check(void* raw) noexcept {
        auto& self = *static_cast<CountdownProbe*>(raw);
        ++self.checks;
        if (self.remaining == 0) {
            return true;
        }
        --self.remaining;
        return false;
    }
};

} // namespace

TEST(Lz4Frame_LevelsBoundAndUnsupportedUses) {
    const auto range = TryGetCompressionLevelRange(Frame).Value();
    CHECK(range.minimum == 1 && range.maximum == 12 && range.defaultLevel == 1);
    CHECK(CompressionSettings::TryMake(Frame, 0).Error() == ErrorCode::InvalidArgument);
    CHECK(CompressionSettings::TryMake(Frame, 13).Error() == ErrorCode::InvalidArgument);
    CHECK(CompressionSettings::TryMake(Frame, 12));
    CHECK(CompressionSettings::TryDefault(Frame).Value().Level() == 1);

    // bound = 헤더 19 + 64KiB 블록마다 4 + 원본 + endmark·checksum 8. Lz4 raw block의 입력 한계는
    // 없다.
    struct BoundCase {
        usize size;
        usize bound;
    };
    static constexpr BoundCase Bounds[] = {
        {0, 27},
        {1, 32},
        {65536, 65567},
        {65537, 65572},
        {(usize{1} << 20) + 5, (usize{1} << 20) + 5 + 27 + 17 * 4}};
    for (const BoundCase& item : Bounds) {
        CHECK(TryGetCompressBound(Frame, item.size).Value() == item.bound);
    }
    CHECK(TryGetCompressBound(Frame, Lz4MaxInputBytes + 1));
    CHECK(TryGetCompressBound(Frame, static_cast<usize>(-1)).Error() == ErrorCode::Overflow);
    CHECK(TryGetCompressBound(Frame, static_cast<usize>(-1) - (usize{1} << 20)).Error() ==
          ErrorCode::Overflow);

    SystemAllocator allocator;
    {
        // 사전은 zstd만 지원한다. 아는 형식이므로 Unsupported다.
        const Span<const uint8> content = Text("lz4 frame dictionary content");
        CHECK(CompressDictionary::TryCreate(DefaultSettings(Frame), content, allocator).Error() ==
              ErrorCode::Unsupported);
        CHECK(DecompressDictionary::TryCreate(Frame, content, allocator).Error() ==
              ErrorCode::Unsupported);
        // 블록 컨테이너의 블록 형식이 아니다(LZ4 블록은 raw block으로 저장한다).
        CHECK(CompressedBlocksSettings::TryMake(DefaultSettings(Frame)).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(
            CompressedBlocksReadContext::TryCreate(Frame, DefaultCompressedBlockSizeLog2, allocator)
                .Error() == ErrorCode::InvalidArgument);
        CHECK(CompressedBlocksSettings::TryMake(DefaultSettings(CompressionFormat::Lz4)));
        // Lz4 raw block은 여전히 스트림 형식이 아니다.
        CHECK(
            CompressStream::TryCreate(DefaultSettings(CompressionFormat::Lz4), allocator).Error() ==
            ErrorCode::Unsupported);
        CHECK(CompressStream::TryCreate(DefaultSettings(Frame), allocator));
        CHECK(DecompressStream::TryCreate(Frame, Limits(0), allocator).Error() ==
              ErrorCode::InvalidArgument);
        DecompressStreamLimits wide = Limits(1);
        wide.maxWindowLog = MaxZstdWindowLog + 1;
        CHECK(DecompressStream::TryCreate(Frame, wide, allocator).Error() ==
              ErrorCode::InvalidArgument);

        // 빈 해제 입력과 겹침은 InvalidArgument다.
        Array<uint8> packed(allocator);
        REQUIRE(TryCompressAppend(DefaultSettings(Frame), Text(GoldenText), packed));
        Array<uint8> output(allocator);
        CHECK(TryDecompressAppend(Frame, Span<const uint8>(), 100, output).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(
            TryDecompress(Frame, View(packed), Span<uint8>(packed.Data(), packed.Size()), allocator)
                .Error() == ErrorCode::InvalidArgument);
        CHECK(TryDecompressAppend(Frame, Slice(View(packed), 0, 4), 100, packed).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(
            TryCompressAppend(DefaultSettings(Frame), Slice(View(packed), 0, 4), packed).Error() ==
            ErrorCode::InvalidArgument);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(Lz4Frame_RoundTripAcrossSizesAndLevels) {
    SystemAllocator allocator;
    {
        struct Case {
            usize size;
            Pattern pattern;
        };
        // 64KiB 블록 경계와 분할 단위(1MiB)를 넘는 크기를 포함한다.
        static constexpr Case Cases[] = {{0, Pattern::Zeros},
                                         {1, Pattern::Random},
                                         {1000, Pattern::Text},
                                         {65536, Pattern::Random},
                                         {65537, Pattern::Text},
                                         {(usize{1} << 20) + 5, Pattern::Text},
                                         {(usize{1} << 20) + 5, Pattern::Zeros}};
        static constexpr int32 Levels[] = {1, 2, 9, 12};
        for (const Case& item : Cases) {
            Array<uint8> original(allocator);
            Fill(original, item.size, item.pattern, item.size + 3);
            for (int32 level : Levels) {
                const auto settings = Settings(Frame, level);
                const usize bound = TryGetCompressBound(Frame, item.size).Value();
                Array<uint8> packed(allocator);
                REQUIRE(packed.TryResize(bound));
                auto packedBytes = TryCompress(settings, View(original),
                                               Span<uint8>(packed.Data(), bound), allocator);
                REQUIRE(packedBytes);
                CHECK(packedBytes.Value() <= bound);
                REQUIRE(packed.TryResize(packedBytes.Value()));
                // 일회성은 원본 크기를 기록한다(빈 입력은 0 = 기록 없음).
                CHECK(item.size == 0 ? (packed[4] & 0x08) == 0
                                     : RecordsContentSize(View(packed), item.size));

                // Append·문맥 재사용은 같은 byte다.
                Array<uint8> appended(allocator);
                REQUIRE(appended.TryAppend(Text("prefix")));
                auto appendedBytes = TryCompressAppend(settings, View(original), appended);
                REQUIRE(appendedBytes);
                CHECK(Same(Slice(View(appended), 6, appended.Size() - 6), View(packed)));
                auto compressor = Compressor::TryCreate(settings, allocator);
                REQUIRE(compressor);
                Array<uint8> reused(allocator);
                REQUIRE(compressor.Value().TryCompressAppend(View(original), reused));
                CHECK(Same(reused, View(packed)));

                // 출력이 정확히 결과 크기여도 성공하고 같은 byte다. 1byte 작으면 용량 부족이다.
                Array<uint8> exact(allocator);
                REQUIRE(exact.TryResize(packed.Size()));
                auto exactBytes = TryCompress(settings, View(original),
                                              Span<uint8>(exact.Data(), exact.Size()), allocator);
                CHECK(exactBytes && exactBytes.Value() == packed.Size() &&
                      Same(exact, View(packed)));
                CHECK(TryCompress(settings, View(original),
                                  Span<uint8>(exact.Data(), exact.Size() - 1), allocator)
                          .Error() == ErrorCode::CapacityExceeded);

                // 정확히 원본 크기의 출력·상한은 성공한다.
                Array<uint8> restored(allocator);
                REQUIRE(restored.TryResize(item.size));
                auto written = TryDecompress(
                    Frame, View(packed), Span<uint8>(restored.Data(), restored.Size()), allocator);
                CHECK(written && written.Value() == item.size && Same(restored, View(original)));
                Array<uint8> grown(allocator);
                REQUIRE(grown.TryAppend(Text("xy")));
                auto grownBytes = TryDecompressAppend(Frame, View(packed), item.size, grown);
                CHECK(grownBytes && grownBytes.Value() == item.size);
                CHECK(Same(Slice(View(grown), 2, grown.Size() - 2), View(original)));
                auto decompressor = Decompressor::TryCreate(Frame, allocator);
                REQUIRE(decompressor);
                Array<uint8> again(allocator);
                CHECK(decompressor.Value().TryDecompressAppend(View(packed), item.size, again) &&
                      Same(again, View(original)));

                // 1byte 부족하면 용량 부족이며 Append는 길이를 되돌린다.
                if (item.size != 0) {
                    CHECK(TryDecompress(Frame, View(packed),
                                        Span<uint8>(restored.Data(), item.size - 1), allocator)
                              .Error() == ErrorCode::CapacityExceeded);
                    Array<uint8> limited(allocator);
                    REQUIRE(limited.TryAppend(Text("keep")));
                    CHECK(
                        TryDecompressAppend(Frame, View(packed), item.size - 1, limited).Error() ==
                        ErrorCode::CapacityExceeded);
                    CHECK(limited.Size() == 4);
                }
            }
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 재사용 문맥은 입력 크기·이전 실패와 무관하게 새 문맥(자유 함수)과 같은 byte를 낸다.
TEST(Lz4Frame_ReusedContextsMatchFreeFunctionsAfterFailures) {
    SystemAllocator allocator;
    {
        static constexpr usize Sizes[] = {100, 3000, 300000, (usize{2} << 20) + 5, 3000, 100};
        Array<uint8> inputs[6] = {Array<uint8>(allocator), Array<uint8>(allocator),
                                  Array<uint8>(allocator), Array<uint8>(allocator),
                                  Array<uint8>(allocator), Array<uint8>(allocator)};
        for (usize index = 0; index < 6; ++index) {
            Fill(inputs[index], Sizes[index], index % 2 == 0 ? Pattern::Text : Pattern::Random,
                 index + 3);
        }

        auto decompressor = Decompressor::TryCreate(Frame, allocator);
        REQUIRE(decompressor);
        static constexpr int32 Levels[] = {1, 2, 12};
        for (int32 level : Levels) {
            const auto settings = Settings(Frame, level);
            auto compressor = Compressor::TryCreate(settings, allocator);
            REQUIRE(compressor);
            for (usize index = 0; index < 6; ++index) {
                if (level == 12 && Sizes[index] > 300000) {
                    continue;
                }
                const Span<const uint8> input = View(inputs[index]);
                Array<uint8> expected(allocator);
                REQUIRE(TryCompressAppend(settings, input, expected));

                // 실패(헤더·블록·footer 단계의 출력 부족) 직후에도 다음 결과가 같아야 한다.
                const usize Tiny[] = {4, 20, expected.Size() - 1};
                for (usize tiny : Tiny) {
                    Array<uint8> small(allocator);
                    REQUIRE(small.TryResize(tiny));
                    CHECK(compressor.Value()
                              .TryCompress(input, Span<uint8>(small.Data(), tiny))
                              .Error() == ErrorCode::CapacityExceeded);
                }
                Array<uint8> packed(allocator);
                REQUIRE(compressor.Value().TryCompressAppend(input, packed));
                CHECK(Same(packed, View(expected)));

                uint8 four[4];
                CHECK(decompressor.Value()
                          .TryDecompress(View(packed), Span<uint8>(four, 4))
                          .Error() == ErrorCode::CapacityExceeded);
                Array<uint8> restored(allocator);
                CHECK(decompressor.Value().TryDecompressAppend(View(packed), input.Size(),
                                                               restored) &&
                      Same(restored, input));
            }
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 같은 입력·설정의 일회성 결과는 플랫폼과 무관하게 같아야 한다(쿠킹 재현성). 값은 upstream
// LZ4F_compressFrame이 같은 입력에서 만든 결과와 같다(probe 확인). LZ4 버전을 올리면 의도적으로
// 갱신한다.
TEST(Lz4Frame_DeterministicOneShotGoldens) {
    struct Golden {
        int32 level;
        usize size;
        uint32 crc;
    };
    static constexpr Golden Goldens[] = {
        {1, 1562140, 0xe250deb8u},
        {2, 1306125, 0xb83157f9u},
        {9, 1162174, 0x7ac190ddu},
        {12, 1157494, 0x3a2e0fa6u},
    };
    SystemAllocator allocator;
    {
        Array<uint8> original(allocator);
        // CompressionTests 결정성 golden과 같은 입력이다. 분할 단위(1MiB)를 넘긴다.
        Fill(original, (usize{3} << 19) + 11, Pattern::Text, 7);
        for (const Golden& golden : Goldens) {
            Array<uint8> packed(allocator);
            REQUIRE(TryCompressAppend(Settings(Frame, golden.level), View(original), packed));
            const uint32 crc = Crc32(View(packed));
            if (packed.Size() != golden.size || crc != golden.crc) {
                printf("lz4 frame golden mismatch: level %d size %llu crc 0x%08x\n", golden.level,
                       static_cast<unsigned long long>(packed.Size()), crc);
            }
            CHECK(packed.Size() == golden.size);
            CHECK(crc == golden.crc);
            CHECK(DecodesTo(View(packed), View(original)));
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 다른 구현(upstream LZ4F)이 만든 frame을 풀고, adapter가 만든 frame이 upstream 결과와 byte 단위로
// 같다.
TEST(Lz4Frame_UpstreamCompatibility) {
    SystemAllocator allocator;
    {
        const Span<const uint8> text = Text(GoldenText);
        CHECK(DecodesTo(Bytes(UpstreamDefaultFrame), text));
        CHECK(StreamDecodesTo(Bytes(UpstreamDefaultFrame), text));
        CHECK(DecodesTo(Bytes(AdapterLevel1Frame), text));

        // adapter 결과 = upstream이 같은 설정으로 만든 frame(upstream 해제기도 probe에서 풀었다).
        Array<uint8> packed(allocator);
        REQUIRE(TryCompressAppend(Settings(Frame, 1), text, packed));
        CHECK(Same(packed, Bytes(AdapterLevel1Frame)));

        // 독립 블록·블록 checksum·4MiB 블록·비압축 블록이 섞인 frame.
        Array<uint8> mixed(allocator);
        MakeMixedContent(mixed);
        CHECK(Crc32(View(mixed)) == 0xed4078eau);
        CHECK(DecodesTo(Bytes(UpstreamMixedFrame), View(mixed)));
        CHECK(StreamDecodesTo(Bytes(UpstreamMixedFrame), View(mixed)));
        auto decoder = DecompressStream::TryCreate(Frame, Limits(MixedContentBytes), allocator);
        REQUIRE(decoder);
        Array<uint8> streamed(allocator);
        CHECK(DecompressInChunks(decoder.Value(), Bytes(UpstreamMixedFrame), 1, streamed));
        CHECK(decoder.Value().TryFinish() && Same(streamed, View(mixed)));

        // 블록 checksum 손상은 InvalidArgument다(마지막 블록 checksum: endmark·content checksum 앞
        // 4byte).
        Array<uint8> damaged(allocator);
        REQUIRE(damaged.TryAppend(Bytes(UpstreamMixedFrame)));
        damaged[damaged.Size() - 9] ^= 0x01;
        CHECK(FailsEverywhere(View(damaged), ErrorCode::InvalidArgument, MixedContentBytes));

        // 사전 ID를 기록한 frame은 사전이 필요하므로 Unsupported다(zstd 사전 ID frame·zlib FDICT와
        // 같다).
        CHECK(FailsEverywhere(Bytes(DictionaryIdFrame), ErrorCode::Unsupported));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(Lz4Frame_ConcatenatedAndSkippableFrames) {
    SystemAllocator allocator;
    {
        Array<uint8> first(allocator);
        Array<uint8> second(allocator);
        Fill(first, 70000, Pattern::Text, 61);
        Fill(second, 3000, Pattern::Random, 62);
        Array<uint8> expected(allocator);
        REQUIRE(expected.TryAppend(View(first)));
        REQUIRE(expected.TryAppend(View(second)));

        // skippable frame(magic 0x184D2A5X, LE32 크기, 내용)은 앞·사이·뒤 어디에 있어도 건너뛴다.
        static constexpr uint8 Skippable[] = {0x5a, 0x2a, 0x4d, 0x18, 0x03, 0x00,
                                              0x00, 0x00, 1,    2,    3};
        static constexpr uint8 EmptySkippable[] = {0x50, 0x2a, 0x4d, 0x18, 0x00, 0x00, 0x00, 0x00};
        Array<uint8> joined(allocator);
        REQUIRE(joined.TryAppend(Bytes(Skippable)));
        REQUIRE(TryCompressAppend(Settings(Frame, 1), View(first), joined));
        REQUIRE(joined.TryAppend(Bytes(EmptySkippable)));
        REQUIRE(TryCompressAppend(Settings(Frame, 9), View(second), joined));
        REQUIRE(joined.TryAppend(Bytes(Skippable)));
        CHECK(DecodesTo(View(joined), View(expected)));
        CHECK(StreamDecodesTo(View(joined), View(expected)));

        // 출력 상한은 모든 frame의 합이다.
        Array<uint8> limited(allocator);
        CHECK(TryDecompressAppend(Frame, View(joined), expected.Size() - 1, limited).Error() ==
              ErrorCode::CapacityExceeded);
        CHECK(limited.Size() == 0);

        // skippable frame만 있으면 출력 없이 성공한다. 빈 입력의 frame도 출력 0이다.
        CHECK(DecodesTo(Bytes(Skippable), Span<const uint8>()));
        CHECK(StreamDecodesTo(Bytes(EmptySkippable), Span<const uint8>()));
        Array<uint8> empty(allocator);
        REQUIRE(TryCompressAppend(DefaultSettings(Frame), Span<const uint8>(), empty));
        CHECK(empty.Size() == 15);
        CHECK(DecodesTo(View(empty), Span<const uint8>()));
        Array<uint8> none(allocator);
        CHECK(TryDecompressAppend(Frame, View(empty), 0, none) && none.Size() == 0);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(Lz4Frame_CorruptionTruncationAndTrailingBytes) {
    SystemAllocator allocator;
    {
        Array<uint8> original(allocator);
        Fill(original, 5000, Pattern::Text, 71);
        Array<uint8> packed(allocator);
        REQUIRE(TryCompressAppend(DefaultSettings(Frame), View(original), packed));

        // 모든 길이로 자른 입력은 실패한다(endmark·content checksum까지 받아야 frame이 끝난다).
        for (usize length = 1; length < packed.Size(); ++length) {
            Array<uint8> output(allocator);
            const ErrorCode error = ErrorOf(TryDecompressAppend(
                Frame, Slice(View(packed), 0, length), original.Size(), output));
            CHECK(error == ErrorCode::InvalidArgument);
            CHECK(output.Size() == 0);
        }
        CHECK(
            FailsEverywhere(Slice(View(packed), 0, packed.Size() - 1), ErrorCode::InvalidArgument));

        // frame 뒤의 남는 byte: 쓰레기, 0, magic 앞부분만 있는 다음 frame은 모두 손상이다.
        static constexpr uint8 Junk[] = {'!'};
        static constexpr uint8 Zeros[] = {0, 0, 0, 0, 0, 0, 0, 0};
        static constexpr uint8 MagicPrefix[] = {0x04, 0x22};
        static constexpr uint8 SkippablePrefix[] = {0x50, 0x2a, 0x4d, 0x18, 0x09};
        const Span<const uint8> Trailers[] = {
            Bytes(Junk),  Slice(Bytes(Zeros), 0, 1), Slice(Bytes(Zeros), 0, 4),
            Bytes(Zeros), Bytes(MagicPrefix),        Bytes(SkippablePrefix)};
        for (Span<const uint8> trailer : Trailers) {
            Array<uint8> trailing(allocator);
            REQUIRE(trailing.TryAppend(View(packed)));
            REQUIRE(trailing.TryAppend(trailer));
            CHECK(FailsEverywhere(View(trailing), ErrorCode::InvalidArgument));
        }

        // content checksum·헤더 checksum·magic·예약 bit 손상.
        const usize Positions[] = {packed.Size() - 1, packed.Size() - 4, 14, 0, 5};
        for (usize position : Positions) {
            Array<uint8> damaged(allocator);
            REQUIRE(damaged.TryAppend(View(packed)));
            damaged[position] ^= 0x02;
            CHECK(FailsEverywhere(View(damaged), ErrorCode::InvalidArgument));
        }

        // 고정 seed bit flip: content checksum이 있으므로 실패하거나, 성공하면 원본과 같다(의미
        // 없는 bit).
        uint64 state = 0x2545f4914f6cdd1dull;
        for (uint32 trial = 0; trial < 600; ++trial) {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            Array<uint8> damaged(allocator);
            REQUIRE(damaged.TryAppend(View(packed)));
            const usize position =
                trial < 256 ? trial % packed.Size() : static_cast<usize>(state % packed.Size());
            damaged[position] = static_cast<uint8>(damaged[position] ^ (1u << ((state >> 40) % 8)));
            Array<uint8> output(allocator);
            auto decoded = TryDecompressAppend(Frame, View(damaged), original.Size() * 2, output);
            if (decoded) {
                CHECK(Same(output, View(original)));
            }
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 원본 크기 필드는 frame 끝에서 확인한다. 헤더 checksum을 다시 맞춘 거짓 크기도 InvalidArgument다.
TEST(Lz4Frame_ContentSizeAndChecksumVerified) {
    SystemAllocator allocator;
    {
        CHECK(FailsEverywhere(Bytes(LyingSmallerFrame), ErrorCode::InvalidArgument));
        CHECK(FailsEverywhere(Bytes(LyingLargerFrame), ErrorCode::InvalidArgument));
        // 원본 크기 필드는 출력을 미리 잡는 힌트일 뿐이다. Span 출력이 거짓 크기(66byte)뿐이어도
        // 남은 1byte와 frame 끝의 크기 검사가 같은 LZ4F 호출에서 나오므로
        // 손상(InvalidArgument)이다.
        uint8 tight[66];
        CHECK(TryDecompress(Frame, Bytes(LyingSmallerFrame), Span<uint8>(tight, sizeof(tight)),
                            allocator)
                  .Error() == ErrorCode::InvalidArgument);

        // 2^40byte를 선언한 3byte frame: 먼저 확보하는 출력은 입력에 비례해 작고, frame 끝에서
        // 거부한다.
        CountingAllocator counting(allocator);
        {
            Array<uint8> output(counting);
            CHECK(TryDecompressAppend(Frame, Bytes(HugeDeclaredFrame), usize{1} << 30, output)
                      .Error() == ErrorCode::InvalidArgument);
            CHECK(output.Size() == 0);
        }
        CHECK(counting.Peak() < (usize{1} << 20));
        CHECK(counting.Current() == 0);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 상한은 해제 결과와 작업 메모리 모두를 묶는다(압축 폭탄).
TEST(Lz4Frame_DecompressionBombsStayBounded) {
    SystemAllocator system;
    CountingAllocator counting(system);
    constexpr usize Limit = usize{1} << 20;
    {
        Array<uint8> zeros(system);
        Fill(zeros, usize{16} << 20, Pattern::Zeros);
        Array<uint8> packed(system);
        REQUIRE(TryCompressAppend(Settings(Frame, 9), View(zeros), packed));
        CHECK(packed.Size() < zeros.Size() / 200);

        // 일회성: 원본 크기(16MiB)를 기록했지만 상한(1MiB)까지만 먼저 확보한다.
        counting.ResetPeak();
        {
            Array<uint8> output(counting);
            CHECK(TryDecompressAppend(Frame, View(packed), Limit, output).Error() ==
                  ErrorCode::CapacityExceeded);
            CHECK(output.Size() == 0);
        }
        CHECK(counting.Peak() < Limit * 2);

        // 스트림: 출력 누계 상한 + 해제 문맥(약 256KiB)이다.
        counting.ResetPeak();
        {
            auto decoder = DecompressStream::TryCreate(Frame, Limits(Limit), counting);
            REQUIRE(decoder);
            Array<uint8> streamed(counting);
            CHECK(decoder.Value().TryWriteAppend(View(packed), streamed).Error() ==
                  ErrorCode::CapacityExceeded);
            CHECK(streamed.Size() == 0);
        }
        CHECK(counting.Peak() < Limit * 2);
    }
    CHECK(counting.Current() == 0);
}

// 분할 루프의 취소 지점: 압축은 입력 1MiB마다, 해제는 입력 소비·출력 생성 1MiB마다 확인하며
// 취소되면 Append 출력 길이를 호출 전 길이로 되돌린다.
TEST(Lz4Frame_CancellationProbeStopsAtChunks) {
    SystemAllocator allocator;
    {
        Array<uint8> original(allocator);
        Fill(original, (usize{1} << 20) * 3 + 7, Pattern::Text, 21);
        Array<uint8> packed(allocator);
        REQUIRE(TryCompressAppend(DefaultSettings(Frame), View(original), packed));
        static constexpr uint8 Base[] = {'k', 'e', 'e', 'p'};
        for (uint32 checks = 0; checks < 3; ++checks) {
            CountdownProbe countdown{checks};
            detail::CompressionProbe probe{&CountdownProbe::Check, &countdown};
            Array<uint8> compressed(allocator);
            REQUIRE(compressed.TryAppend(Bytes(Base)));
            CHECK(detail::CompressAppendOnce(Frame, DefaultSettings(Frame).Level(), View(original),
                                             compressed, allocator, probe)
                      .Error() == ErrorCode::Cancelled);
            CHECK(Same(compressed, Bytes(Base)));
            CHECK(countdown.checks == checks + 1);

            countdown = CountdownProbe{checks};
            Array<uint8> restored(allocator);
            REQUIRE(restored.TryAppend(Bytes(Base)));
            CHECK(detail::DecompressAppendOnce(Frame, View(packed), original.Size(), restored,
                                               allocator, probe)
                      .Error() == ErrorCode::Cancelled);
            CHECK(Same(restored, Bytes(Base)));

            countdown = CountdownProbe{checks};
            Array<uint8> fixed(allocator);
            REQUIRE(fixed.TryResize(original.Size()));
            CHECK(detail::DecompressIntoOnce(Frame, View(packed),
                                             Span<uint8>(fixed.Data(), fixed.Size()), allocator,
                                             probe)
                      .Error() == ErrorCode::Cancelled);
        }

        // 취소되지 않은 probe의 확인 횟수: 압축은 조각 4개 + 끝 1번, 해제는 출력 1MiB마다다.
        CountdownProbe untouched{1000};
        detail::CompressionProbe probe{&CountdownProbe::Check, &untouched};
        Array<uint8> again(allocator);
        REQUIRE(detail::CompressAppendOnce(Frame, DefaultSettings(Frame).Level(), View(original),
                                           again, allocator, probe));
        CHECK(untouched.checks == 5);
        CHECK(Same(again, View(packed)));

        // 해제는 출력 생성 1MiB마다도 확인한다: 압축률이 높은 입력(4MiB의 0, 압축본 수십 KiB)도
        // 출력 1MiB마다 확인한다(입력 기준만이면 한두 번이다).
        Array<uint8> zeros(allocator);
        Fill(zeros, usize{4} << 20, Pattern::Zeros);
        Array<uint8> packedZeros(allocator);
        REQUIRE(TryCompressAppend(DefaultSettings(Frame), View(zeros), packedZeros));
        REQUIRE(packedZeros.Size() < (usize{1} << 20));
        CountdownProbe counted{1000};
        detail::CompressionProbe countedProbe{&CountdownProbe::Check, &counted};
        Array<uint8> restored(allocator);
        REQUIRE(detail::DecompressAppendOnce(Frame, View(packedZeros), zeros.Size(), restored,
                                             allocator, countedProbe));
        CHECK(counted.checks >= 4);
        CHECK(Same(restored, View(zeros)));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 비동기 작업은 동기 API와 같은 byte를 만들고 같은 결과로 푼다.
TEST(Lz4Frame_AsyncMatchesSync) {
    SystemAllocator allocator;
    {
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({.workerCount = 4, .maxInFlight = 64}));
        {
            constexpr uint32 Jobs = 6;
            static constexpr usize Sizes[Jobs] = {0, 1, 5000, 70000, (usize{2} << 20) + 3, 300000};
            static constexpr int32 Levels[Jobs] = {1, 2, 9, 1, 1, 12};
            Array<uint8> originals[Jobs] = {Array<uint8>(allocator), Array<uint8>(allocator),
                                            Array<uint8>(allocator), Array<uint8>(allocator),
                                            Array<uint8>(allocator), Array<uint8>(allocator)};
            Array<uint8> packed[Jobs] = {Array<uint8>(allocator), Array<uint8>(allocator),
                                         Array<uint8>(allocator), Array<uint8>(allocator),
                                         Array<uint8>(allocator), Array<uint8>(allocator)};
            Array<uint8> restored[Jobs] = {Array<uint8>(allocator), Array<uint8>(allocator),
                                           Array<uint8>(allocator), Array<uint8>(allocator),
                                           Array<uint8>(allocator), Array<uint8>(allocator)};
            Array<uint8> fixed[Jobs] = {Array<uint8>(allocator), Array<uint8>(allocator),
                                        Array<uint8>(allocator), Array<uint8>(allocator),
                                        Array<uint8>(allocator), Array<uint8>(allocator)};
            CompressionTask compressTasks[Jobs];
            CompressionTask decompressTasks[Jobs];
            CompressionTask fixedTasks[Jobs];
            for (uint32 job = 0; job < Jobs; ++job) {
                Fill(originals[job], Sizes[job], job % 2 == 0 ? Pattern::Text : Pattern::Random,
                     job + 11);
                REQUIRE(fixed[job].TryResize(Sizes[job]));
                REQUIRE(TrySubmitCompress(scheduler, compressTasks[job],
                                          Settings(Frame, Levels[job]), View(originals[job]),
                                          packed[job]));
            }
            for (uint32 job = 0; job < Jobs; ++job) {
                auto bytes = compressTasks[job].TryWaitResult();
                REQUIRE(bytes);
                Array<uint8> expected(allocator);
                REQUIRE(TryCompressAppend(Settings(Frame, Levels[job]), View(originals[job]),
                                          expected));
                CHECK(Same(packed[job], View(expected)));

                const Span<const WorkerTaskHandle> after(&compressTasks[job].Handle(), 1);
                REQUIRE(TrySubmitDecompress(scheduler, decompressTasks[job], Frame,
                                            View(packed[job]), Sizes[job], restored[job], after));
                REQUIRE(TrySubmitDecompress(scheduler, fixedTasks[job], Frame, View(packed[job]),
                                            Span<uint8>(fixed[job].Data(), fixed[job].Size()),
                                            allocator, after));
            }
            for (uint32 job = 0; job < Jobs; ++job) {
                auto bytes = decompressTasks[job].TryWaitResult();
                CHECK(bytes && bytes.Value() == Sizes[job] &&
                      Same(restored[job], View(originals[job])));
                auto fixedBytes = fixedTasks[job].TryWaitResult();
                CHECK(fixedBytes && fixedBytes.Value() == Sizes[job] &&
                      Same(fixed[job], View(originals[job])));
            }
            // 빈 해제 입력은 제출 시점에 거부한다.
            CompressionTask rejected;
            Array<uint8> output(allocator);
            CHECK(TrySubmitDecompress(scheduler, rejected, Frame, Span<const uint8>(), 1, output)
                      .Error() == ErrorCode::InvalidArgument);
        }
        REQUIRE(scheduler.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 모든 할당 시도 위치에서 실패시킨다: OutOfMemory, 누수 0, output 길이 복원(단일 스레드, 계수기는
// 원자적이지 않다).
TEST(Lz4Frame_OutOfMemoryAtEveryAllocationRollsBack) {
    SystemAllocator system;
    Array<uint8> original(system);
    Fill(original, 150000, Pattern::Text, 3);
    Array<uint8> packed(system);
    REQUIRE(TryCompressAppend(Settings(Frame, 9), View(original), packed));
    NFailAllocator failing(system);
    static constexpr int32 Levels[] = {1, 9};
    for (int32 level : Levels) {
        bool succeeded = false;
        for (uint64 attempt = 1; attempt <= 64 && !succeeded; ++attempt) {
            {
                Array<uint8> output(failing);
                failing.DisableFailure();
                REQUIRE(output.TryAppend(Text("base")));
                failing.FailOnAttempt(attempt);
                auto result = TryCompressAppend(Settings(Frame, level), View(original), output);
                succeeded = result.IsOk();
                CHECK(!succeeded || failing.Attempts() < attempt);
                CHECK(succeeded || result.Error() == ErrorCode::OutOfMemory);
                CHECK(succeeded || output.Size() == 4);
            }
            CHECK(system.Stats().liveAllocations == 2); // original·packed만 남는다.
        }
        CHECK(succeeded);
    }

    bool succeeded = false;
    for (uint64 attempt = 1; attempt <= 64 && !succeeded; ++attempt) {
        {
            Array<uint8> output(failing);
            failing.DisableFailure();
            REQUIRE(output.TryAppend(Text("base")));
            failing.FailOnAttempt(attempt);
            auto result = TryDecompressAppend(Frame, View(packed), original.Size(), output);
            succeeded = result.IsOk();
            CHECK(!succeeded || failing.Attempts() < attempt);
            CHECK(succeeded || result.Error() == ErrorCode::OutOfMemory);
            CHECK(succeeded ? Same(Slice(View(output), 4, output.Size() - 4), View(original))
                            : output.Size() == 4);
        }
        CHECK(system.Stats().liveAllocations == 2);
    }
    CHECK(succeeded);

    // Span 해제: 작업 메모리(문맥·블록 버퍼)만 할당한다.
    succeeded = false;
    Array<uint8> fixed(system);
    REQUIRE(fixed.TryResize(original.Size()));
    for (uint64 attempt = 1; attempt <= 16 && !succeeded; ++attempt) {
        failing.FailOnAttempt(attempt);
        auto result =
            TryDecompress(Frame, View(packed), Span<uint8>(fixed.Data(), fixed.Size()), failing);
        succeeded = result.IsOk();
        CHECK(!succeeded || failing.Attempts() < attempt);
        CHECK(succeeded || result.Error() == ErrorCode::OutOfMemory);
        CHECK(system.Stats().liveAllocations == 3);
    }
    CHECK(succeeded && Same(fixed, View(original)));

    // Append 성장 경로: 원본 크기가 없는 스트림 frame(64KiB를 먼저 잡고 두 배씩 늘린다) 뒤에 일회성
    // frame을 이어 붙인 입력이다. 예비 창 뒤의 성장 실패도 OutOfMemory이고 길이를 되돌린다.
    auto stream = CompressStream::TryCreate(DefaultSettings(Frame), system);
    REQUIRE(stream);
    Array<uint8> joined(system);
    REQUIRE(CompressInChunks(stream.Value(), View(original), 65536, joined));
    REQUIRE(joined.TryAppend(View(packed)));
    const uint64 baseline = system.Stats().liveAllocations;
    succeeded = false;
    uint64 failures = 0;
    for (uint64 attempt = 1; attempt <= 64 && !succeeded; ++attempt) {
        {
            Array<uint8> output(failing);
            failing.DisableFailure();
            REQUIRE(output.TryAppend(Text("base")));
            failing.FailOnAttempt(attempt);
            auto result = TryDecompressAppend(Frame, View(joined), original.Size() * 2, output);
            succeeded = result.IsOk();
            failures += succeeded ? 0 : 1;
            CHECK(!succeeded || failing.Attempts() < attempt);
            CHECK(succeeded || result.Error() == ErrorCode::OutOfMemory);
            CHECK(succeeded ? output.Size() == 4 + original.Size() * 2 : output.Size() == 4);
        }
        CHECK(system.Stats().liveAllocations == baseline);
    }
    CHECK(succeeded);
    // 문맥·상태·첫 확보·블록 버퍼 2개 뒤에 성장 실패 지점이 더 있다.
    CHECK(failures > 5);
}

// 모든 코덱 메모리는 주입한 할당기를 거친다. 문맥을 만든 뒤 첫 frame에서 블록 버퍼를 확보하면 같은
// 설정의 다음 압축·해제는 할당하지 않는다. 메모리 크기는 Docs/Design/Compression.ko.md 12절의
// 값이다.
TEST(Lz4Frame_AllocationsGoThroughInjectedAllocator) {
    SystemAllocator system;
    CountingAllocator counting(system);
    Array<uint8> original(system);
    Fill(original, 200000, Pattern::Text, 5);
    const usize bound = TryGetCompressBound(Frame, original.Size()).Value();
    Array<uint8> packed(system);
    REQUIRE(packed.TryResize(bound));
    Array<uint8> restored(system);
    REQUIRE(restored.TryResize(original.Size()));

    static constexpr int32 Levels[] = {1, 9};
    for (int32 level : Levels) {
        const uint64 before = counting.Allocations();
        auto written = TryCompress(Settings(Frame, level), View(original),
                                   Span<uint8>(packed.Data(), packed.Size()), counting);
        REQUIRE(written);
        CHECK(counting.Allocations() > before);
        CHECK(counting.Current() == 0);

        auto compressor = Compressor::TryCreate(Settings(Frame, level), counting);
        REQUIRE(compressor);
        REQUIRE(compressor.Value().TryCompress(View(original),
                                               Span<uint8>(packed.Data(), packed.Size())));
        const uint64 warmed = counting.Allocations();
        for (uint32 round = 0; round < 3; ++round) {
            CHECK(compressor.Value().TryCompress(View(original),
                                                 Span<uint8>(packed.Data(), packed.Size())));
        }
        CHECK(counting.Allocations() == warmed);
        // 일회성 문맥: 수준 1 약 145KiB, HC 약 450KiB(블록 버퍼 128KiB + LZ4 상태).
        CHECK(counting.Current() < (level == 1 ? usize{160} << 10 : usize{470} << 10));
    }
    CHECK(counting.Current() == 0);

    const usize packedBytes = TryCompress(DefaultSettings(Frame), View(original),
                                          Span<uint8>(packed.Data(), packed.Size()), system)
                                  .Value();
    {
        auto decompressor = Decompressor::TryCreate(Frame, counting);
        REQUIRE(decompressor);
        const Span<const uint8> frame = Slice(View(packed), 0, packedBytes);
        REQUIRE(decompressor.Value().TryDecompress(frame,
                                                   Span<uint8>(restored.Data(), restored.Size())));
        const uint64 warmed = counting.Allocations();
        CHECK(decompressor.Value().TryDecompress(frame,
                                                 Span<uint8>(restored.Data(), restored.Size())));
        CHECK(counting.Allocations() == warmed);
        CHECK(Same(restored, View(original)));
        // 해제 문맥: 64KiB 블록이면 약 256KiB.
        CHECK(counting.Current() < usize{270} << 10);
    }

    // 스트림 문맥: 수준 1 약 270KiB, HC 약 580KiB(블록 버퍼 192KiB, 작은 창용 64KiB 포함).
    for (int32 level : Levels) {
        auto stream = CompressStream::TryCreate(Settings(Frame, level), counting);
        REQUIRE(stream);
        Array<uint8> streamed(system);
        REQUIRE(CompressInChunks(stream.Value(), View(original), 4096, streamed));
        CHECK(counting.Current() < (level == 1 ? usize{280} << 10 : usize{590} << 10));
    }
    CHECK(counting.Current() == 0);
}

TEST(Lz4Frame_StreamRoundTripAcrossChunkings) {
    SystemAllocator allocator;
    {
        static constexpr usize Sizes[] = {0, 1, 1000, (usize{1} << 20) + 5};
        static constexpr usize Chunks[] = {1, 7, 4096, usize{1} << 20};
        static constexpr Pattern Patterns[] = {Pattern::Text, Pattern::Random};
        for (usize size : Sizes) {
            for (Pattern pattern : Patterns) {
                Array<uint8> original(allocator);
                Fill(original, size, pattern, size + 1);
                Array<uint8> reference(allocator);
                for (usize chunk : Chunks) {
                    // 1byte 조각은 작은 입력에서만 잰다(시간).
                    if (chunk < 4096 && size > 4096) {
                        continue;
                    }
                    auto stream = CompressStream::TryCreate(DefaultSettings(Frame), allocator);
                    REQUIRE(stream);
                    Array<uint8> packed(allocator);
                    REQUIRE(CompressInChunks(stream.Value(), View(original), chunk, packed));
                    CHECK(StreamDecodesTo(View(packed), View(original)));
                    // 스트림 frame은 원본 크기를 기록하지 않는다. 결과는 입력 조각 나누기와
                    // 무관하다.
                    CHECK((packed[4] & 0x08) == 0);
                    if (reference.Size() == 0) {
                        REQUIRE(reference.TryAppend(View(packed)));
                    }
                    CHECK(Same(packed, View(reference)));

                    // 해제 조각 나누기와 무관하게 같은 결과다.
                    auto decoder = DecompressStream::TryCreate(Frame, Limits(size + 1), allocator);
                    REQUIRE(decoder);
                    Array<uint8> restored(allocator);
                    CHECK(DecompressInChunks(decoder.Value(), View(packed), chunk, restored));
                    CHECK(decoder.Value().TryFinish());
                    CHECK(Same(restored, View(original)));
                }

                // 일회성 압축 결과도 스트림으로 해제한다.
                Array<uint8> packedOnce(allocator);
                REQUIRE(TryCompressAppend(DefaultSettings(Frame), View(original), packedOnce));
                CHECK(StreamDecodesTo(View(packedOnce), View(original)));
            }
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 스트림 결과는 입력 내용과 flush 위치로만 정해진다: 입력 조각·출력 창 크기와 무관하다. 1.5MiB
// golden은 upstream에 1byte–1MiB 조각으로 넣은 결과와 같다(probe 확인).
TEST(Lz4Frame_StreamOutputDependsOnlyOnContentAndFlushPoints) {
    SystemAllocator allocator;
    {
        struct Golden {
            int32 level;
            usize size;
            uint32 crc;
        };
        static constexpr Golden Goldens[] = {{1, 1561736, 0xb444525eu}, {9, 1162580, 0x585c1b22u}};
        Array<uint8> original(allocator);
        // CompressionStreamTests 결정성 golden과 같은 입력·호출 순서(4099byte 조각, 절반에서
        // flush)다.
        Fill(original, (usize{3} << 19) + 11, Pattern::Text, 41);
        const usize half = original.Size() / 2;
        for (const Golden& golden : Goldens) {
            auto stream = CompressStream::TryCreate(Settings(Frame, golden.level), allocator);
            REQUIRE(stream);
            Array<uint8> packed(allocator);
            REQUIRE(CompressInChunks(stream.Value(), View(original), 4099, packed, half));
            const uint32 crc = Crc32(View(packed));
            if (packed.Size() != golden.size || crc != golden.crc) {
                printf("lz4 frame stream golden mismatch: level %d size %llu crc 0x%08x\n",
                       golden.level, static_cast<unsigned long long>(packed.Size()), crc);
            }
            CHECK(packed.Size() == golden.size);
            CHECK(crc == golden.crc);
            CHECK(StreamDecodesTo(View(packed), View(original)));

            // 같은 flush 위치면 큰 조각·Reset 뒤 재사용도 같은 byte다.
            stream.Value().Reset();
            Array<uint8> coarse(allocator);
            REQUIRE(CompressInChunks(stream.Value(), View(original), usize{1} << 20, coarse, half));
            CHECK(Same(coarse, View(packed)));
        }

        // 작은 입력에서 출력 창(7byte–70000byte)·입력 조각(1byte–128KiB)을 바꿔도 같은 byte다(1byte
        // 창은 Lz4Frame_StreamBackpressureAndTinyWindows).
        Array<uint8> small(allocator);
        Fill(small, 150000, Pattern::Text, 43);
        auto reference = CompressStream::TryCreate(DefaultSettings(Frame), allocator);
        REQUIRE(reference);
        Array<uint8> expected(allocator);
        REQUIRE(CompressInChunks(reference.Value(), View(small), 65536, expected, 70001));
        struct Shape {
            usize inputChunk;
            usize window;
        };
        static constexpr Shape Shapes[] = {
            {5000, 7}, {65535, 65547}, {131072, 65548}, {333, 70000}, {1, 4096}};
        for (const Shape& shape : Shapes) {
            auto stream = CompressStream::TryCreate(DefaultSettings(Frame), allocator);
            REQUIRE(stream);
            Array<uint8> packed(allocator);
            REQUIRE(CompressWithBackpressure(stream.Value(), View(small), shape.inputChunk,
                                             shape.window, packed, 70001));
            CHECK(Same(packed, View(expected)));
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 출력 창이 작으면 오류 없이 일부만 진행한다. 1byte 창으로도 헤더·블록·flush·finish가 끝난다.
TEST(Lz4Frame_StreamBackpressureAndTinyWindows) {
    SystemAllocator allocator;
    {
        Array<uint8> original(allocator);
        Fill(original, 140000, Pattern::Text, 11);
        Array<uint8> expected(allocator);
        REQUIRE(TryCompressAppend(DefaultSettings(Frame), View(original), expected));
        auto reference = CompressStream::TryCreate(DefaultSettings(Frame), allocator);
        REQUIRE(reference);
        Array<uint8> streamed(allocator);
        REQUIRE(CompressInChunks(reference.Value(), View(original), 65536, streamed, 70000));
        static constexpr usize Windows[] = {1, 13, 4096};
        for (usize window : Windows) {
            auto stream = CompressStream::TryCreate(DefaultSettings(Frame), allocator);
            REQUIRE(stream);
            Array<uint8> packed(allocator);
            REQUIRE(CompressWithBackpressure(stream.Value(), View(original), 5000, window, packed,
                                             70000));
            // 1byte 창도 같은 flush 위치의 Append 결과와 같은 byte다.
            CHECK(Same(packed, View(streamed)));
            CHECK(StreamDecodesTo(View(packed), View(original)));

            auto decoder = DecompressStream::TryCreate(Frame, Limits(original.Size()), allocator);
            REQUIRE(decoder);
            Array<uint8> restored(allocator);
            CHECK(DecompressWithBackpressure(decoder.Value(), View(packed), 333, window, restored));
            CHECK(decoder.Value().TryFinish());
            CHECK(Same(restored, View(original)));

            // 일회성 frame(원본 크기·autoFlush)도 같은 창으로 풀린다.
            auto onceDecoder =
                DecompressStream::TryCreate(Frame, Limits(original.Size()), allocator);
            REQUIRE(onceDecoder);
            Array<uint8> once(allocator);
            CHECK(DecompressWithBackpressure(onceDecoder.Value(), View(expected), 4099, window,
                                             once));
            CHECK(onceDecoder.Value().TryFinish() && Same(once, View(original)));
        }

        // 8MiB의 0: 압축본 전체를 한 번에 줘도 출력 창(4KiB)만큼만 나온다.
        Array<uint8> zeros(allocator);
        Fill(zeros, usize{8} << 20, Pattern::Zeros);
        Array<uint8> packed(allocator);
        REQUIRE(TryCompressAppend(DefaultSettings(Frame), View(zeros), packed));
        auto decoder = DecompressStream::TryCreate(Frame, Limits(zeros.Size()), allocator);
        REQUIRE(decoder);
        Array<uint8> window(allocator);
        REQUIRE(window.TryResize(4096));
        auto step = decoder.Value().TryProcess(View(packed), Span<uint8>(window.Data(), 4096));
        REQUIRE(step);
        CHECK(step.Value().produced == 4096);
        CHECK(!step.Value().complete);
        CHECK(!decoder.Value().IsAtStreamBoundary());
        Array<uint8> restored(allocator);
        REQUIRE(restored.TryAppend(Span<const uint8>(window.Data(), 4096)));
        const usize consumed = step.Value().consumed;
        CHECK(decoder.Value().TryWriteAppend(
            Slice(View(packed), consumed, packed.Size() - consumed), restored));
        CHECK(decoder.Value().TryFinish());
        CHECK(Same(restored, View(zeros)));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// flush한 지점까지의 byte만으로 그때까지의 입력을 모두 해제할 수 있다(네트워크 메시지 경계).
TEST(Lz4Frame_StreamFlushMakesPrefixDecodable) {
    SystemAllocator allocator;
    {
        const Span<const uint8> first = Text("first message over the lz4 frame stream");
        const Span<const uint8> second = Text("second message, sent after a sync flush");
        static constexpr usize Windows[] = {1, usize{1} << 20};
        for (usize windowBytes : Windows) {
            auto stream = CompressStream::TryCreate(DefaultSettings(Frame), allocator);
            REQUIRE(stream);
            auto decoder = DecompressStream::TryCreate(Frame, Limits(1 << 20), allocator);
            REQUIRE(decoder);
            Array<uint8> window(allocator);
            REQUIRE(window.TryResize(windowBytes));
            const Span<uint8> target(window.Data(), window.Size());
            Array<uint8> packed(allocator);
            Array<uint8> restored(allocator);

            REQUIRE(stream.Value().TryWriteAppend(first, packed));
            REQUIRE(RepeatUntilComplete([&](Span<uint8> to) { return stream.Value().TryFlush(to); },
                                        target, packed));
            // 이미 flush했으면 다시 flush해도 출력 없이 성공한다.
            const usize flushed = packed.Size();
            REQUIRE(stream.Value().TryFlushAppend(packed));
            CHECK(packed.Size() == flushed);
            REQUIRE(decoder.Value().TryWriteAppend(View(packed), restored));
            CHECK(Same(restored, first));
            CHECK(!decoder.Value().IsAtStreamBoundary());
            CHECK(decoder.Value().TryFinish().Error() == ErrorCode::InvalidArgument);

            const usize sent = packed.Size();
            REQUIRE(stream.Value().TryWriteAppend(second, packed));
            REQUIRE(RepeatUntilComplete(
                [&](Span<uint8> to) { return stream.Value().TryFinish(to); }, target, packed));
            REQUIRE(decoder.Value().TryWriteAppend(Slice(View(packed), sent, packed.Size() - sent),
                                                   restored));
            CHECK(decoder.Value().IsAtStreamBoundary());
            CHECK(decoder.Value().TryFinish());
            CHECK(restored.Size() == first.Size() + second.Size());
            CHECK(Same(Slice(View(restored), first.Size(), second.Size()), second));
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 미완료 flush·finish 중에는 같은 요청만 받고, 거부는 상태를 바꾸지 않는다.
TEST(Lz4Frame_StreamPhaseRules) {
    SystemAllocator allocator;
    {
        Array<uint8> original(allocator);
        Fill(original, 20000, Pattern::Random, 17);
        uint8 one[1] = {};
        const Span<uint8> tiny(one, 1);
        auto created = CompressStream::TryCreate(DefaultSettings(Frame), allocator);
        REQUIRE(created);
        CompressStream& stream = created.Value();
        Array<uint8> packed(allocator);

        // 출력 공간이 0이면 진행하지 않고 단계도 그대로다.
        auto empty = stream.TryFlush(Span<uint8>());
        CHECK(empty && empty.Value().produced == 0 && !empty.Value().complete);
        auto nothing = stream.TryProcess(Span<const uint8>(), Span<uint8>());
        CHECK(nothing && nothing.Value().complete);
        auto blocked = stream.TryProcess(View(original), Span<uint8>());
        CHECK(blocked && blocked.Value().consumed == 0 && !blocked.Value().complete);

        REQUIRE(stream.TryWriteAppend(View(original), packed));
        auto flushing = stream.TryFlush(tiny);
        REQUIRE(flushing);
        CHECK(!flushing.Value().complete);
        REQUIRE(packed.TryAppend(Span<const uint8>(one, flushing.Value().produced)));
        CHECK(stream.TryProcess(View(original), tiny).Error() == ErrorCode::InvalidArgument);
        CHECK(stream.TryFinish(tiny).Error() == ErrorCode::InvalidArgument);
        CHECK(stream.TryFinishAppend(packed).Error() == ErrorCode::InvalidArgument);
        REQUIRE(stream.TryFlushAppend(packed));

        auto finishing = stream.TryFinish(tiny);
        REQUIRE(finishing);
        CHECK(!finishing.Value().complete);
        REQUIRE(packed.TryAppend(Span<const uint8>(one, finishing.Value().produced)));
        CHECK(stream.TryProcess(View(original), tiny).Error() == ErrorCode::InvalidArgument);
        CHECK(stream.TryFlush(tiny).Error() == ErrorCode::InvalidArgument);
        REQUIRE(stream.TryFinishAppend(packed));
        // 거부된 TryProcess는 입력을 소비하지 않았다.
        CHECK(StreamDecodesTo(View(packed), View(original)));

        // 끝난 뒤: Finish는 빈 완료, Process·Flush는 거부다.
        auto again = stream.TryFinish(tiny);
        CHECK(again && again.Value().complete && again.Value().produced == 0);
        CHECK(stream.TryFinishAppend(packed).Value() == 0);
        CHECK(stream.TryProcess(View(original), tiny).Error() == ErrorCode::InvalidArgument);
        CHECK(stream.TryWriteAppend(View(original), packed).Error() == ErrorCode::InvalidArgument);
        CHECK(stream.TryFlushAppend(packed).Error() == ErrorCode::InvalidArgument);

        // Reset 뒤(단계 버퍼에 출력이 남은 채 Reset해도) 같은 호출 순서는 새 객체와 같은 byte다.
        stream.Reset();
        REQUIRE(stream.TryProcess(View(original), tiny));
        stream.Reset();
        Array<uint8> reused(allocator);
        REQUIRE(CompressInChunks(stream, View(original), 3000, reused));
        auto fresh = CompressStream::TryCreate(DefaultSettings(Frame), allocator);
        REQUIRE(fresh);
        Array<uint8> expected(allocator);
        REQUIRE(CompressInChunks(fresh.Value(), View(original), 3000, expected));
        CHECK(Same(reused, View(expected)));
        CHECK(StreamDecodesTo(View(reused), View(original)));

        // 아무 입력 없이 끝낸 스트림은 빈 frame(헤더 7 + endmark·checksum 8)이다.
        auto blank = CompressStream::TryCreate(DefaultSettings(Frame), allocator);
        REQUIRE(blank);
        Array<uint8> blankFrame(allocator);
        REQUIRE(blank.Value().TryFinishAppend(blankFrame));
        CHECK(blankFrame.Size() == 15);
        CHECK(StreamDecodesTo(View(blankFrame), Span<const uint8>()));

        // 이동된 객체는 StaleHandle이다.
        CompressStream moved = Move(stream);
        CHECK(stream.TryFinishAppend(packed).Error() == ErrorCode::StaleHandle);
        CHECK(moved.TryFinishAppend(packed));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 상한은 Create·Reset 이후 모든 frame 출력의 누계이며 정확히 상한이면 성공한다. 경계·절단·frame 뒤
// byte.
TEST(Lz4Frame_StreamLimitBoundariesAndTrailingBytes) {
    SystemAllocator allocator;
    {
        Array<uint8> original(allocator);
        Fill(original, 300000, Pattern::Text, 29);
        const usize size = original.Size();
        Array<uint8> packed(allocator);
        REQUIRE(TryCompressAppend(DefaultSettings(Frame), View(original), packed));

        auto exact = DecompressStream::TryCreate(Frame, Limits(size), allocator);
        REQUIRE(exact);
        Array<uint8> restored(allocator);
        CHECK(exact.Value().TryWriteAppend(View(packed), restored));
        CHECK(exact.Value().TryFinish());

        auto tight = DecompressStream::TryCreate(Frame, Limits(size - 1), allocator);
        REQUIRE(tight);
        Array<uint8> output(allocator);
        REQUIRE(output.TryAppend(Text("keep")));
        CHECK(tight.Value().TryWriteAppend(View(packed), output).Error() ==
              ErrorCode::CapacityExceeded);
        CHECK(output.Size() == 4);
        // sticky: Reset 전까지 같은 오류다.
        CHECK(tight.Value().TryWriteAppend(Span<const uint8>(), output).Error() ==
              ErrorCode::CapacityExceeded);
        CHECK(tight.Value().TryFinish().Error() == ErrorCode::CapacityExceeded);
        CHECK(!tight.Value().IsAtStreamBoundary());
        // 큰 출력 창을 주는 TryProcess도 상한에서 멈춘다.
        tight.Value().Reset();
        Array<uint8> large(allocator);
        REQUIRE(large.TryResize(size * 2));
        CHECK(tight.Value()
                  .TryProcess(View(packed), Span<uint8>(large.Data(), large.Size()))
                  .Error() == ErrorCode::CapacityExceeded);
        tight.Value().Reset();
        CHECK(tight.Value().TryWriteAppend(Slice(View(packed), 0, 10), output));

        // 두 번째 frame은 누계로 센다. Reset은 누계를 지운다.
        Array<uint8> twice(allocator);
        REQUIRE(twice.TryAppend(View(packed)));
        REQUIRE(twice.TryAppend(View(packed)));
        auto both = DecompressStream::TryCreate(Frame, Limits(size * 2), allocator);
        auto short2 = DecompressStream::TryCreate(Frame, Limits(size * 2 - 1), allocator);
        REQUIRE(both && short2);
        Array<uint8> joined(allocator);
        CHECK(both.Value().TryWriteAppend(View(twice), joined));
        CHECK(both.Value().TryFinish() && joined.Size() == size * 2);
        Array<uint8> clipped(allocator);
        CHECK(short2.Value().TryWriteAppend(View(twice), clipped).Error() ==
              ErrorCode::CapacityExceeded);
        both.Value().Reset();
        joined.Clear();
        CHECK(both.Value().TryWriteAppend(View(twice), joined) && joined.Size() == size * 2);

        // 마지막 byte 전까지는 절단이다. 마지막 byte로 경계에 닿는다.
        auto decoder = DecompressStream::TryCreate(Frame, Limits(1 << 20), allocator);
        REQUIRE(decoder);
        CHECK(!decoder.Value().IsAtStreamBoundary());
        CHECK(decoder.Value().TryFinish().Error() == ErrorCode::InvalidArgument);
        Array<uint8> streamed(allocator);
        REQUIRE(
            decoder.Value().TryWriteAppend(Slice(View(packed), 0, packed.Size() - 1), streamed));
        CHECK(!decoder.Value().IsAtStreamBoundary());
        CHECK(decoder.Value().TryFinish().Error() == ErrorCode::InvalidArgument);
        REQUIRE(
            decoder.Value().TryWriteAppend(Slice(View(packed), packed.Size() - 1, 1), streamed));
        CHECK(decoder.Value().IsAtStreamBoundary());
        CHECK(decoder.Value().TryFinish());
        CHECK(Same(streamed, View(original)));

        // frame 뒤 쓰레기는 받은 호출에서 바로 거부한다. magic 앞부분만 오면 다음 frame을
        // 기다린다(경계가 아니다).
        static constexpr uint8 Junk[] = {'x', 'y', 'z'};
        CHECK(decoder.Value().TryWriteAppend(Bytes(Junk), streamed).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(decoder.Value().TryFinish().Error() == ErrorCode::InvalidArgument);
        decoder.Value().Reset();
        Array<uint8> next(allocator);
        static constexpr uint8 MagicPrefix[] = {0x04, 0x22, 0x4d};
        REQUIRE(decoder.Value().TryWriteAppend(View(packed), next));
        REQUIRE(decoder.Value().TryWriteAppend(Bytes(MagicPrefix), next));
        CHECK(!decoder.Value().IsAtStreamBoundary());
        CHECK(decoder.Value().TryFinish().Error() == ErrorCode::InvalidArgument);

        // skippable frame을 1byte씩 넣어도 출력 없이 경계다(skippable만 받아도 Finish는 성공한다).
        static constexpr uint8 Skippable[] = {0x5f, 0x2a, 0x4d, 0x18, 0x03, 0x00,
                                              0x00, 0x00, 1,    2,    3};
        auto skipper = DecompressStream::TryCreate(Frame, Limits(1 << 20), allocator);
        REQUIRE(skipper);
        Array<uint8> skipped(allocator);
        REQUIRE(DecompressInChunks(skipper.Value(), Bytes(Skippable), 1, skipped));
        CHECK(skipped.Size() == 0 && skipper.Value().IsAtStreamBoundary() &&
              skipper.Value().TryFinish());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 코덱·출력 성장 할당 실패는 sticky이며 Append 길이를 되돌린다. Reset 뒤에는 다시 성공한다.
TEST(Lz4Frame_StreamOutOfMemoryIsStickyAndRecoverable) {
    SystemAllocator system;
    Array<uint8> original(system);
    Fill(original, 150000, Pattern::Text, 37);
    Array<uint8> packed(system);
    REQUIRE(TryCompressAppend(DefaultSettings(Frame), View(original), packed));
    NFailAllocator failing(system);

    // 생성 실패는 누수 없이 OutOfMemory다.
    bool created = false;
    for (uint64 attempt = 1; attempt <= 16 && !created; ++attempt) {
        failing.FailOnAttempt(attempt);
        auto stream = CompressStream::TryCreate(DefaultSettings(Frame), failing);
        auto decoder = DecompressStream::TryCreate(Frame, Limits(1 << 20), failing);
        created = stream && decoder;
        CHECK(created || (stream ? decoder.Error() : stream.Error()) == ErrorCode::OutOfMemory);
    }
    CHECK(created);
    CHECK(system.Stats().liveAllocations == 2);

    bool succeeded = false;
    for (uint64 attempt = 1; attempt <= 64 && !succeeded; ++attempt) {
        {
            failing.DisableFailure();
            auto stream = CompressStream::TryCreate(DefaultSettings(Frame), failing);
            REQUIRE(stream);
            Array<uint8> output(failing);
            REQUIRE(output.TryAppend(Text("base")));
            failing.FailOnAttempt(attempt);
            auto written = stream.Value().TryWriteAppend(View(original), output);
            const usize afterWrite = output.Size();
            auto finished = written ? stream.Value().TryFinishAppend(output)
                                    : Result<usize>::Failure(written.Error());
            succeeded = finished.IsOk();
            CHECK(!succeeded || failing.Attempts() < attempt);
            if (!succeeded) {
                CHECK(finished.Error() == ErrorCode::OutOfMemory);
                CHECK(output.Size() == (written ? afterWrite : 4));
                CHECK(stream.Value().TryFinishAppend(output).Error() == ErrorCode::OutOfMemory);
                failing.DisableFailure();
                stream.Value().Reset();
                REQUIRE(output.TryResize(4));
                REQUIRE(stream.Value().TryWriteAppend(View(original), output));
                REQUIRE(stream.Value().TryFinishAppend(output));
            }
            CHECK(StreamDecodesTo(Slice(View(output), 4, output.Size() - 4), View(original)));
        }
        CHECK(system.Stats().liveAllocations == 2);
    }
    CHECK(succeeded);

    succeeded = false;
    for (uint64 attempt = 1; attempt <= 64 && !succeeded; ++attempt) {
        {
            failing.DisableFailure();
            auto decoder = DecompressStream::TryCreate(Frame, Limits(1 << 20), failing);
            REQUIRE(decoder);
            Array<uint8> output(failing);
            REQUIRE(output.TryAppend(Text("base")));
            failing.FailOnAttempt(attempt);
            auto written = decoder.Value().TryWriteAppend(View(packed), output);
            succeeded = written.IsOk();
            CHECK(!succeeded || failing.Attempts() < attempt);
            if (!succeeded) {
                CHECK(written.Error() == ErrorCode::OutOfMemory);
                CHECK(output.Size() == 4);
                CHECK(decoder.Value().TryFinish().Error() == ErrorCode::OutOfMemory);
                failing.DisableFailure();
                decoder.Value().Reset();
                REQUIRE(decoder.Value().TryWriteAppend(View(packed), output));
            }
            CHECK(decoder.Value().TryFinish());
            CHECK(Same(Slice(View(output), 4, output.Size() - 4), View(original)));
        }
        CHECK(system.Stats().liveAllocations == 2);
    }
    CHECK(succeeded);
}

// frame 헤더는 사전 ID를 보기 전에 LZ4F가 검증한다: FLG가 손상된 frame은 사전 frame이 아니라
// 손상이다. legacy frame(lz4 -l)은 아는 형식이지만 지원하지 않는다.
TEST(Lz4Frame_HeaderValidatedBeforeDictionaryCheck) {
    SystemAllocator allocator;
    {
        // 일회성 frame(FLG 0x4C)·스트림 frame(FLG 0x44)의 사전 ID bit를 켜면 헤더 크기와 헤더
        // checksum이 맞지 않는다.
        Array<uint8> once(allocator);
        REQUIRE(TryCompressAppend(DefaultSettings(Frame), Text(GoldenText), once));
        once[4] ^= 0x01;
        CHECK(FailsEverywhere(View(once), ErrorCode::InvalidArgument));
        auto stream = CompressStream::TryCreate(DefaultSettings(Frame), allocator);
        REQUIRE(stream);
        Array<uint8> streamed(allocator);
        REQUIRE(CompressInChunks(stream.Value(), Text(GoldenText), 16, streamed));
        REQUIRE(streamed[4] == 0x44);
        streamed[4] ^= 0x01;
        CHECK(FailsEverywhere(View(streamed), ErrorCode::InvalidArgument));

        // version bit가 00인 FLG(사전 ID bit만 켬)도 손상이다.
        static constexpr uint8 BadVersion[] = {0x04, 0x22, 0x4d, 0x18, 0x01, 0x40,
                                               0x07, 0x00, 0x00, 0x00, 0x00, 0x00};
        CHECK(FailsEverywhere(Bytes(BadVersion), ErrorCode::InvalidArgument));

        // 헤더 checksum까지 맞는 사전 ID frame은 그대로 Unsupported다.
        CHECK(FailsEverywhere(Bytes(DictionaryIdFrame), ErrorCode::Unsupported));

        // legacy frame magic(0x184C2102)은 Unsupported다. 정상 frame 뒤에 와도 같다.
        static constexpr uint8 Legacy[] = {0x02, 0x21, 0x4c, 0x18, 0x06, 0x00, 0x00,
                                           0x00, 0x50, 'h',  'e',  'l',  'l',  'o'};
        CHECK(FailsEverywhere(Bytes(Legacy), ErrorCode::Unsupported));
        Array<uint8> followed(allocator);
        REQUIRE(followed.TryAppend(Bytes(AdapterLevel1Frame)));
        REQUIRE(followed.TryAppend(Bytes(Legacy)));
        CHECK(FailsEverywhere(View(followed), ErrorCode::Unsupported));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 스트림은 모든 수준(lz4mid·HC·optimal parser 포함)에서 결과가 입력 조각 나누기와 무관하다.
TEST(Lz4Frame_StreamLevelsAreChunkIndependent) {
    SystemAllocator allocator;
    {
        Array<uint8> original(allocator);
        Fill(original, 200000, Pattern::Text, 59);
        static constexpr int32 Levels[] = {2, 9, 12};
        static constexpr usize Chunks[] = {4099, 65536, usize{1} << 20};
        for (int32 level : Levels) {
            Array<uint8> reference(allocator);
            for (usize chunk : Chunks) {
                auto stream = CompressStream::TryCreate(Settings(Frame, level), allocator);
                REQUIRE(stream);
                Array<uint8> packed(allocator);
                REQUIRE(CompressInChunks(stream.Value(), View(original), chunk, packed, 100000));
                if (reference.Size() == 0) {
                    REQUIRE(reference.TryAppend(View(packed)));
                    CHECK(StreamDecodesTo(View(packed), View(original)));
                }
                CHECK(Same(packed, View(reference)));
            }
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 분할 단위(1MiB)를 넘는 창, 상한에 정확히 닿는 TryProcess, 상한 직후의 빈 frame·1byte frame, 여러
// 번 늘린 뒤의 길이 복원, 상한 SIZE_MAX, 이동 대입.
TEST(Lz4Frame_StreamLargeWindowsAndLimitEdges) {
    SystemAllocator allocator;
    {
        Array<uint8> original(allocator);
        Fill(original, (usize{3} << 20) + 5, Pattern::Text, 47);
        const usize size = original.Size();
        Array<uint8> window(allocator);
        REQUIRE(window.TryResize(size + 1));
        const Span<uint8> big(window.Data(), window.Size());

        // 압축: 큰 창 하나로 Process·Flush·Finish가 각각 한 번에 끝난다.
        auto stream = CompressStream::TryCreate(DefaultSettings(Frame), allocator);
        REQUIRE(stream);
        Array<uint8> packed(allocator);
        auto processed = stream.Value().TryProcess(View(original), big);
        REQUIRE(processed);
        CHECK(processed.Value().complete && processed.Value().consumed == size);
        REQUIRE(packed.TryAppend(Span<const uint8>(window.Data(), processed.Value().produced)));
        auto flushed = stream.Value().TryFlush(big);
        REQUIRE(flushed && flushed.Value().complete);
        REQUIRE(packed.TryAppend(Span<const uint8>(window.Data(), flushed.Value().produced)));
        auto finished = stream.Value().TryFinish(big);
        REQUIRE(finished && finished.Value().complete);
        REQUIRE(packed.TryAppend(Span<const uint8>(window.Data(), finished.Value().produced)));
        CHECK(StreamDecodesTo(View(packed), View(original)));

        // 해제: 허용량보다 큰 창으로 정확히 상한에 닿으면 성공이다.
        auto exact = DecompressStream::TryCreate(Frame, Limits(size), allocator);
        REQUIRE(exact);
        auto decoded = exact.Value().TryProcess(View(packed), big);
        REQUIRE(decoded);
        CHECK(decoded.Value().consumed == packed.Size() && decoded.Value().complete);
        CHECK(decoded.Value().produced == size);
        CHECK(Same(Span<const uint8>(window.Data(), size), View(original)));
        CHECK(exact.Value().TryFinish());

        // 상한에 닿은 뒤 빈 frame은 성공(헤더 검증도 1byte 창으로 진행), 1byte라도 더 나오면
        // CapacityExceeded다.
        Array<uint8> empty(allocator);
        REQUIRE(TryCompressAppend(DefaultSettings(Frame), Span<const uint8>(), empty));
        Array<uint8> tail(allocator);
        CHECK(exact.Value().TryWriteAppend(View(empty), tail));
        CHECK(tail.Size() == 0 && exact.Value().TryFinish());
        Array<uint8> one(allocator);
        REQUIRE(TryCompressAppend(DefaultSettings(Frame), Text("x"), one));
        CHECK(exact.Value().TryWriteAppend(View(one), tail).Error() == ErrorCode::CapacityExceeded);
        CHECK(tail.Size() == 0);

        // 1MiB를 여러 번 늘린 뒤 상한 초과로 실패해도 길이를 되돌린다.
        auto tight = DecompressStream::TryCreate(Frame, Limits(size - 1), allocator);
        REQUIRE(tight);
        Array<uint8> output(allocator);
        REQUIRE(output.TryAppend(Text("keep")));
        CHECK(tight.Value().TryWriteAppend(View(packed), output).Error() ==
              ErrorCode::CapacityExceeded);
        CHECK(output.Size() == 4 && output.Capacity() >= size - 1);

        // 상한 SIZE_MAX도 overflow 없이 동작한다.
        auto unlimited =
            DecompressStream::TryCreate(Frame, Limits(static_cast<usize>(-1)), allocator);
        REQUIRE(unlimited);
        Array<uint8> restored(allocator);
        CHECK(unlimited.Value().TryWriteAppend(View(packed), restored));
        CHECK(unlimited.Value().TryFinish() && Same(restored, View(original)));

        // 이동 대입은 대상의 상태를 해제하고 원본을 넘겨받는다.
        DecompressStream assigned;
        assigned = Move(unlimited.Value());
        CHECK(assigned.TryFinish());
        assigned = Move(exact.Value());
        CHECK(!exact.Value().IsAtStreamBoundary());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 판정이 출력 창 크기에 따라 달라지지 않는다: 창이 정확히 원본 크기면 한 호출로 frame 끝까지 가고
// 경계다. 입력·출력 겹침은 상태를 바꾸지 않고 거부하며, 이동된 해제 스트림은 StaleHandle이다.
TEST(Lz4Frame_StreamWindowOutcomeAndHandleContracts) {
    SystemAllocator allocator;
    {
        Array<uint8> original(allocator);
        Fill(original, 5000, Pattern::Text, 53);
        Array<uint8> window(allocator);
        REQUIRE(window.TryResize(original.Size()));
        Array<uint8> once(allocator);
        REQUIRE(TryCompressAppend(DefaultSettings(Frame), View(original), once));
        auto stream = CompressStream::TryCreate(DefaultSettings(Frame), allocator);
        REQUIRE(stream);
        Array<uint8> streamed(allocator);
        REQUIRE(CompressInChunks(stream.Value(), View(original), 1000, streamed));
        const Span<const uint8> Frames[] = {View(once), View(streamed)};
        for (Span<const uint8> frame : Frames) {
            auto decoder = DecompressStream::TryCreate(Frame, Limits(1 << 20), allocator);
            REQUIRE(decoder);
            auto step =
                decoder.Value().TryProcess(frame, Span<uint8>(window.Data(), window.Size()));
            REQUIRE(step);
            CHECK(step.Value().consumed == frame.Size() && step.Value().complete);
            CHECK(step.Value().produced == original.Size());
            CHECK(decoder.Value().IsAtStreamBoundary());
            CHECK(Same(window, View(original)));
        }

        // 입력·출력이 겹치면 거부하고 상태는 그대로다.
        auto compressor = CompressStream::TryCreate(DefaultSettings(Frame), allocator);
        REQUIRE(compressor);
        Array<uint8> buffer(allocator);
        REQUIRE(buffer.TryAppend(View(original)));
        CHECK(compressor.Value()
                  .TryProcess(View(buffer), Span<uint8>(buffer.Data() + 10, 100))
                  .Error() == ErrorCode::InvalidArgument);
        CHECK(compressor.Value().TryWriteAppend(Slice(View(buffer), 0, 10), buffer).Error() ==
              ErrorCode::InvalidArgument);
        Array<uint8> packed(allocator);
        REQUIRE(CompressInChunks(compressor.Value(), View(original), 1000, packed));
        CHECK(Same(packed, View(streamed)));

        auto decoder = DecompressStream::TryCreate(Frame, Limits(1 << 20), allocator);
        REQUIRE(decoder);
        CHECK(decoder.Value().TryWriteAppend(Slice(View(packed), 0, 4), packed).Error() ==
              ErrorCode::InvalidArgument);
        Array<uint8> restored(allocator);
        REQUIRE(decoder.Value().TryWriteAppend(View(packed), restored));
        CHECK(decoder.Value().TryFinish() && Same(restored, View(original)));

        // 이동된 해제 스트림은 StaleHandle이고 Reset은 아무것도 하지 않는다.
        DecompressStream taken = Move(decoder.Value());
        CHECK(decoder.Value().TryWriteAppend(View(packed), restored).Error() ==
              ErrorCode::StaleHandle);
        CHECK(decoder.Value().TryFinish().Error() == ErrorCode::StaleHandle);
        CHECK(!decoder.Value().IsAtStreamBoundary());
        decoder.Value().Reset();
        CHECK(taken.TryFinish());
        DecompressStream emptyDecoder;
        CHECK(emptyDecoder.TryFinish().Error() == ErrorCode::StaleHandle);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace rocks::test
