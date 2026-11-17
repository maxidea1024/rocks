#pragma once

// 스파이크 한 번 실행이다(Main.cpp가 인자를 읽어 부른다).
namespace rocks::spike {

struct RunOptions {
    // 산출물 디렉터리(UTF-8 경로). 없으면 만든다. 같은 이름 파일은 덮어쓴다.
    const char* outputDirectory = nullptr;
    // DXIL 셰이더 모델 프로필 뒤쪽("6_0"). 측정 S2 전 기본값은 6.0이다.
    const char* shaderModel = "6_0";
};

// 도구를 열고 모든 형식을 만들어 검증·측정한다. 산출물·manifest.json(결정적
// 내용)·report.json(측정값 포함)을 outputDirectory에 쓴다. 필수 검사(형식별 산출, spirv-val, DXIL
// 서명, FXC)가 모두 통과하면 0, 도구를 열지 못하면 2, 그 밖의 실패는 1이다.
int RunSpike(const RunOptions& options) noexcept;

} // namespace rocks::spike
