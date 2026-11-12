#pragma once

#include <Compression/CompressionDictionary.h>

// zstd 사전 학습이다(Docs/Design/Compression.ko.md 11.6절). 쿠커·도구가 표본(같은 종류의 작은
// 데이터 여러 개)으로 사전을 만들고, 결과 byte를 CompressDictionary·DecompressDictionary로 연다.
// 게임 런타임은 링크하지 않는다.
//
// 사용 예(표본을 이어 붙이고 길이 목록을 함께 준다):
//   Array<uint8> samples(allocator);   // 표본 byte를 순서대로 이어 붙인다
//   Array<usize> sizes(allocator);     // 표본마다 길이
//   DictionaryTrainingSettings settings;
//   settings.compressionLevel = 19;    // 사전을 쓸 압축 수준
//   settings.dictionaryId = 40001;     // 사전마다 다른 ID 권장
//   Array<uint8> dictionary(allocator);
//   auto trained = TryTrainCompressionDictionary(samples.View(), sizes.View(), settings,
//   dictionary); auto encoding = CompressDictionary::TryCreate(
//       CompressionSettings::TryMake(CompressionFormat::Zstd, 19).Value(), dictionary.View(),
//       allocator);
//
// 공통 계약:
// - 결과는 zstd 사전 형식(엔트로피 표·반복 offset·내용·사전 ID)이며 output 뒤에 붙인다. 실패하면
//   output 길이를 되돌린다.
// - 결정성: 같은 표본(순서 포함)·설정이면 64bit little-endian 대상에서 같은 byte다(fastCover, 단일
//   스레드). 표본 순서는 일부만 결과에 영향을 준다(빈도는 순서와 무관하다).
// - 할당: 결과 저장은 output의 할당기를 쓰지만, zstd dictBuilder는 내부 작업 메모리를 CRT
//   malloc으로 직접 할당한다(승인된 좁은 예외, 학습 시점에만). 학습 작업 메모리는 대략 10MiB + 4 ×
//   capacity + 압축 수준별 문맥이다(capacity 110KiB에서 수준 3 약 12MiB, 수준 19 약 15MiB).
// - 스레드: 학습은 dictBuilder가 프로세스 전역 상태를 쓰므로 이 모듈 안에서 직렬화한다(동시에
//   불러도 안전하지만 한 번에 하나씩 실행된다). finalize는 전역 상태를 쓰지 않아 직렬화하지 않는다.
//   취소·진행률은 없다(dictBuilder가 제공하지 않는다).
// - 사전 ID: 0이면 내용의 XXH64에서 유도한다(0이 되지 않으며 엔트로피 표·수준은 반영하지 않는다).
//   직접 주는 ID는 [32768, 2^31 − 1]이다(zstd가 예약한 범위 밖). 같은 내용으로 수준만 다른 사전을
//   만들면 ID를 다르게 준다.
// - 오류: 인자 위반 InvalidArgument, output 성장 실패는 그 오류(OutOfMemory·Overflow), dictBuilder
//   할당 실패 OutOfMemory, dictBuilder가 모든 후보에서 실패 DependencyFailed(후보 안의 할당 실패도
//   이렇게 보고될 수 있다).
namespace rocks {

// 직접 줄 수 있는 사전 ID 범위다(zdict.h: 32767 이하와 2^31 이상은 예약).
inline constexpr uint32 MinExplicitDictionaryId = 32768;
inline constexpr uint32 MaxExplicitDictionaryId = 0x7FFFFFFF;

// 학습 결과 사전의 최소 용량(ZDICT_DICTSIZE_MIN)이다.
inline constexpr usize MinTrainingCapacity = 256;

struct DictionaryTrainingSettings {
    // 결과 사전의 최대 크기다(zstd CLI 기본 110KiB). MinTrainingCapacity – MaxDictionaryBytes. 표본
    // 합이 capacity의 10배 이상이면 좋다(적으면 사전이 표본을 그대로 담아 효과가 떨어진다).
    usize capacity = usize{110} << 10;
    // 엔트로피 표 통계를 낼 압축 수준이다. 사전을 쓸 수준과 맞춘다(수준 3으로 만든 사전을 19에서
    // 쓰면 약 8% 나빠진다). 1–22.
    int32 compressionLevel = 3;
    // frame에 기록할 사전 ID다. 0이면 내용에서 유도한다.
    uint32 dictionaryId = 0;
    // fastCover 조각 크기 k다. 0이면 [50, 2000]에서 후보 5개를 시험해 고른다(후보마다 표본을 압축해
    // 평가하므로 그만큼 느리다). 지정하면 dmerSize ≤ k ≤ capacity이고 k − dmerSize + 1 ≤ 65535이다.
    uint32 segmentSize = 0;
    // fastCover d다. 6 또는 8.
    uint32 dmerSize = 8;
};

struct DictionaryTrainingResult {
    usize bytes = 0; // output 뒤에 붙인 사전 크기
    uint32 segmentSize =
        0; // 실제로 쓴 k(탐색했으면 고른 값). 같은 설정으로 다시 학습하면 같은 사전이 나온다
    uint32 dmerSize = 0; // 실제로 쓴 d
};

// 표본으로 사전을 학습한다. samples는 표본을 이어 붙인 byte, sampleSizes는 표본마다 길이다(합이
// samples 크기와 같다). 0byte 표본도 된다(8byte 미만 표본은 빈도에 들어가지 않는다).
// 인자 규칙(어기면 InvalidArgument): 표본 5개 이상·2^32 − 2개 이하, 길이 합 = samples 크기, 8 ≤ 합
// < 2^32 − 1, capacity·압축 수준·사전 ID·k·d 범위(위 설정 설명), samples·sampleSizes가 output
// 저장소와 겹치지 않음.
Result<DictionaryTrainingResult>
TryTrainCompressionDictionary(Span<const uint8> samples, Span<const usize> sampleSizes,
                              const DictionaryTrainingSettings& settings,
                              Array<uint8>& output) noexcept;

struct DictionaryFinalizeSettings {
    // 엔트로피 표 통계를 낼 압축 수준이다(1–22).
    int32 compressionLevel = 3;
    // 0이면 내용에서 유도한다. 직접 주면 [32768, 2^31 − 1].
    uint32 dictionaryId = 0;
};

// 호출자가 정한 내용(content, 사전 끝부분일수록 가까운 데이터로 쓰인다)에 엔트로피 표·반복
// offset·사전 ID를 붙여 zstd 형식 사전을 만든다(ZDICT_finalizeDictionary). 표본은 엔트로피 통계에만
// 쓴다(0개도 된다). 결과는 헤더(최대 256byte) + max(내용, 8)byte이며 내용을 자르지 않는다.
// 인자 규칙(어기면 InvalidArgument): 내용 ≤ MaxDictionaryBytes − 256, 표본 길이 합 = samples 크기 <
// 2^32 − 1, 압축 수준·사전 ID 범위, content·samples·sampleSizes가 output 저장소와 겹치지 않음.
Result<usize> TryFinalizeCompressionDictionary(Span<const uint8> content, Span<const uint8> samples,
                                               Span<const usize> sampleSizes,
                                               const DictionaryFinalizeSettings& settings,
                                               Array<uint8>& output) noexcept;

// 형식 사전(학습 결과 등)의 내용은 그대로 두고 다른 압축 수준·사전 ID로 엔트로피 표를 다시 만든다.
// 한 번 학습한 사전으로 수준별 사전을 만들 때 쓴다(재학습 없이). 같은 내용이면 ID 0(유도)은 같은
// 값이 되므로 수준마다 다른 ID를 준다. 사용 예:
//   DictionaryFinalizeSettings level19;
//   level19.compressionLevel = 19;
//   level19.dictionaryId = 40019;
//   TryRefinalizeCompressionDictionary(trained.View(), samples.View(), sizes.View(), level19, out);
// 오류: dictionary가 zstd 형식 사전이 아니거나 손상·MaxDictionaryBytes 초과면 InvalidArgument, 그
// 밖은 TryFinalizeCompressionDictionary와 같다.
Result<usize> TryRefinalizeCompressionDictionary(Span<const uint8> dictionary,
                                                 Span<const uint8> samples,
                                                 Span<const usize> sampleSizes,
                                                 const DictionaryFinalizeSettings& settings,
                                                 Array<uint8>& output) noexcept;

} // namespace rocks
