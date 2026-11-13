#pragma once

#include <Core/Encoding/Sha256.h>

// 빌드가 실행 파일마다 주입하는 신원 값이다(Docs/Design/ReflectionFoundations.ko.md 8.1절).
// 정의는 Rocks.Build가 이 헤더를 제공하는 Core에 닿는 실행 파일마다 만드는 생성 TU
// (<출력>/BuildInfo/<실행파일>.BuildInfo.cpp)에 있다. 정적 라이브러리는 정의를 갖지 않으므로
// 주입되지 않은 링크(Rocks.Build를 거치지 않은 실행 파일)에서 부르면 링크 오류다.
// 원문(지문 정규 텍스트와 도구 빌드 ID manifest)은 같은 폴더의 ToolchainFingerprint.txt와
// <실행파일>.BuildInfo.txt에 있고, 그 SHA-256이 아래 값과 같다.
//
// 사용 예(에셋 너겟 키와 쿠킹 보고서):
//   const Sha256Digest toolchain = build::ToolchainFingerprint();   // 너겟 키에 넣는다
//   char id[Sha256Digest::HexBytes + 1];
//   build::ToolBuildId().ToHex(id);                                 // 보고서·캐시 header 진단용
//
// 두 함수 모두 실패하지 않고 프로세스 수명 동안 같은 값을 돌려준다(상수 반환 — 스레드 안전).
namespace rocks::build {

// 이 실행 파일을 빌드한 툴체인의 지문이다.
// SHA-256(컴파일러 식별·버전, 컴파일러·링커 바이너리 해시, 대상 triple, FP 모델 인자)이며
// 빌드 구성(Debug·Release)과 호스트 경로는 들어가지 않는다 — 같은 툴체인의 모든 구성이 같다.
// 용도: 에셋 너겟 키(다른 툴체인이 만든 너겟 항목을 재사용하지 않는다). 쿠킹 byte에는 넣지 않는다.
Sha256Digest ToolchainFingerprint() noexcept;

// 이 실행 파일의 도구 빌드 ID다.
// SHA-256(정렬된 입력 내용, 정규화된 compile·link 인자, 툴체인 지문)이며 소스 편집(주석 포함)·
// 구성·도구 버전이 바뀌면 달라진다. 용도: 진단과 캐시 무효화(쿠킹 보고서·AssetIndex 캐시 header).
// 패키지(Summary 등)와 쿠킹 byte에는 쓰지 않는다 — 쓰면 엔진 빌드마다 쿠킹 결과가 바뀐다.
Sha256Digest ToolBuildId() noexcept;

} // namespace rocks::build
