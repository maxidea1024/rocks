#pragma once

#include "ObjectRegistryInternal.h"

#include <Core/Diagnostics/Log.h>
#include <Platform/Atomics.h>

// ObjectRegistry GC 파일들이 함께 쓰는 상수와 작은 helper다(모듈 내부 — ObjectRegistryGc*.cpp만
// include한다).
namespace rocks::registry_detail {

// 직렬 marker가 시계를 읽는 간격(trace한 객체 수)이다. 시간 예산은 soft deadline이라 이 간격만큼
// 넘을 수 있다.
constexpr uint32 TimeCheckInterval = 128;
// sweep이 시계를 읽는 간격(chunk 수)이다.
constexpr uint32 SweepTimeCheckInterval = 64;
// 작업 큐에서 몇 칸 앞의 hot 레코드·객체 본체를 미리 읽는지다(3.4절 모델의 16·8칸). 참조 배열
// 버퍼는 본체 배치를 알아야 하므로(M3) 미리 읽지 않는다.
constexpr usize HotPrefetchDistance = 16;
constexpr usize ObjectPrefetchDistance = 8;
// 참조 위치 표(M3)의 배열 칸 버퍼를 몇 칸 앞에서 미리 읽는지다(본체는 8칸 앞에서 이미 당겼다).
constexpr usize ArrayPrefetchDistance = 4;
// 병렬 slice의 꼬리 시간 보정이 시간 예산에서 가져갈 수 있는 최대 몫(예산 / 이 값)이다. 선점 같은
// 일회성 지연이 다음 slice의 표식 시간을 지나치게 줄이지 않게 한다.
constexpr uint64 ParallelTailBudgetDivisor = 4;
// GC 자료 할당 태그다.
constexpr uint32 GcPageTag = 0x47435047u;      // 'GCPG'
constexpr uint32 GcPageTableTag = 0x47435054u; // 'GCPT'
constexpr uint32 GcTypeTag = 0x47435459u;      // 'GCTY'
constexpr uint32 RootListTag = 0x4743524cu;    // 'GCRL'
constexpr uint32 GcBlockTag = 0x4743424bu;     // 'GCBK'

// GC 사이클 통계 로그 카테고리다(Debug 레벨 — 객체마다 찍지 않는다).
inline constexpr LogCategory LogObjectGC{"Object.GC"};

inline void LogCycle(const char* kind, const GcReport& report) noexcept {
    Log::Debug(LogObjectGC,
               "{Kind} finished: examined {Examined}, marked {Marked}, retired {Retired}, "
               "reclaimed {Reclaimed}, {Milliseconds:.3f} ms",
               kind, report.examined, report.marked, report.retired, report.reclaimed,
               static_cast<double>(report.durationNanoseconds) * 1e-6);
}

inline uint64 LoadMark(const uint64& word) noexcept {
    return platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&word);
}

inline void StoreMark(uint64& word, uint64 value) noexcept {
    platform::AtomicStore<platform::MemoryOrder::Relaxed>(&word, value);
}

inline uint64 BitOf(uint32 index) noexcept {
    return uint64(1) << (index % SlotsPerChunk);
}

// 타입 키 주소의 색인 위치다(주소의 낮은 비트는 정렬로 늘 같으므로 섞는다).
inline uint32 GcTypeBucket(const void* key, uint32 capacity) noexcept {
    const uint64 bits = static_cast<uint64>(reinterpret_cast<uptr>(key));
    return static_cast<uint32>(((bits >> 4) * 0x9e3779b97f4a7c15ULL) >> 32) & (capacity - 1);
}

} // namespace rocks::registry_detail
