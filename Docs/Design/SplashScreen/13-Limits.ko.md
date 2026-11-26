# 시작 화면(스플래시) — 13. 한도 상수

[목차](README.ko.md)

## 13. 한도 상수

| 이름 | 값 | 근거 |
|---|---|---|
| `kSplashPollInterval` | 0.1 s | 문구 갱신으로 충분, ProgressReporting `kChildControlPollInterval`과 같음 |
| `kSplashAnimationInterval` | 1/60 s | 보간·불확정 표시 동안만 |
| `kSplashBarEaseSeconds` | 0.15 s | 튀지 않되 늦어 보이지 않음 |
| `kSplashBarSettleEpsilon` | 0.001 | 막대 1 px 미만 |
| `kSplashMarqueePeriod` | 1.2 s | 불확정 표시 한 바퀴 |
| `kMaxSplashImageBytes` | 32 MiB | 비정상 파일 차단 |
| `kMaxSplashImageDimension` | 4096 px | 4K 모니터 2x 여유 |
| `kMinSplashLogicalDimension` | 160 px | 글자 띠가 들어갈 최소 |
| `kMaxSplashLineBytes` | 512 B | 한 줄 문구 |
| `kMaxCustomSplashTexts` | 6 | UE `User1–6` |
| `kSplashThreadStackBytes` | 256 KiB | WIC·GDI 호출 여유(UE 128 KiB 예약) |
| `kStartupProfileBlend` | 0.5 | 지난 값과 새 값의 반반 |
| `kStartupStepWeightFloor` | 0.02 | 단계 최소 몫 |
