# 셰이더 시스템 설계 — 16. 이후 단계(선행 작업 뒤)

[목차](../README.ko.md)

## 16. 이후 단계(선행 작업 뒤)

| 단계 | 내용 | 선행 | 완료 기준 |
|---|---|---|---|
| SH0 | 도구 스파이크: 지금의 내장 샘플 셰이더(`Programs/Sandbox/Private/SampleRenderer.cpp`의 HLSL)를 DXIL·SPIR-V·DXBC·MSL·ESSL로 만들고 검증 도구를 돌린다. Windows·macOS 양쪽에서 실행 | P3·P4·P10 | 형식별 산출, `spirv-val` 통과, DXIL 서명 hash 검사(macOS DXC 포함, S1), FXC 고정본 로드, 리플렉션 덤프, `ROCKS_IMAGE_FORMAT`의 DXC 지원 확인, S2·S3·S6·S8·S9·S10·S12(데스크톱)·S13·S14 측정. 남은 것(2026-11-20): macOS 부분(S1·S3 호스트 사이·S9)과 기계 하나로 못 재는 S2·S3·S12 일부(5.4절 끝) |
| SH1 | 소스 트리·가상 경로·`Platform.rsh`·전처리기(결정 6)·ShaderCompileWorker(P1, 11절)·두 층 키(P5)·에셋 너겟·`shader-compile` 명령·Development 필요 시 컴파일. 설계·하위 단계 SH1a–SH1e: 11.1절(개정 23) | SH0·P1·P5(A2·A3·P6a)·결정 6 | 같은 입력 같은 바이트, 빈/찬 캐시 동일, 워커 강제 종료 보고, 부모 종료 시 워커와 그 자식 종료, 형식 버전 불일치 진단, 도구 조회, 전처리 정합성(측정 S7) |
| SH2 | 리플렉션 정규화와 워커 규칙(7.2절)·ShaderCore와 셰이더 타입·퍼뮤테이션(P14)·쿠킹 라이브러리와 매니페스트 v2(P6b)·Graphics API의 쿠킹 코드 수신·RB3 셰이더 집합·런타임 컴파일러 제거·GLES 미리보기 Release 제외·샘플 렌더러 전환. 설계와 하위 단계 SH2a–SH2g: 16.1절(개정 31) | SH1·P2·P4·P9·P11·A0-G·결정 2·3·4·5·9 | 16.1절 완료 기준 |
| SH3 | 파라미터 구조체(P7)·생성 HLSL·오프셋 시험. 화면 마일스톤 밖, 렌더러 RS0 전(RenderBase 18.4절 SM11, 개정 49). 설계와 하위 단계 SH3a–SH3c: 16.2절(개정 54). SH3a 구현(개정 55), SH3b·SH3c 설계(개정 56), SH3b 구현(개정 57), SH3c 구현(개정 58) — SH3 끝 | SH2·P7 | 7.3절 (a)·(f)를 생성 구조체로 |
| SH4 | 감시 재컴파일(`DirectoryWatch`)·오류 위치·디버그 심볼·PSO 교체. 화면 마일스톤 밖, 렌더러 RS0 전(RenderBase 18.4절 SM12, 개정 49). 설계와 하위 단계 SH4a–SH4d: 16.3절(개정 60), 구현(개정 61) — SH4 끝 | SH2 | 저장 후 10초 안 반영(계획 I5) |
| SH5 | MetalGraphics(D3D11의 쿠킹 셰이더 전환은 개정 32에서 SH2로 옮겼다) | SH2·P12·P15 | Metal이 D3D12와 허용 오차 안 일치, 7.3절 (c)·(e)·(g)의 Metal 부분 |
| SH6 | Android: `GLES31_ANDROID` 기기 실행, `VULKAN_ANDROID` 형식과 VulkanGraphics Android | SH2·P13·P11 | Android(실기기 또는 S11 에뮬레이터)에서 D3D12 기준 이미지와 허용 오차 안 일치, 7.3절 (c)·(i)의 Android 부분, S12(기기) |

절 파일:

- [16.1 SH2 설계](16.01-SH2Design.ko.md)
- [16.2 SH3 설계](16.02-SH3Design.ko.md)
- [16.3 SH4 설계](16.03-SH4Design.ko.md)
- [16.4 컴파일 경고·FXC 분류·워커 분배](16.04-CompileFollowups.ko.md)
