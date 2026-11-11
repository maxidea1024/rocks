#pragma once

#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>

namespace rocks {

// RFC 3986 URI 참조(절대 URI와 상대 참조)를 분해한 결과다. 모든 view는 TryParseUri에 넘긴 입력을
// 빌리므로 입력 문자열보다 오래 쓰지 않는다. 각 view는 구분자(':', '//', '@', '?', '#')를 포함하지
// 않으며 percent-encoding을 해제하지 않은 원문이다. 해제가 필요하면 TryPercentDecode를 쓴다.
//
// UE 5.6에서는 같은 용도의 함수(GetUrlDomain/Port/Path/Parameter, UrlEncode/Decode)가 Core가 아닌
// HTTP 모듈의 FGenericPlatformHttp에 있다. Rocks는 네트워크 계층이 공용으로 쓰도록 Core에 둔다.
struct UriView {
    StringView scheme; // 비어 있으면 상대 참조다. 대소문자는 원문 그대로다.
    bool hasAuthority = false;
    StringView authority; // "//" 뒤 authority 원문 전체(userinfo@host:port). 재조합에 쓴다.
    StringView userInfo;  // '@' 앞부분. hasUserInfo로 빈 userinfo와 부재를 구분한다.
    bool hasUserInfo = false;
    StringView host; // IP literal은 대괄호를 뺀 내용이다(isIpLiteral). 빈 host도 유효하다.
    bool isIpLiteral = false;
    bool hasPort = false;
    uint16 port = 0; // hasPort이고 숫자가 있으면 그 값이다. "host:"처럼 숫자가 없으면 0이다.
    StringView path;
    bool hasQuery = false;
    StringView query;
    bool hasFragment = false;
    StringView fragment;
};

// RFC 3986 문법으로 검증·분해한다. 실패(InvalidArgument): 허용되지 않은 문자(공백, 제어 문자,
// 비ASCII byte, 문맥에 맞지 않는 '[' ']' 등), 잘못된 %HH, 65535를 넘는 포트, 잘못된 IPv6 literal,
// scheme 문법 위반, authority가 있는데 '/'로 시작하지 않는 path, authority 없이 "//"로 시작하는
// path, scheme 없는 상대 참조의 첫 segment에 ':'가 있는 경우. IRI(비ASCII 문자 직접 사용)는
// 지원하지 않으므로 호출자가 먼저 UTF-8을 percent-encoding해야 한다. 성공·실패 모두 입력을 바꾸지
// 않는다.
Result<UriView> TryParseUri(StringView text) noexcept;

// percent-decoding 방식이다. Component는 RFC 3986 그대로 '+'를 유지하고, FormUrlEncoded는
// application/x-www-form-urlencoded처럼 '+'를 공백으로 바꾼다.
enum class PercentDecodeMode : uint8 {
    Component,
    FormUrlEncoded
};

// %HH를 byte로 바꿔 output에 쓰고 쓴 byte 수를 반환한다. 결과 byte가 유효한 UTF-8인지는 검사하지
// 않는다(필요하면 StringView::ValidateUtf8). 실패하면 output을 바꾸지 않는다.
// 오류: 잘못된 '%' 표기는 InvalidArgument, output 부족은 CapacityExceeded.
Result<usize> TryPercentDecode(StringView text, Span<char> output,
                               PercentDecodeMode mode = PercentDecodeMode::Component) noexcept;

// unreserved(A–Z a–z 0–9 - . _ ~)를 제외한 모든 byte를 대문자 %HH로 바꾼다(UE UrlEncode와 같은 문자
// 집합). 결과 길이를 반환하고 NUL을 붙이지 않는다. 실패하면 output을 바꾸지 않는다.
// 오류: output 부족은 CapacityExceeded, 길이 계산 overflow는 Overflow.
Result<usize> TryPercentEncode(StringView text, Span<char> output) noexcept;

// TryPercentEncode에 필요한 출력 크기다.
Result<usize> PercentEncodedSize(StringView text) noexcept;

// query("a=1&b=2", '?' 제외)에서 name과 정확히(대소문자 구분, 인코딩 해제 없이) 일치하는 첫
// 파라미터의 원문 값을 찾는다. "flag"처럼 '='가 없으면 빈 값으로 찾은 것으로 본다. 값 view는
// query를 빌린다.
bool FindQueryParameter(StringView query, StringView name, StringView& value) noexcept;

// TryResolveUriReference가 요구하는 출력 크기(base + reference + 2 byte)다. 결과는 이보다 짧을 수
// 있다. 오류: 길이 계산 overflow는 Overflow.
Result<usize> ResolvedUriMaxSize(StringView base, StringView reference) noexcept;

// reference를 base 기준으로 해석한 대상 URI를 output에 쓰고 길이를 반환한다(NUL 없음). RFC 3986
// §5.2.2(strict: reference에 scheme이 있으면 base와 같은 scheme이어도 절대 URI로 본다), §5.2.3
// merge, §5.2.4 dot segment 제거, §5.3 재조합을 따른다.
// - base는 scheme이 있는 URI여야 하며 base의 fragment는 무시한다.
// - percent-encoding 해제·대소문자·기본 포트 같은 정규화는 하지 않는다.
// - authority 없는 결과 path가 "//"로 시작하면 다시 파싱할 때 authority로 오인되므로 "/."를 앞에
//   붙인다(RFC 3986에는 규정이 없고 WHATWG URL serializer와 같은 처리).
// - output.Size()는 결과 길이가 아니라 ResolvedUriMaxSize 이상이어야 한다. 실패하면 output을
//   바꾸지 않는다.
// 오류: base·reference 문법 오류와 scheme 없는 base는 InvalidArgument, output 부족은
// CapacityExceeded, 크기 overflow는 Overflow.
//
// 사용 예:
//   char buffer[256];
//   auto resolved = TryResolveUriReference("http://a/b/c/d;p?q", "../g", Span<char>(buffer));
//   // resolved.Value() == 12, buffer = "http://a/b/g"
Result<usize> TryResolveUriReference(StringView base, StringView reference,
                                     Span<char> output) noexcept;

// TryNormalizeUri가 요구하는 출력 크기(text + 2 byte)다. 오류: 길이 계산 overflow는 Overflow.
Result<usize> NormalizedUriMaxSize(StringView text) noexcept;

// URI 참조를 정규화해 output에 쓰고 길이를 반환한다(NUL 없음). 같은 자원을 가리키는 표기를 하나로
// 모아 비교·캐시 키로 쓰기 위한 것이다.
// - RFC 3986 §6.2.2 문법 기반: scheme·host 소문자화, %hh의 16진 숫자 대문자화, unreserved 문자
//   (A–Z a–z 0–9 - . _ ~)를 나타내는 %hh 해제, scheme이 있는 URI의 dot segment 제거. scheme 없는
//   상대 참조는 ".."가 의미를 가지므로 dot segment를 남긴다.
// - §6.2.3 scheme 기반(http·https·ws·wss만): 기본 포트(80·443) 제거, authority가 있는 빈 path는
// "/".
//   모든 scheme에서 숫자 없는 빈 포트(":")를 제거하고 포트의 앞자리 0을 없앤다.
// - userinfo·path·query·fragment의 대소문자와 예약 문자의 %hh(예: %2F)는 의미가 있으므로 유지한다.
// - authority 없는 결과 path가 "//"로 시작하면 TryResolveUriReference와 같이 "/."를 붙인다.
// - output.Size()는 NormalizedUriMaxSize 이상이어야 하며 실패하면 output을 바꾸지 않는다.
// 오류: 문법 오류는 InvalidArgument, output 부족은 CapacityExceeded, 크기 overflow는 Overflow.
//
// 사용 예:
//   char buffer[64];
//   auto normalized = TryNormalizeUri("HTTP://Example.COM:80/a/./%7euser", Span<char>(buffer));
//   // buffer = "http://example.com/a/~user"
Result<usize> TryNormalizeUri(StringView text, Span<char> output) noexcept;

} // namespace rocks
