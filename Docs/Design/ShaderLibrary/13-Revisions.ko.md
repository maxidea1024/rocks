# 셰이더 라이브러리 — 13. 개정 기록

[목차](README.ko.md)

## 13. 개정 기록

- 개정 1(2026-11-23, 초안): 사용자 지시("UE `Engine/Shaders`에 있는 것 대부분을 Rocks도 구현")에 따라 UE 5.8.4 `Engine/Shaders` 1,227개를 전수 분류했다(영역 35, 제외 10종 67개, 12장). 같은 날 사용자 결정(큰 기능 후속, 기본 영역 중심)과 추가 지시(렌더링 필수 컴포넌트 → 셰이더 표, 5장)를 반영했다. 기본 영역 A01–A23의 Rocks 파일·단계(SL0–SL10·SLU)·근거 문헌, 공통 라이브러리 1차 범위(SL0 — SS SH2와 함께), 시험 방침(컴파일 행렬·CPU 기준 구현 대조·golden·통계·기준 영상), 결정 SLD1–SLD16(권장안 확정), 다른 문서 요구 SLR1–SLR10을 적었다. 조사 중 확인한 사실: UE 5.7부터 고전 shading model 함수가 deprecated(Substrate로 대체)라 BRDF 근거를 Substrate 단일 slab 평가로 잡았다(SLD3). UE 5.8 ACES 버전 기본값은 2.0이다(SLD7). `SimpleF32*` 셰이더는 참조가 없다(1.4절).
