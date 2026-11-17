#pragma once

#include <Base/Types.h>

namespace rocks {

// 소스 코드의 호출 위치(파일·함수·줄)다. STL <source_location> 대신 컴파일러 내장 함수로 채운다.
//
// Current()를 함수의 기본 인자로 두면 기본 인자는 호출 지점에서 평가되므로 그 함수를 부른 쪽의
// 위치가 남는다. 함수 본문에서 직접 부르면 그 본문의 위치다.
//
// 값:
// - file: 컴파일러가 받은 원본 파일 경로(경로 접두사 정리는 빌드 설정의 몫이다)
// - function: 함수 이름(clang은 한정·인자 없는 이름, 람다 안이면 "operator()")
// - line: 1부터 센 줄 번호
// 기본 생성 값은 모르는 위치(빈 문자열·줄 0)다.
//
// 수명: 문자열은 컴파일러가 만든 정적 저장소라 프로그램 수명 동안 유효하다(모듈은 정적 링크이고
// 언로드하지 않는다).
//
// 사용 예:
//   void Report(StringView text, SourceLocation where = SourceLocation::Current()) noexcept;
//   Report("disk full"); // where는 이 줄의 파일·함수·줄이다
struct SourceLocation {
    const char* file = "";
    const char* function = "";
    uint32 line = 0;

    // 호출 지점의 위치를 만든다. 인자를 직접 넘기지 않는다(기본 인자가 호출 지점을 담는다).
    static consteval SourceLocation Current(const char* file = __builtin_FILE(),
                                            const char* function = __builtin_FUNCTION(),
                                            uint32 line = __builtin_LINE()) noexcept {
        SourceLocation location;
        location.file = file;
        location.function = function;
        location.line = line;
        return location;
    }
};

} // namespace rocks
