#pragma once

#include <Base/Types.h>
#include <Core/Text/StringView.h>

// Custom HLSL 텍스트의 V13 토큰 검사 결과다(Docs/Design/MaterialSystem 6.13절 MS2f5, 7.5절).
// 검사기는 컴파일러가 보는 것과 같게 읽는다: 주석(// 줄·/* */ 묶음)과 문자열 리터럴("…", 역슬래시
// escape) 밖의 식별자 토큰·#·중괄호만 센다. 줄 잇기(\ 뒤 줄바꿈)는 컴파일러가 토큰을 이어 붙이므로
// 따로 표시한다(쓰는 쪽이 거부한다).
//
// 사용 예:
//   CustomCodeScan scan;
//   ScanCustomCode(code, scan);
//   if (scan.lineContinuation) { ... }
namespace rocks::material::detail {

// 금지 단어 수다(CustomForbiddenWord 순서 — register·cbuffer·tbuffer·groupshared·static·_Pragma·
// __pragma).
inline constexpr usize CustomForbiddenWordCount = 7;

struct CustomCodeScan {
    // 금지 단어별 발견 표시다(CustomForbiddenWord(index) 순서).
    bool words[CustomForbiddenWordCount] = {};
    // 주석·문자열 밖 # 이 있는가와 그 첫 지시 이름(# 뒤 공백을 지난 식별자 — 없으면 빈 값)이다.
    bool directive = false;
    StringView directiveName;
    // \ 뒤 줄바꿈(\n 또는 \r\n)이 있는가.
    bool lineContinuation = false;
    // 닫히지 않은 /* 또는 문자열이 있는가.
    bool unterminatedComment = false;
    bool unterminatedString = false;
    // 주석이 있는가(닫힌 것 포함 — 정의 값 검사).
    bool comment = false;
    // 중괄호가 0 아래로 닫히는가, 끝에 열린 채 남는가.
    bool closesBody = false;
    bool leavesOpen = false;
    // 주석·문자열 밖 clip 호출(식별자 clip 뒤 공백·주석을 지나 '(')·식별자 discard가 있는가.
    bool clipCall = false;
    bool discardWord = false;
};

// index번 금지 단어다(범위 밖이면 빈 값).
StringView CustomForbiddenWord(usize index) noexcept;

// text를 검사해 scan을 채운다(scan은 기본값이어야 한다).
void ScanCustomCode(StringView text, CustomCodeScan& scan) noexcept;

// HLSL 식별자인가([A-Za-z_][A-Za-z0-9_]*).
bool IsCustomIdentifier(StringView text) noexcept;

} // namespace rocks::material::detail
