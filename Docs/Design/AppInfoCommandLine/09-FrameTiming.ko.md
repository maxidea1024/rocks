# 앱 정보와 명령줄 — 9. 프레임 시간 `FrameTiming`

[목차](README.ko.md)

## 9. 프레임 시간 `FrameTiming`

### 9.1 값

```cpp
namespace rocks {

// 한 프레임의 시간 값이다. 모든 시간은 double 초다(UE FAppTime과 같은 단위·형식).
// 엔진 RunFrame이 프레임마다 게임 lane에서 새로 만든다. 만든 뒤 바꾸지 않는다.
struct FrameTiming {
    uint64 frameIndex = 0;              // 1부터. 0은 첫 프레임 전이다
    double currentSeconds = 0.0;        // 이번 프레임의 논리 시각(단조 시계 초). 고정 step이면 이전 값 + 고정 delta
    double lastSeconds = 0.0;           // 이전 프레임의 currentSeconds
    double deltaSeconds = 0.0;          // 이번 프레임 delta
    double idleSeconds = 0.0;           // 프레임 상한을 맞추려고 잔 시간
    double idleOvershootSeconds = 0.0;  // 요청보다 더 잔 시간(0 이상)
    double accumulatedSeconds = 0.0;    // deltaSeconds 누적. pause·배율이 없다(World 시간은 GameClock)
    double averageFrameSeconds = 0.0;   // 최근 프레임 지수 이동 평균
    Optional<double> fixedDeltaSeconds; // 고정 step을 쓰는 프레임이면 그 길이(RunTraits::UsesFixedTimeStep)
};

// 아무 스레드나 읽는 마지막 프레임 번호다(relaxed). 시간 값이 필요한 다른 스레드는
// platform::MonotonicNanoseconds()를 쓴다.
uint64 LatestFrameIndex() noexcept;

} // namespace rocks
```

| Rocks 칸 | UE 칸(`AT` 66–95) | 차이 |
|---|---|---|
| `frameIndex` | (`GFrameCounter`, FAppTime 밖) | 한 값에 묶는다 |
| `currentSeconds`·`lastSeconds`·`deltaSeconds` | `CurrentTime`·`LastTime`·`DeltaTime` | 같다. 실제 시간 모드의 원천은 `platform::MonotonicNanoseconds()`다(UE `FPlatformTime::Seconds()`) |
| `idleSeconds`·`idleOvershootSeconds` | `IdleTime`·`IdleTimeOvershoot` | 같다. Rocks 엔진에는 아직 프레임 상한 대기가 없어 0이다 |
| `accumulatedSeconds` | `GameTime` | 이름만 다르다. 지금 `EngineTime::realTimeSeconds`가 이 값이다. 고정 step이면 실제 시간이 아니므로 이름을 바꾼다 |
| `fixedDeltaSeconds` | `bUseFixedTimeStep` + `FixedDeltaTime` | 두 칸을 Optional 하나로 묶는다 |
| `averageFrameSeconds` | (없음) | Rocks에 이미 있다 |
| (나중) | `CurrentFrameTime`(timecode) | timecode provider가 생길 때 `Optional<…>` 칸을 더한다. CV2의 `FrameRate`·`FrameTime`을 쓴다 |

- 지금의 `EngineTime`을 `FrameTiming`으로 대체한다. `Engine::Time()`은 `Engine::Timing()`이 된다(하위호환 없음).
- **고정 step delta는 double 그대로다(D5).** UE는 고정 delta를 float 지역 변수로 거쳐 넣는다(`UEN` 3019). `-FPS=`의 `1 / FixedFPS`도 float 나눗셈이다(`LEL` 4730). 그래서 UE의 1/30은 float로 반올림된 값이다. 단위·형식(double 초)과 기본값(1/30)은 UE와 같고, Rocks는 중간 float 반올림만 따르지 않는다. 이 차이는 D5에 결정으로 적었다. 결정적 실행의 비트 결과가 UE와 같아야 하는 기능(UE 데이터와 비트 대조)이 생기면 다시 본다.
- **음수 delta 처리 없음.** 단조 시계(`uint64` 차)라 음수가 없다. UE의 "음수면 0.01"(`UEN` 3160–3164)은 필요 없다.
- **엔진 delta에 상한이 없다.** UE 단독 게임의 `MaxDeltaTime` 상한(`UEN` 3184–3196)은 Rocks에서 World `GameClock`의 `maxFrameDeltaSeconds`가 맡는다([엔진 런타임](../EngineRuntime/05-Time.ko.md) 5절). 엔진 프레임 값은 측정값 그대로다.

### 9.2 스레드별 읽기

| 읽는 스레드 | 읽는 법 | 보는 값 |
|---|---|---|
| 게임 lane | `Engine::Timing()` | 이번 프레임 값 |
| 렌더 lane | 프레임 packet(RenderBase `FramePacket`) 안 `FrameTiming` 사본 | 그 packet을 만든 프레임의 값 |
| worker 작업 | 작업 문맥이 물려받은 값. CVar 설계가 계획한 "Task 생성 때 만든 lane의 묶음을 작업에 넘긴다" 장치를 그대로 쓴다. 게임 lane이 프레임마다 게시하는 refcount 묶음(CVar snapshot과 같은 묶음)에 그 프레임의 `FrameTiming`을 넣고, 작업에는 묶음 포인터 하나만 넘긴다. 작업마다 `FrameTiming`을 복사하지 않는다 | 작업을 만든 프레임의 값 |
| 그 밖(로그 출력 스레드, trace writer, 플랫폼 콜백) | `LatestFrameIndex()`(relaxed), 시각은 `platform::MonotonicNanoseconds()` | 대략값 |

- 엔진 밖 스레드가 "현재 프레임 시간"을 정확히 읽는 전역 API는 두지 않는다. UE도 그 경우를 데이터 경쟁으로 본다(`AT` 98–103 주석).
- `LatestFrameIndex()`는 엔진이 2단계에서 relaxed 저장한다. 지금 `Log::SetFrame`이 따로 쥔 프레임 번호는 이 값을 읽도록 나중에 정리할 수 있다. 필수는 아니다.
- worker 상속 장치가 생기기 전(AP3 전)에는 worker가 `FrameTiming`을 읽을 길이 없다. 필요한 작업은 제출할 때 값을 인자로 넘긴다.
