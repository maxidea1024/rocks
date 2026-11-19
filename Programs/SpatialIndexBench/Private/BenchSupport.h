#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Base/NumericLimits.h>
#include <Platform/Clock.h>

// 벤치 공용 부품이다(Docs/Design/SpatialIndex.ko.md 9.5절, SIP8). 공용 벤치 하니스가 없으므로 이
// 프로그램 안에서 통계(K회 묶음 단위 분위수)와 JSON 출력을 만든다. 엔진 바이너리·시험 실행 파일에는
// 들어가지 않는다.
namespace rocks::spatialbench {

// 모든 비교군이 같은 구현을 쓰는 할당기다(9.1절 공통 조건). 플랫폼 정렬 할당(OS/CRT 힙)에 그대로
// 넘기고 요청 크기 기준의 계수만 더한다. Core SystemAllocator는 살아 있는 할당의 선형 목록이라
// 노드별 할당이 많은 비교군(B)을 부당하게 느리게 만들므로 쓰지 않는다. 단일 스레드 전용이다.
class BenchAllocator final : public Allocator {
public:
    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override;
    Result<void> Deallocate(void* address) noexcept override;

    // 계수를 0으로 되돌린다(살아 있는 byte는 유지하고 피크를 현재 값으로 맞춘다).
    void ResetCounters() noexcept;

    uint64 allocations = 0;
    uint64 deallocations = 0;
    uint64 liveBytes = 0;
    uint64 peakBytes = 0;
};

// 연산 시간 표본이다. 표본 하나는 연산 K개 묶음의 시간을 K로 나눈 값(ns/연산)이다(9.5절: 시계 호출
// 비용과 해상도가 수십 ns 연산을 지배하지 않게 한다).
struct SampleSummary {
    uint32 samples = 0;
    uint32 batch = 0;
    uint64 operations = 0;
    double totalNanoseconds = 0.0;
    double median = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;
    double mean = 0.0;
};

class SampleSet {
public:
    explicit SampleSet(Allocator& allocator) noexcept : values_(allocator) {
    }

    // 연산 batch개를 elapsed ns에 처리한 묶음 하나를 더한다.
    bool Add(uint64 elapsedNanoseconds, uint32 batch) noexcept;
    void Clear() noexcept;
    // 분위수는 정렬한 표본의 최근접 순위(ceil(p · n) 번째)다. 표본이 없으면 모두 0이다.
    SampleSummary Summarize() noexcept;

private:
    Array<double> values_;
    uint32 batch_ = 0;
    uint64 operations_ = 0;
    double total_ = 0.0;
};

// 구간 시간을 잰다.
class Stopwatch {
public:
    void Start() noexcept {
        start_ = platform::MonotonicNanoseconds();
    }

    uint64 Elapsed() const noexcept {
        return platform::MonotonicNanoseconds() - start_;
    }

private:
    uint64 start_ = 0;
};

// 질의 결과를 미리 예약한 버퍼에 식별 값으로 기록한다(9.1절 공통 결과 처리). 넘치는 결과는 세기만
// 한다. checksum은 식별 값의 합이다.
struct ResultBuffer {
    uint32* ids = nullptr;
    uint32 capacity = 0;
    uint32 count = 0;
    uint64 checksum = 0;

    void Reset() noexcept {
        count = 0;
        checksum = 0;
    }

    void Push(uint32 id) noexcept {
        if (count < capacity) {
            ids[count] = id;
        }
        ++count;
        checksum += id;
    }
};

// 계수 모드에서만 채우는 질의 계수다(9.3절 방문·검사·후보 수). 타이밍 경로는 Counting=false 템플릿
// 인스턴스라 계수 코드가 컴파일 시점에 빠진다(비교군 A·B·C). D는 QueryStats 포인터를 nullptr로
// 넘긴다(분기는 남는다).
struct QueryCounters {
    uint64 visitedNodes = 0;
    uint64 testedProxies = 0;
    uint64 containedProxies = 0;
    uint64 results = 0;
};

// 작은 JSON 작성기다. 키와 문자열은 ASCII만 받는다(이스케이프는 따옴표와 역슬래시만).
class JsonWriter {
public:
    explicit JsonWriter(Allocator& allocator) noexcept : text_(allocator) {
    }

    void BeginObject() noexcept;
    void EndObject() noexcept;
    void BeginArray() noexcept;
    void EndArray() noexcept;
    void Key(const char* key) noexcept;
    void String(const char* value) noexcept;
    void Number(double value) noexcept;
    void Integer(uint64 value) noexcept;
    void SignedInteger(int64 value) noexcept;
    void Bool(bool value) noexcept;
    void Null() noexcept;

    void Field(const char* key, const char* value) noexcept {
        Key(key);
        String(value);
    }

    void Field(const char* key, double value) noexcept {
        Key(key);
        Number(value);
    }

    void FieldInteger(const char* key, uint64 value) noexcept {
        Key(key);
        Integer(value);
    }

    void FieldBool(const char* key, bool value) noexcept {
        Key(key);
        Bool(value);
    }

    void Summary(const char* key, const SampleSummary& summary) noexcept;

    bool Failed() const noexcept {
        return failed_;
    }

    bool WriteToFile(const char* path) noexcept;

private:
    void Separator() noexcept;
    void Append(const char* text) noexcept;
    void AppendChar(char value) noexcept;

    Array<char> text_;
    // 깊이별로 첫 원소인지다(쉼표 결정).
    bool first_[64] = {};
    uint32 depth_ = 0;
    bool afterKey_ = false;
    bool failed_ = false;
};

// 고정 시드 해시(splitmix64)다. 필터 mask처럼 식별 값에서 결정적으로 유도하는 값에 쓴다.
constexpr uint64 Mix64(uint64 value) noexcept {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}

} // namespace rocks::spatialbench
