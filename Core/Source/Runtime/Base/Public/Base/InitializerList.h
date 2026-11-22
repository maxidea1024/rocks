#pragma once

// 형식 추론이 std::initializer_list를 요구하는 문법을 여는 헤더다(Docs/CodingStyle.md "중괄호 목록
// 추론", 정책 initializer-list-deduction·initializer-list-header).
//
// 엔진은 STL 헤더를 쓰지 않으므로 이 헤더 없이는 Android(NDK) 빌드에 std::initializer_list가 보이지
// 않아 중괄호 목록 range-for·auto 중괄호 복사 초기화가 컴파일되지 않는다. 그 문법을 쓰는 파일은
// 이 헤더를 직접 include한다(간접 include는 정책이 인정하지 않는다). <initializer_list>를 직접
// include하지 않는다. std::initializer_list 이름을 코드에 쓰는 일(컨테이너 생성자 등)은 이 예외에
// 들지 않는다.
//
// 전역(Base/Types.h)으로 열지 않는 이유: MSVC <initializer_list>는 <cstddef>·<xtr1common>을 함께
// 열어 PCH 없는 번역 단위마다 약 25 ms가 든다.
//
// 사용 예:
//   #include <Base/InitializerList.h>
//   for (const float cutoff : {250.0f, 1000.0f, 5000.0f}) { ... }

#include <initializer_list>
