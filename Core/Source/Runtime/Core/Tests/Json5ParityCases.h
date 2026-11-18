#pragma once

// JSON5 읽기(JsonSyntax::Json5)의 C++·C# 공용 대조 사례다. C++ Json5Tests.cpp가 이 헤더를 그대로
// 쓰고, C# Json5ReaderTests(Programs/Rocks.ToolTests)가 이 파일을 텍스트로 읽어 같은 표를 C 문자열
// 규칙
// (\\ \" \n \r \t \v \f \xHH, 이웃 문자열 이어 붙이기)으로 풀어 같은 기대값을 확인한다 — 두 구현이
// 같은 입력 집합을 받고 같은 값·오류 코드·위치를 내는지 본다. 그래서 이 파일의 표 안에서는 위
// 이스케이프 밖의 C 문법(매크로·\u·8진 이스케이프·원시 문자열)을 쓰지 않는다. \xHH 뒤에 16진수
// 글자가 오면 C는 이어서 읽으므로 그때는 문자열을 나눈다("\xA0" "1").
//
// 기대값 표기(두 쪽 시험의 Dump와 같다):
// - null·true·false, 정수 i<10진>, 부호 없는 정수 u<10진>, 실수 d<double 비트 16진 16자리>
// - 문자열 "..."(", \ 는 \", \\, U+0020–U+007E 밖은 \u{대문자 16진})
// - 배열 [a,b], 객체 {"k":v,...}(원문 순서)
// - 오류 !<JsonErrorCode 이름>@<줄>:<열>
// 입력은 기본 한도(JsonReadOptions 기본값)로 읽는다. 입력에 NUL은 넣지 않는다(C 문자열).
namespace rocks::test {

struct Json5ParityCase {
    const char* input;
    const char* expected;
};

inline constexpr Json5ParityCase Json5ParityCases[] = {
    // 구조·끝 쉼표·중복 키
    {"[1, 2,]", "[i1,i2]"},
    {"{a: 1, b: 2,}", "{\"a\":i1,\"b\":i2}"},
    {"[,]", "!UnexpectedCharacter@1:2"},
    {"[1,,]", "!UnexpectedCharacter@1:4"},
    {"{,}", "!UnexpectedCharacter@1:2"},
    {"[1 2]", "!UnexpectedCharacter@1:4"},
    {"{a 1}", "!UnexpectedCharacter@1:4"},
    {"", "!UnexpectedEnd@1:1"},
    {"[1] [2]", "!TrailingContent@1:5"},
    {"{a: 1, a: 2}", "!DuplicateKey@1:8"},
    {"{\"a\": 1, 'a': 2}", "!DuplicateKey@1:10"},
    {"{a: 1, \\u0061: 2}", "!DuplicateKey@1:8"},
    {"{null: 1, true: 2, Infinity: 3}", "{\"null\":i1,\"true\":i2,\"Infinity\":i3}"},
    {"  42  ", "i42"},
    {"'x'", "\"x\""},
    // 주석
    {"// only comment", "!UnexpectedEnd@1:16"},
    {"/* a */ 1 // b", "i1"},
    {"[1 /* unterminated", "!UnexpectedEnd@1:19"},
    {"[1 / 2]", "!UnexpectedCharacter@1:4"},
    {"[1, // c\n2]", "[i1,i2]"},
    {"[1, /* x\n y */ 2]", "[i1,i2]"},
    {"/**/1", "i1"},
    {"/***/1", "i1"},
    {"1 /* a */ /* b */", "i1"},
    {"[1, // c\xE2\x80\xA8 2]", "[i1,i2]"},
    {"[1, // c\xE2\x80\xA9 2]", "[i1,i2]"},
    {"[1 // \xFF\n]", "!InvalidUtf8@1:7"},
    {"/* \xC3 */1", "!InvalidUtf8@1:4"},
    // 공백
    {"\v\f[\xC2\xA0 1\xE2\x80\xA8,\xE2\x80\xA9 2\xEF\xBB\xBF]", "[i1,i2]"},
    {"[\xE3\x80\x80"
     "1\xE1\x9A\x80,\xE2\x80\x80"
     "2\xE2\x81\x9F]",
     "[i1,i2]"},
    {"[\xC2\x85"
     "1]",
     "!UnexpectedCharacter@1:2"},
    {"[\xE2\x80\x8B"
     "1]",
     "!UnexpectedCharacter@1:2"},
    {"\xEF\xBB\xBF{a:1}", "{\"a\":i1}"},
    {"\xEF\xBB\xBF]", "!UnexpectedCharacter@1:1"},
    {"[\xEF\xBB\xBF]]", "!TrailingContent@1:4"},
    {"\xEF\xBB\xBF\xEF\xBB\xBF"
     "1",
     "i1"},
    // 식별자 키
    {"{$_: 1, _a1: 2, $: 3}", "{\"$_\":i1,\"_a1\":i2,\"$\":i3}"},
    {"{a\xE2\x80\x8C"
     "b\xE2\x80\x8D"
     "c: 1}",
     "{\"a\\u{200C}b\\u{200D}c\":i1}"},
    {"{\xC7\x85: 1, \xCA\xB0: 2, \xEA\xB0\x80: 3, \xE2\x85\xA0: 4, \xC3\x84\xC3\xA9: 5}",
     "{\"\\u{1C5}\":i1,\"\\u{2B0}\":i2,\"\\u{AC00}\":i3,\"\\u{2160}\":i4,\"\\u{C4}\\u{E9}\":i5}"},
    {"{\xCC\x81"
     "a: 1}",
     "!UnexpectedCharacter@1:2"},
    {"{1a: 1}", "!UnexpectedCharacter@1:2"},
    {"{a\xCC\x81\xE0\xA4\x83\xD9\xA3\xE2\x80\xBF: 1}",
     "{\"a\\u{301}\\u{903}\\u{663}\\u{203F}\":i1}"},
    {"{\xF0\xA0\x80\x80: 1}", "{\"\\u{20000}\":i1}"},
    {"{\\u0061b: 1}", "{\"ab\":i1}"},
    {"{a\\u0020: 1}", "!InvalidEscape@1:3"},
    {"{\\u0031: 1}", "!InvalidEscape@1:2"},
    {"{a\\u0031: 1}", "{\"a1\":i1}"},
    {"{\\uD840\\uDC00: 1}", "!InvalidEscape@1:2"},
    {"{\\x61: 1}", "!InvalidEscape@1:2"},
    {"{\\u00G1: 1}", "!InvalidEscape@1:2"},
    {"{\\u006", "!UnexpectedEnd@1:7"},
    {"{a-b: 1}", "!UnexpectedCharacter@1:3"},
    {"{\\u0024: 1}", "{\"$\":i1}"},
    {"{\\uAC00: 1}", "{\"\\u{AC00}\":i1}"},
    {"{a\xFF: 1}", "!InvalidUtf8@1:3"},
    {"{abc", "!UnexpectedEnd@1:5"},
    {"{\xE2\x82\xAC: 1}", "!UnexpectedCharacter@1:2"},
    // 문자열
    {"'a\"b'", "\"a\\\"b\""},
    {"\"a'b\"", "\"a'b\""},
    {"'\\'\\\"\\\\\\b\\f\\n\\r\\t\\v\\0'",
     "\"'\\\"\\\\\\u{8}\\u{C}\\u{A}\\u{D}\\u{9}\\u{B}\\u{0}\""},
    {"'\\01'", "!InvalidEscape@1:2"},
    {"'\\1'", "!InvalidEscape@1:2"},
    {"'\\9'", "!InvalidEscape@1:2"},
    {"'\\x41\\xe9'", "\"A\\u{E9}\""},
    {"'\\x4'", "!InvalidEscape@1:2"},
    {"'\\x4", "!UnexpectedEnd@1:5"},
    {"'\\a\\q\\/\\ '", "\"aq/ \""},
    {"'\\\xEA\xB0\x80'", "\"\\u{AC00}\""},
    {"'a\\\nb'", "\"ab\""},
    {"'a\\\r\nb'", "\"ab\""},
    {"'a\\\rb'", "\"ab\""},
    {"'a\\\xE2\x80\xA8"
     "b'",
     "\"ab\""},
    {"'a\\\xE2\x80\xA9"
     "b'",
     "\"ab\""},
    {"'a\xE2\x80\xA8"
     "b\xE2\x80\xA9'",
     "\"a\\u{2028}b\\u{2029}\""},
    {"'a\nb'", "!ControlCharacter@1:3"},
    {"'a\rb'", "!ControlCharacter@1:3"},
    {"'a\tb\x01'", "\"a\\u{9}b\\u{1}\""},
    {"'\\uD83D\\uDE00'", "\"\\u{1F600}\""},
    {"'\\uDE00'", "!UnpairedSurrogate@1:2"},
    {"'\\uD83Dx'", "!UnpairedSurrogate@1:2"},
    {"'\\uD83D\\x41'", "!UnpairedSurrogate@1:2"},
    {"'\xC3('", "!InvalidUtf8@1:2"},
    {"'abc", "!UnexpectedEnd@1:5"},
    {"'a\\", "!UnexpectedEnd@1:4"},
    {"'\\\xFF'", "!InvalidUtf8@1:3"},
    {"'a\"", "!UnexpectedEnd@1:4"},
    // 숫자
    {"[+1, -1, +0, -0, 0]", "[i1,i-1,i0,d8000000000000000,i0]"},
    {"[.5, 5., -.5, +.5, 5.e3, .5e-1, 0.]",
     "[d3fe0000000000000,d4014000000000000,dbfe0000000000000,d3fe0000000000000,"
     "d40b3880000000000,d3fa999999999999a,d0000000000000000]"},
    {"[0x1F, 0X1f, -0x10, +0xA, 0x0, -0x0]", "[i31,i31,i-16,i10,i0,d8000000000000000]"},
    {"0xFFFFFFFFFFFFFFFF", "u18446744073709551615"},
    {"-0x8000000000000000", "i-9223372036854775808"},
    {"-0x8000000000000001", "dc3e0000000000000"},
    {"0x10000000000000000", "!NumberOutOfRange@1:1"},
    {"0x00000000000000000001", "i1"},
    {"0x", "!UnexpectedEnd@1:3"},
    {"[0x]", "!InvalidNumber@1:4"},
    {"0xg", "!InvalidNumber@1:3"},
    {"0x1g", "!InvalidNumber@1:4"},
    {"[01]", "!InvalidNumber@1:3"},
    {"[-01]", "!InvalidNumber@1:4"},
    {"[00]", "!InvalidNumber@1:3"},
    {"[1e5, 1E+5, 1e-5]", "[d40f86a0000000000,d40f86a0000000000,d3ee4f8b588e368f1]"},
    {"[1e]", "!InvalidNumber@1:4"},
    {"1e", "!UnexpectedEnd@1:3"},
    {"[1x]", "!InvalidNumber@1:3"},
    {"[1.5.3]", "!UnexpectedCharacter@1:5"},
    {"[.]", "!InvalidNumber@1:3"},
    {"[+]", "!InvalidNumber@1:3"},
    {"+", "!UnexpectedEnd@1:2"},
    {"[-x]", "!InvalidNumber@1:3"},
    {"[.e1]", "!InvalidNumber@1:3"},
    {"[Infinity, -Infinity, +Infinity, NaN, -NaN, +NaN]",
     "[d7ff0000000000000,dfff0000000000000,d7ff0000000000000,d7ff8000000000000,"
     "d7ff8000000000000,d7ff8000000000000]"},
    {"[Infinit]", "!InvalidLiteral@1:2"},
    {"[-Inf", "!UnexpectedEnd@1:6"},
    {"[NaNa]", "!InvalidLiteral@1:2"},
    {"[-infinity]", "!InvalidNumber@1:3"},
    {"[Infinity1]", "!InvalidLiteral@1:2"},
    {"1e400", "!NumberOutOfRange@1:1"},
    {"-1e400", "!NumberOutOfRange@1:1"},
    {"1e-400", "d0000000000000000"},
    {"18446744073709551616", "d43f0000000000000"},
    // 단어
    {"[true, false, null]", "[true,false,null]"},
    {"[truex]", "!InvalidLiteral@1:2"},
    {"[tru", "!UnexpectedEnd@1:5"},
    {"[nul]", "!InvalidLiteral@1:2"},
    {"[undefined]", "!InvalidLiteral@1:2"},
    {"[True]", "!InvalidLiteral@1:2"},
    {"[@]", "!UnexpectedCharacter@1:2"},
    {"[\xEA\xB0\x80]", "!InvalidLiteral@1:2"},
    {"[\xE2\x82\xAC]", "!UnexpectedCharacter@1:2"},
    {"[true\\u0061]", "!InvalidLiteral@1:2"},
    // 위치(줄·열): CRLF·CR·LF는 줄바꿈, U+2028·U+2029는 아니다
    {"{\n  a: 1,\n  b: x\n}", "!InvalidLiteral@3:6"},
    {"[\r\n1,\r2,\n3,\xE2\x80\xA8 x]", "!InvalidLiteral@4:5"},
    {"['\xEA\xB0\x80', x]", "!InvalidLiteral@1:7"},
};

} // namespace rocks::test
