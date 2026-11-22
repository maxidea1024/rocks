#pragma once

// ARMv8.4 FEAT_DIT(Data Independent Timing)를 켜는 범위다. DIT를 켜면 Arm ARM이 정한 명령(정수
// 산술·논리, load·store 등)의 시간이 데이터 값에 따라 달라지지 않고, Apple M3에서는 데이터 메모리
// 의존 prefetcher(DMP)도 꺼진다(GoFetch, 2024. M4 이후는 공개 확인이 없다). 암호 모듈이 비밀을
// 다루는 구간에서만 켠다(Docs/Design/Crypto.ko.md 6.3절, 결정 D12).
//
// - PSTATE.DIT는 스레드 상태다(문맥 전환 때 보존된다). 범위 객체는 만든 스레드에서만 효과가 있다.
// - 켠 직후 speculation barrier(FEAT_SB가 있으면 SB, 없으면 DSB NSH·ISB)를 둔다(Apple 지침).
// - 지금은 macOS arm64에서만 감지한다(CpuFeatures::armDit). 다른 대상에서는 아무 일도 하지 않는다.
// - Apple M1·M2는 DIT를 켜도 DMP가 꺼지지 않는다(끌 방법이 없는 한계).
//
// 사용 예:
//   {
//       platform::DataIndependentTimingScope dit;   // FEAT_DIT가 있고 꺼져 있으면 켠다
//       RunCryptoOperation();
//   }                                               // 이 범위가 켰을 때만 끈다
namespace rocks::platform {

// 지금 스레드의 DIT가 켜져 있는가. FEAT_DIT가 없으면 false다.
[[nodiscard]] bool IsDataIndependentTimingEnabled() noexcept;

// FEAT_DIT가 있고 꺼져 있으면 켜고 true를 돌려준다. true일 때만 EndDataIndependentTiming으로 끈다.
[[nodiscard]] ROCKS_PLATFORMBASE_API bool BeginDataIndependentTiming() noexcept;
ROCKS_PLATFORMBASE_API void EndDataIndependentTiming() noexcept;

// Begin·End를 묶은 범위 객체다. 겹쳐 만들어도 바깥 범위가 끝날 때까지 켜져 있다.
class DataIndependentTimingScope {
public:
    DataIndependentTimingScope() noexcept : enabled_(BeginDataIndependentTiming()) {
    }

    ~DataIndependentTimingScope() noexcept {
        if (enabled_) {
            EndDataIndependentTiming();
        }
    }

    DataIndependentTimingScope(const DataIndependentTimingScope&) = delete;
    DataIndependentTimingScope& operator=(const DataIndependentTimingScope&) = delete;

private:
    bool enabled_;
};

} // namespace rocks::platform
