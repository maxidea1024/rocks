#include "Tests/Support/Test.h"
#include <Core/Text/Uri.h>

namespace rocks::test {
namespace {

bool Is(StringView view, const char* expected) {
    return view == StringView(expected);
}

} // namespace

TEST(Uri_ParsesRfc3986Examples) {
    auto full = TryParseUri("foo://example.com:8042/over/there?name=ferret#nose");
    REQUIRE(full);
    const UriView& uri = full.Value();
    CHECK(Is(uri.scheme, "foo") && uri.hasAuthority && Is(uri.host, "example.com"));
    CHECK(uri.hasPort && uri.port == 8042 && Is(uri.path, "/over/there"));
    CHECK(uri.hasQuery && Is(uri.query, "name=ferret"));
    CHECK(uri.hasFragment && Is(uri.fragment, "nose"));
    CHECK(!uri.hasUserInfo && !uri.isIpLiteral);

    auto urn = TryParseUri("urn:example:animal:ferret:nose");
    REQUIRE(urn);
    CHECK(Is(urn.Value().scheme, "urn") && !urn.Value().hasAuthority);
    CHECK(Is(urn.Value().path, "example:animal:ferret:nose"));

    auto ldap = TryParseUri("ldap://[2001:db8::7]/c=GB?objectClass?one");
    REQUIRE(ldap);
    CHECK(ldap.Value().isIpLiteral && Is(ldap.Value().host, "2001:db8::7"));
    CHECK(Is(ldap.Value().query, "objectClass?one"));

    auto mail = TryParseUri("mailto:John.Doe@example.com");
    REQUIRE(mail);
    CHECK(Is(mail.Value().path, "John.Doe@example.com") && !mail.Value().hasAuthority);

    auto user = TryParseUri("https://user:pa%20ss@host.example/");
    REQUIRE(user);
    CHECK(user.Value().hasUserInfo && Is(user.Value().userInfo, "user:pa%20ss"));
    CHECK(Is(user.Value().host, "host.example") && !user.Value().hasPort);

    auto file = TryParseUri("file:///C:/Games/Rocks");
    REQUIRE(file);
    CHECK(file.Value().hasAuthority && file.Value().host.IsEmpty());
    CHECK(Is(file.Value().path, "/C:/Games/Rocks"));

    auto ipv4 = TryParseUri("telnet://192.0.2.16:80/");
    REQUIRE(ipv4);
    CHECK(Is(ipv4.Value().host, "192.0.2.16") && ipv4.Value().port == 80);
}

TEST(Uri_ParsesRelativeReferences) {
    const char* valid[] = {"",    "g",  "./g", "g/",    "/g",         "//g",        "?y",
                           "g?y", "#s", "g#s", "g?y#s", ";x",         "g;x",        "g;x?y#s",
                           ".",   "./", "..",  "../g",  "../../../g", "./this:that"};
    for (const char* text : valid) {
        CHECK(TryParseUri(text));
    }
    auto network = TryParseUri("//g");
    REQUIRE(network);
    CHECK(network.Value().scheme.IsEmpty() && network.Value().hasAuthority);
    CHECK(Is(network.Value().host, "g"));
    auto empty = TryParseUri("");
    REQUIRE(empty);
    CHECK(empty.Value().path.IsEmpty() && !empty.Value().hasQuery && !empty.Value().hasFragment);
    // "?"와 "#"만 있어도 빈 query·fragment가 존재한다.
    auto marks = TryParseUri("x?#");
    REQUIRE(marks);
    CHECK(marks.Value().hasQuery && marks.Value().query.IsEmpty() && marks.Value().hasFragment);
}

TEST(Uri_RejectsInvalidSyntax) {
    const char* invalid[] = {
        "http://exa mple.com",         // 공백
        "http://host:99999/",          // 포트 범위
        "http://host:12a/",            // 숫자가 아닌 포트
        "http://host/%zz",             // 잘못된 percent-encoding
        "http://host/%4",              // 잘린 percent-encoding
        "http://[::1/",                // 닫히지 않은 IP literal
        "http://[1:2:3:4:5:6:7:8:9]/", // 조각 초과
        "http://[1::2::3]/",           // "::" 두 번
        "http://[::1]x/",              // literal 뒤 잡음
        "http://[12345::]/",           // 5자리 조각
        "http://[::256.0.0.1]/",       // IPv4 범위
        "http://[::01.2.3.4]/",        // IPv4 앞자리 0
        "http://a@b@c/",               // host의 '@'
        "1http://x",                   // scheme이 아니므로 첫 segment의 ':'
        ":relative",                   // 빈 scheme
        "a:b c",                       // 공백
        "http://host/\x7f",            // 제어 문자
        "http://host/\xc3\xa9",        // 비ASCII(IRI 미지원)
        "http://host/a#b#c",           // fragment 안의 '#'
        "scheme://host:80:90/",        // 포트 구분자 중복
    };
    for (const char* text : invalid) {
        auto parsed = TryParseUri(text);
        CHECK(!parsed && parsed.Error() == ErrorCode::InvalidArgument);
    }
    const char embedded[] = {'a', ':', 'b', '\0', 'c'};
    CHECK(!TryParseUri(StringView(embedded, 5)));
    CHECK(TryParseUri("http://[v1.fe:80]/"));         // IPvFuture
    CHECK(TryParseUri("http://[::ffff:192.0.2.1]/")); // IPv4 접미
    CHECK(TryParseUri("http://host:/"));              // 빈 포트는 문법상 허용
    CHECK(TryParseUri("http://host:00080/").Value().port == 80);
}

TEST(Uri_PercentDecodeIsStrictAndAtomic) {
    char output[32] = {};
    auto decoded = TryPercentDecode("%41%62c%2Fd+e", Span<char>(output));
    REQUIRE(decoded);
    CHECK(Is(StringView(output, decoded.Value()), "Abc/d+e"));
    auto form = TryPercentDecode("a+b%2B", Span<char>(output), PercentDecodeMode::FormUrlEncoded);
    REQUIRE(form);
    CHECK(Is(StringView(output, form.Value()), "a b+"));
    // 잘못된 표기와 용량 부족은 출력 버퍼를 바꾸지 않는다.
    const char* bad[] = {"%", "%4", "%G1", "abc%", "%%41"};
    for (const char* text : bad) {
        char untouched[8] = {'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x'};
        auto result = TryPercentDecode(text, Span<char>(untouched));
        CHECK(!result && result.Error() == ErrorCode::InvalidArgument);
        CHECK(untouched[0] == 'x' && untouched[7] == 'x');
    }
    char small[2] = {'x', 'x'};
    auto capacity = TryPercentDecode("abc", Span<char>(small));
    CHECK(!capacity && capacity.Error() == ErrorCode::CapacityExceeded && small[0] == 'x');
    // 디코딩 결과의 UTF-8 유효성은 검사하지 않는다(호출자 책임).
    auto raw = TryPercentDecode("%FF%00", Span<char>(output));
    REQUIRE(raw);
    CHECK(raw.Value() == 2 && static_cast<uint8>(output[0]) == 0xff && output[1] == '\0');
}

TEST(Uri_PercentEncodeUnreservedSet) {
    char output[64] = {};
    auto encoded = TryPercentEncode("a b/\xc3\xbc-._~!", Span<char>(output));
    REQUIRE(encoded);
    CHECK(Is(StringView(output, encoded.Value()), "a%20b%2F%C3%BC-._~%21"));
    CHECK(PercentEncodedSize("a b").Value() == 5);
    char small[4] = {'x', 'x', 'x', 'x'};
    CHECK(TryPercentEncode("a b", Span<char>(small)).Error() == ErrorCode::CapacityExceeded);
    CHECK(small[0] == 'x');
    // 인코딩 → 디코딩 왕복은 모든 byte를 보존한다.
    char all[256];
    for (int value = 0; value < 256; ++value) {
        all[value] = static_cast<char>(value);
    }
    char wide[768];
    char back[256];
    auto encodedAll = TryPercentEncode(StringView(all, 256), Span<char>(wide));
    REQUIRE(encodedAll);
    auto decodedAll = TryPercentDecode(StringView(wide, encodedAll.Value()), Span<char>(back));
    REQUIRE(decodedAll && decodedAll.Value() == 256);
    bool same = true;
    for (int value = 0; value < 256; ++value) {
        same = same && back[value] == all[value];
    }
    CHECK(same);
}

TEST(Uri_FindQueryParameter) {
    StringView value;
    CHECK(FindQueryParameter("a=1&b=2&a=3", "a", value) && Is(value, "1"));
    CHECK(FindQueryParameter("a=1&b=2&a=3", "b", value) && Is(value, "2"));
    CHECK(FindQueryParameter("flag&x=%20y", "flag", value) && value.IsEmpty());
    CHECK(FindQueryParameter("flag&x=%20y", "x", value) && Is(value, "%20y"));
    CHECK(FindQueryParameter("k=", "k", value) && value.IsEmpty());
    CHECK(!FindQueryParameter("a=1&b=2", "A", value)); // 대소문자 구분
    CHECK(!FindQueryParameter("", "a", value));
    CHECK(!FindQueryParameter("ab=1", "a", value));
    CHECK(FindQueryParameter("=v", "", value) && Is(value, "v"));
}

} // namespace rocks::test
