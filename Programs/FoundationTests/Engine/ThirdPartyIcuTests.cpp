// ICU(Engine/Source/ThirdParty/Icu)와 데이터 부분집합(IcuData)의 연결 시험이다(Docs/Design/Font
// 8.3·8.5절, FT1c).
#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/File.h>
#include <TestSupport/Test.h>
#include <new>
#include <unicode/ubidi.h>
#include <unicode/ubrk.h>
#include <unicode/uchar.h>
#include <unicode/udata.h>
#include <unicode/uscript.h>
#include <unicode/ustring.h>
#include <unicode/uversion.h>

using namespace rocks;
namespace third_party_icu_tests_file {

namespace {

constexpr const char* IcuDataPath =
    "Engine/Source/ThirdParty/IcuData/icudata-79.1-rocks/icudt79l.dat";

bool ReadFile(const char* path, Array<uint8>& out) noexcept {
    platform::File file;
    if (!file.Open(path, platform::FileOpenMode::ReadExisting)) {
        return false;
    }

    uint64 size = 0;
    bool ok = static_cast<bool>(file.Size(size)) &&
              static_cast<bool>(out.TryResize(static_cast<usize>(size)));
    if (ok && size != 0) {
        ok = static_cast<bool>(file.ReadExact(out.Data(), static_cast<usize>(size)).status);
    }

    const bool closed = static_cast<bool>(file.Close());
    return ok && closed;
}

// 데이터 패키지를 프로세스에 한 번 등록한다. ICU는 등록한 메모리를 계속 쓰므로 버퍼는 프로세스
// 끝까지 산다(소멸하지 않는 정적 저장소). 등록과 할당 hook(u_setMemoryFunctions)·u_cleanup은
// 프로세스 전역이라 시험마다 바꾸지 않는다 — HarfBuzz(hb-icu)가 ICU 정규화 객체를 붙잡으므로
// u_cleanup은 그것을 끊는다. hook 연결과 종료 정리는 글꼴 backend의 시작·종료 경로와 함께 FT2에서
// 시험한다(Docs/Design/Font 8.5절).
bool RegisterIcuData() noexcept {
    alignas(SystemAllocator) static unsigned char storage[sizeof(SystemAllocator)];
    static SystemAllocator* allocator = ::new (static_cast<void*>(storage)) SystemAllocator();
    alignas(Array<uint8>) static unsigned char arrayStorage[sizeof(Array<uint8>)];
    static Array<uint8>* data = ::new (static_cast<void*>(arrayStorage)) Array<uint8>(*allocator);
    static const bool registered = [] {
        if (!ReadFile(IcuDataPath, *data) || data->Size() == 0) {
            return false;
        }

        UErrorCode status = U_ZERO_ERROR;
        udata_setCommonData(data->Data(), &status);
        return U_SUCCESS(status) != 0;
    }();
    return registered;
}

// UTF-8 리터럴을 UTF-16 버퍼로 바꾼다(ICU 경계 API는 UTF-16을 받는다).
int32_t ToUtf16(const char* text, UChar* out, int32_t capacity) {
    UErrorCode status = U_ZERO_ERROR;
    int32_t length = 0;
    u_strFromUTF8(out, capacity, &length, text, -1, &status);
    return U_SUCCESS(status) ? length : -1;
}

// 경계 수(시작 0을 뺀 경계 위치 수)를 센다.
int32_t CountBoundaries(UBreakIteratorType type, const char* locale, const UChar* text,
                        int32_t length) {
    UErrorCode status = U_ZERO_ERROR;
    UBreakIterator* iterator = ubrk_open(type, locale, text, length, &status);
    if (U_FAILURE(status) || iterator == nullptr) {
        return -1;
    }

    int32_t count = 0;
    while (ubrk_next(iterator) != UBRK_DONE) {
        ++count;
    }

    ubrk_close(iterator);
    return count;
}

constexpr int32_t TextCapacity = 64;

} // namespace

TEST(ThirdParty_IcuVersionAndBuiltInProperties) {
    UVersionInfo version;
    u_getVersion(version);
    CHECK(version[0] == 79 && version[1] == 1);
    // 문자 속성은 common에 내장되어 데이터 등록 없이 동작한다(Docs/Design/Font 8.3절).
    CHECK(u_charType(0x0041) == U_UPPERCASE_LETTER);
    CHECK(u_charType(0xAC00) == U_OTHER_LETTER);
    UErrorCode status = U_ZERO_ERROR;
    CHECK(uscript_getScript(0xAC00, &status) == USCRIPT_HANGUL && U_SUCCESS(status));
    CHECK(u_charDirection(0x05D0) == U_RIGHT_TO_LEFT);
}

TEST(ThirdParty_IcuDataSubsetBoundariesAndBidi) {
    CHECK(RegisterIcuData());
    UErrorCode status = U_ZERO_ERROR;

    UChar text[TextCapacity];
    // 줄 경계: "hello world"는 공백 뒤와 끝에서 나눌 수 있다(경계 2개).
    int32_t length = ToUtf16("hello world", text, TextCapacity);
    CHECK(length == 11);
    CHECK(CountBoundaries(UBRK_LINE, "en", text, length) == 2);

    // 단어 경계: 한국어 두 어절("안녕하세요 세계")은 어절 끝·공백 끝·문자열 끝(경계 3개)이다.
    length = ToUtf16(
        "\xEC\x95\x88\xEB\x85\x95\xED\x95\x98\xEC\x84\xB8\xEC\x9A\x94 \xEC\x84\xB8\xEA\xB3\x84",
        text, TextCapacity);
    CHECK(length == 8);
    CHECK(CountBoundaries(UBRK_WORD, "ko", text, length) == 3);

    // 단어 경계: 태국어는 공백이 없어 사전(thaidict)이 단어를 나눈다("สวัสดีครับ" = สวัสดี + ครับ → 경계
    // 2개).
    length = ToUtf16("\xE0\xB8\xAA\xE0\xB8\xA7\xE0\xB8\xB1\xE0\xB8\xAA\xE0\xB8\x94\xE0\xB8\xB5"
                     "\xE0\xB8\x84\xE0\xB8\xA3\xE0\xB8\xB1\xE0\xB8\x9A",
                     text, TextCapacity);
    CHECK(length > 0);
    CHECK(CountBoundaries(UBRK_WORD, "th", text, length) == 2);

    // 문자(grapheme) 경계: 가족 이모지 ZWJ 열(U+1F468 ZWJ U+1F469 ZWJ U+1F467)은 한 grapheme이다.
    length = ToUtf16("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7",
                     text, TextCapacity);
    CHECK(length == 8);
    CHECK(CountBoundaries(UBRK_CHARACTER, "en", text, length) == 1);

    // BiDi: "abc אבג"는 LTR 문단에서 방향 run 2개다.
    length = ToUtf16("abc \xD7\x90\xD7\x91\xD7\x92", text, TextCapacity);
    UBiDi* bidi = ubidi_open();
    CHECK(bidi != nullptr);
    if (bidi != nullptr) {
        ubidi_setPara(bidi, text, length, UBIDI_DEFAULT_LTR, nullptr, &status);
        CHECK(U_SUCCESS(status));
        CHECK(ubidi_countRuns(bidi, &status) == 2);
        CHECK(ubidi_getDirection(bidi) == UBIDI_MIXED);
        ubidi_close(bidi);
    }
}

} // namespace third_party_icu_tests_file
