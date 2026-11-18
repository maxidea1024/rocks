#pragma once

// json5-tests(Core/Source/ThirdParty/Json5Tests)에서 받는 사례마다 읽은 값이다. C++
// Json5Tests.cpp와 C# Json5ReaderTests가 함께 쓴다(표기·C 문법 제한은 Json5ParityCases.h 머리
// 주석과 같다). 받는 사례가 모두 있어야 하고 쓰지 않는 항목이 있으면 시험이 실패한다. 값을 바꿔야
// 하면 C++ 시험이 출력하는 줄을 검토해 옮긴다.
namespace rocks::test {

struct Json5SuiteExpectation {
    const char* path;
    const char* expected;
};

inline constexpr Json5SuiteExpectation Json5SuiteExpectations[] = {
    {"arrays/empty-array.json", "[]"},
    {"arrays/regular-array.json", "[true,false,null]"},
    {"arrays/trailing-comma-array.json5", "[null]"},
    {"comments/block-comment-following-array-element.json5", "[false]"},
    {"comments/block-comment-following-top-level-value.json5", "null"},
    {"comments/block-comment-in-string.json",
     "\"This /* block comment */ isn't really a block comment.\""},
    {"comments/block-comment-preceding-top-level-value.json5", "null"},
    {"comments/block-comment-with-asterisks.json5", "true"},
    {"comments/inline-comment-following-array-element.json5", "[false]"},
    {"comments/inline-comment-following-top-level-value.json5", "null"},
    {"comments/inline-comment-in-string.json",
     "\"This inline comment // isn't really an inline comment.\""},
    {"comments/inline-comment-preceding-top-level-value.json5", "null"},
    {"comments/irregular-block-comment.json", "true"},
    {"misc/npm-package.json",
     "{\"name\":\"npm\",\"publishConfig\":{\"proprietary-attribs\":false},\"description\":\"A "
     "package manager for node\",\"keywords\":[\"package "
     "manager\",\"modules\",\"install\",\"package.json\"],\"version\":\"1.1.22\",\"preferGlobal\":"
     "true,\"config\":{\"publishtest\":false},\"homepage\":\"http://npmjs.org/"
     "\",\"author\":\"Isaac Z. Schlueter <i@izs.me> "
     "(http://blog.izs.me)\",\"repository\":{\"type\":\"git\",\"url\":\"https://github.com/isaacs/"
     "npm\"},\"bugs\":{\"email\":\"npm-@googlegroups.com\",\"url\":\"http://github.com/isaacs/npm/"
     "issues\"},\"directories\":{\"doc\":\"./doc\",\"man\":\"./man\",\"lib\":\"./lib\",\"bin\":\"./"
     "bin\"},\"main\":\"./lib/npm.js\",\"bin\":\"./bin/"
     "npm-cli.js\",\"dependencies\":{\"semver\":\"~1.0.14\",\"ini\":\"1\",\"slide\":\"1\","
     "\"abbrev\":\"1\",\"graceful-fs\":\"~1.1.1\",\"minimatch\":\"~0.2\",\"nopt\":\"1\",\"node-"
     "uuid\":\"~1.3\",\"proto-list\":\"1\",\"rimraf\":\"2\",\"request\":\"~2.9\",\"which\":\"1\","
     "\"tar\":\"~0.1.12\",\"fstream\":\"~0.1.17\",\"block-stream\":\"*\",\"inherits\":\"1\","
     "\"mkdirp\":\"0.3\",\"read\":\"0\",\"lru-cache\":\"1\",\"node-gyp\":\"~0.4.1\",\"fstream-"
     "npm\":\"0 "
     ">=0.0.5\",\"uid-number\":\"0\",\"archy\":\"0\",\"chownr\":\"0\"},\"bundleDependencies\":["
     "\"slide\",\"ini\",\"semver\",\"abbrev\",\"graceful-fs\",\"minimatch\",\"nopt\",\"node-uuid\","
     "\"rimraf\",\"request\",\"proto-list\",\"which\",\"tar\",\"fstream\",\"block-stream\","
     "\"inherits\",\"mkdirp\",\"read\",\"lru-cache\",\"node-gyp\",\"fstream-npm\",\"uid-number\","
     "\"archy\",\"chownr\"],\"devDependencies\":{\"ronn\":\"https://github.com/isaacs/ronnjs/"
     "tarball/master\"},\"engines\":{\"node\":\"0.6 || 0.7 || "
     "0.8\",\"npm\":\"1\"},\"scripts\":{\"test\":\"node ./test/run.js\",\"prepublish\":\"npm "
     "prune; rm -rf node_modules/*/{test,example,bench}*; make -j4 doc\",\"dumpconf\":\"env | grep "
     "npm | sort | uniq\"},\"licenses\":[{\"type\":\"MIT "
     "+no-false-attribs\",\"url\":\"http://github.com/isaacs/npm/raw/master/LICENSE\"}]}"},
    {"misc/npm-package.json5",
     "{\"name\":\"npm\",\"publishConfig\":{\"proprietary-attribs\":false},\"description\":\"A "
     "package manager for node\",\"keywords\":[\"package "
     "manager\",\"modules\",\"install\",\"package.json\"],\"version\":\"1.1.22\",\"preferGlobal\":"
     "true,\"config\":{\"publishtest\":false},\"homepage\":\"http://npmjs.org/"
     "\",\"author\":\"Isaac Z. Schlueter <i@izs.me> "
     "(http://blog.izs.me)\",\"repository\":{\"type\":\"git\",\"url\":\"https://github.com/isaacs/"
     "npm\"},\"bugs\":{\"email\":\"npm-@googlegroups.com\",\"url\":\"http://github.com/isaacs/npm/"
     "issues\"},\"directories\":{\"doc\":\"./doc\",\"man\":\"./man\",\"lib\":\"./lib\",\"bin\":\"./"
     "bin\"},\"main\":\"./lib/npm.js\",\"bin\":\"./bin/"
     "npm-cli.js\",\"dependencies\":{\"semver\":\"~1.0.14\",\"ini\":\"1\",\"slide\":\"1\","
     "\"abbrev\":\"1\",\"graceful-fs\":\"~1.1.1\",\"minimatch\":\"~0.2\",\"nopt\":\"1\",\"node-"
     "uuid\":\"~1.3\",\"proto-list\":\"1\",\"rimraf\":\"2\",\"request\":\"~2.9\",\"which\":\"1\","
     "\"tar\":\"~0.1.12\",\"fstream\":\"~0.1.17\",\"block-stream\":\"*\",\"inherits\":\"1\","
     "\"mkdirp\":\"0.3\",\"read\":\"0\",\"lru-cache\":\"1\",\"node-gyp\":\"~0.4.1\",\"fstream-"
     "npm\":\"0 "
     ">=0.0.5\",\"uid-number\":\"0\",\"archy\":\"0\",\"chownr\":\"0\"},\"bundleDependencies\":["
     "\"slide\",\"ini\",\"semver\",\"abbrev\",\"graceful-fs\",\"minimatch\",\"nopt\",\"node-uuid\","
     "\"rimraf\",\"request\",\"proto-list\",\"which\",\"tar\",\"fstream\",\"block-stream\","
     "\"inherits\",\"mkdirp\",\"read\",\"lru-cache\",\"node-gyp\",\"fstream-npm\",\"uid-number\","
     "\"archy\",\"chownr\"],\"devDependencies\":{\"ronn\":\"https://github.com/isaacs/ronnjs/"
     "tarball/master\"},\"engines\":{\"node\":\"0.6 || 0.7 || "
     "0.8\",\"npm\":\"1\"},\"scripts\":{\"test\":\"node ./test/run.js\",\"prepublish\":\"npm "
     "prune; rm -rf node_modules/*/{test,example,bench}*; make -j4 doc\",\"dumpconf\":\"env | grep "
     "npm | sort | uniq\"},\"licenses\":[{\"type\":\"MIT "
     "+no-false-attribs\",\"url\":\"http://github.com/isaacs/npm/raw/master/LICENSE\"}]}"},
    {"misc/readme-example.json5",
     "{\"foo\":\"bar\",\"while\":true,\"this\":\"is a multi-line string\",\"here\":\"is "
     "another\",\"hex\":i3735928559,\"half\":d3fe0000000000000,\"delta\":i10,\"to\":"
     "d7ff0000000000000,\"finally\":\"a trailing comma\",\"oh\":[\"we shouldn't forget\",\"arrays "
     "can have\",\"trailing commas too\"]}"},
    {"misc/valid-whitespace.json5", "{\"a\":true}"},
    {"new-lines/comment-cr.json5", "{}"},
    {"new-lines/comment-crlf.json5", "{}"},
    {"new-lines/comment-lf.json5", "{}"},
    {"new-lines/escaped-cr.json5", "{\"a\":\"line 1 line 2\"}"},
    {"new-lines/escaped-crlf.json5", "{\"a\":\"line 1 line 2\"}"},
    {"new-lines/escaped-lf.json5", "{\"a\":\"line 1 line 2\"}"},
    {"numbers/float-leading-decimal-point.json5", "d3fe0000000000000"},
    {"numbers/float-leading-zero.json", "d3fe0000000000000"},
    {"numbers/float-trailing-decimal-point-with-integer-exponent.json5", "d40e86a0000000000"},
    {"numbers/float-trailing-decimal-point.json5", "d4014000000000000"},
    {"numbers/float-with-integer-exponent.json", "d4092c00000000000"},
    {"numbers/float.json", "d3ff3333333333333"},
    {"numbers/hexadecimal-lowercase-letter.json5", "i200"},
    {"numbers/hexadecimal-uppercase-x.json5", "i200"},
    {"numbers/hexadecimal-with-integer-exponent.json5", "i51428"},
    {"numbers/hexadecimal.json5", "i200"},
    {"numbers/infinity.json5", "d7ff0000000000000"},
    {"numbers/integer-with-integer-exponent.json", "d44c52d02c7e14af6"},
    {"numbers/integer-with-negative-integer-exponent.json", "d3b382db34012b251"},
    {"numbers/integer-with-negative-zero-integer-exponent.json", "d4014000000000000"},
    {"numbers/integer-with-positive-integer-exponent.json", "d4059000000000000"},
    {"numbers/integer-with-positive-zero-integer-exponent.json", "d4014000000000000"},
    {"numbers/integer-with-zero-integer-exponent.json", "d4014000000000000"},
    {"numbers/integer.json", "i15"},
    {"numbers/nan.json5", "d7ff8000000000000"},
    {"numbers/negative-float-leading-decimal-point.json5", "dbfe0000000000000"},
    {"numbers/negative-float-leading-zero.json", "dbfe0000000000000"},
    {"numbers/negative-float-trailing-decimal-point.json5", "dc014000000000000"},
    {"numbers/negative-float.json", "dbff3333333333333"},
    {"numbers/negative-hexadecimal.json5", "i-200"},
    {"numbers/negative-infinity.json5", "dfff0000000000000"},
    {"numbers/negative-integer.json", "i-15"},
    {"numbers/negative-zero-float-leading-decimal-point.json5", "d8000000000000000"},
    {"numbers/negative-zero-float-trailing-decimal-point.json5", "d8000000000000000"},
    {"numbers/negative-zero-float.json", "d8000000000000000"},
    {"numbers/negative-zero-hexadecimal.json5", "d8000000000000000"},
    {"numbers/negative-zero-integer.json", "d8000000000000000"},
    {"numbers/positive-float-leading-decimal-point.json5", "d3fe0000000000000"},
    {"numbers/positive-float-leading-zero.json5", "d3fe0000000000000"},
    {"numbers/positive-float-trailing-decimal-point.json5", "d4014000000000000"},
    {"numbers/positive-float.json5", "d3ff3333333333333"},
    {"numbers/positive-hexadecimal.json5", "i200"},
    {"numbers/positive-infinity.json5", "d7ff0000000000000"},
    {"numbers/positive-integer.json5", "i15"},
    {"numbers/positive-zero-float-leading-decimal-point.json5", "d0000000000000000"},
    {"numbers/positive-zero-float-trailing-decimal-point.json5", "d0000000000000000"},
    {"numbers/positive-zero-float.json5", "d0000000000000000"},
    {"numbers/positive-zero-hexadecimal.json5", "i0"},
    {"numbers/positive-zero-integer.json5", "i0"},
    {"numbers/zero-float-leading-decimal-point.json5", "d0000000000000000"},
    {"numbers/zero-float-trailing-decimal-point.json5", "d0000000000000000"},
    {"numbers/zero-float.json", "d0000000000000000"},
    {"numbers/zero-hexadecimal.json5", "i0"},
    {"numbers/zero-integer-with-integer-exponent.json", "d0000000000000000"},
    {"numbers/zero-integer.json", "i0"},
    {"objects/empty-object.json", "{}"},
    {"objects/reserved-unquoted-key.json5", "{\"while\":true}"},
    {"objects/single-quoted-key.json5", "{\"hello\":\"world\"}"},
    {"objects/trailing-comma-object.json5", "{\"foo\":\"bar\"}"},
    {"objects/unquoted-keys.json5", "{\"hello\":\"world\",\"_\":\"underscore\",\"$\":\"dollar "
                                    "sign\",\"one1\":\"numerals\",\"_$_\":\"multiple "
                                    "symbols\",\"$_$hello123world_$_\":\"mixed\"}"},
    {"strings/escaped-single-quoted-string.json5", "\"I can't wait\""},
    {"strings/multi-line-string.json5", "\"hello world\""},
    {"strings/single-quoted-string.json5", "\"hello world\""},
    {"todo/unicode-escaped-unquoted-key.json5", "{\"sig\\u{3A3}ma\":\"the sum of all things\"}"},
    {"todo/unicode-unquoted-key.json5",
     "{\"\\u{FC}ml\\u{E5}\\u{FB}t\":\"that's not really an \\u{FC}mla\\u{FC}t, but this is\"}"},
};

} // namespace rocks::test
