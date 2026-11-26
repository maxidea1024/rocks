# 시작 화면(스플래시) — 7. 진행 연결

[목차](README.ko.md)

## 7. 진행 연결

### 7.1 당겨 오기

- 스플래시는 상태 원천 `SplashStatusSource{poll, release, context}`를 받아 **소유**한다. `poll`은 스플래시 스레드(Windows)·메인 스레드(macOS)에서 `kSplashPollInterval`(0.1 s)마다 불린다. `release`는 Hide가 스플래시 스레드(macOS는 타이머)를 멈춘 뒤 한 번 불린다. 그래서 원천의 수명은 스플래시가 정하고 호스트 객체의 소멸 순서와 무관하다.
- 진행 중계(`ProgressHub::Snapshot`)는 아무 스레드에서 당길 수 있으므로(ProgressReporting 7.2절) 호스트 스레드가 모듈 시작이나 셰이더 대기로 막혀도 화면이 갱신된다(UE 1.6절의 한계를 고친다). UI 펌프 hook(ProgressReporting 6.3절)은 쓰지 않는다.
- `SplashScreen` 자체는 Progress를 모른다. 원천 구현 `StartupSplashSource`가 중계를 읽는다. 그 context는 시작 작업 id·자기 스냅샷 allocator·사건 cursor·`CatalogSnapshot` 참조만 갖고 `StartupProgress`를 가리키지 않는다(작업이 끝나거나 hub가 내려가도 id 조회는 "없음"일 뿐이다 — ProgressReporting 5.3절). 원천이 없으면(시험·PG2 전) 글자는 `SetText`로만 바뀐다.

### 7.2 시작 작업과 문구 고르기

- 호스트가 Show 직전에 최상위 작업 "Starting <앱>"을 연다(`presentation = Hidden` — 명령줄 보고기에 시작·끝만 남는다). `StartupProgress`가 이 작업을 갖고 호스트 스레드를 Primary로 바인딩한 뒤 가장 바깥 구간(8.1절 단계)을 연다.
- **시작 경로의 진행은 모두 시작 작업의 Primary 구간 안에서 보고한다.**
  - 호스트 스레드에서 도는 일은 중첩 `ProgressScope`로 들어온다. `Engine::Initialize`·`ModuleLoader::StartPhase` 같은 라이브러리 코드가 원래 여는 구간이 그대로 현재 단계를 채운다(8.1절).
  - 다른 스레드에서 도는 일(셰이더 job, 에셋 색인 worker)은 `StartupProgress::MakeStepCounter(total)`로 보고한다. 현재 단계 구간의 구간 계수(ProgressReporting 6.4절 `scope.MakeCounter`)이고 아무 스레드에서 `Add`한다.
  - 시작 작업을 부모로 하는 **하위 작업은 쓰지 않는다.** ProgressReporting 5.5절 3의 하위 작업 합성은 단계 구간 밖의 별도 항이어서(자식이 붙으면 비율이 내려가 멈추고, 끝난 자식은 계속 1로 센다) 단계 가중치와 8.2절 보정이 뜻을 잃는다. 시작 중 다른 최상위 작업은 스플래시에 보이지 않는다.
- `StartupSplashSource::Poll`(스플래시 스레드):
  1. 자기 allocator와 cursor로 `Snapshot`을 당긴다.
  2. **비율** = 시작 작업의 보이는 값 `D`. 시작 작업이 끝났거나 스냅샷에 없으면 1.
  3. **문구** = 시작 작업의 한 줄 문구(ProgressReporting 6.2절 — 가장 안쪽의 비어 있지 않은 구간 문구 → 작업 문구 → 제목). 첫 구간 문구가 나오기 전에는 기본 문구 "Loading…"이다.
  4. 문구는 Show 때 받은 `CatalogSnapshot`으로 `ResolveText`한다(카탈로그가 없으면 원문). 한 줄 최대 `kMaxSplashLineBytes`(512 B, UTF-8 경계에서 자름). 계수를 문구에 보이고 싶은 코드는 구간 문구에 직접 넣는다(구간 계수의 완료 수는 스냅샷에 없다).
- 진행 문구 칸은 `"<percent>%  <문구>"`다. percent는 `floor(D × 100)`이고 시작 작업이 끝나기 전에는 99로 막는다(다 된 것처럼 보이고 멈추는 일을 막는다). UE의 도는 점은 두지 않는다(막대가 움직임을 보인다).

### 7.3 막대

- **보간.** 보이는 막대 값 `B`는 `D`를 따라간다: `B ← B + (D − B)·(1 − e^(−Δt/τ))`, `τ = kSplashBarEaseSeconds`(0.15 s). `D`가 뒤로 가지 않으므로(중계 보장) `B`도 뒤로 가지 않는다. `Δt`는 주입 시계다.
- 따라가는 중에는 `kSplashAnimationInterval`(1/60 s)로 깨어나고, `|D − B|`가 `kSplashBarSettleEpsilon`(0.001) 안이면 `B = D`로 맞추고 다시 0.1 s 간격으로 돌아간다.
- **불확정.** 첫 비율이 보고되기 전(`D`가 아직 0 — 바깥 구간을 열었어도 첫 단계 안쪽 보고가 없으면 0이다)에는 막대에 폭 20%의 조각이 `kSplashMarqueePeriod`(1.2 s)마다 왼쪽→오른쪽으로 지나간다. 원천이 없을 때도 같다. `D`가 처음 0보다 커지면 그 자리부터 보간한다.
- **작업 표시줄(Windows, Studio만).** `ITaskbarList3::SetProgressValue(hwnd, B×1000, 1000)`, 불확정은 `TBPF_INDETERMINATE`, Hide 때 `TBPF_NOPROGRESS`. 작업 표시줄 단추가 생긴 뒤(등록 메시지 `TaskbarButtonCreated`를 받은 뒤)에만 부르고, 그 전 값은 들고 있다가 메시지가 오면 적용한다. 게임 스플래시는 작업 표시줄에 없으므로(9.2절) 하지 않는다. macOS Dock 진행은 후속(17절).
