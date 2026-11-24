// ICU(Engine/Source/ThirdParty/Icu)와 데이터 부분집합(IcuData)의 연결 시험이다(Docs/Design/Font
// 8.3·8.5절, FT1c). 데이터 등록(udata_setCommonData)과 할당 hook(u_setMemoryFunctions)은 프로세스
// 전역이라, 데이터를 쓰는 확인은 시험 하나 안에서 등록 → 확인 → u_cleanup → 해제까지 끝낸다.
#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/File.h>
#include <TestSupport/Test.h>
#include <stddef.h>
#include <string.h>
#include <unicode/ubidi.h>
#include <unicode/ubrk.h>
#include <unicode/uchar.h>
#include <unicode/uclean.h>
#include <unicode/udata.h>
#include <unicode/uscript.h>
#include <unicode/ustring.h>
#include <unicode/uversion.h>

using namespace rocks;
namespace third_party_icu_tests_file {

namespace {

constexpr const char* IcuDataPath =
    "Engine/Source/ThirdParty/IcuData/icudata-79.1-rocks/icudt79l.dat";

// ICU 블록 정렬(malloc과 같은 보장)이다.
constexpr usize IcuBlockAlignment = alignof(max_align_t);

// ICU realloc hook은 이전 크기를 주지 않으므로, 블록 앞에 크기를 적어 둔다(엔진 backend도 같은
// 방식).
constexpr usize IcuBlockHeader = alignof(max_align_t);

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

struct CountingMemory {
    SystemAllocator allocator;
    int64 live = 0;
    int64 total = 0;
};

void* U_CALLCONV CountingAlloc(const void* context, size_t size) {
    auto* counter = static_cast<CountingMemory*>(const_cast<void*>(context));
    Result<void*> block = counter->allocator.Allocate(IcuBlockHeader + size, IcuBlockAlignment);
    if (!block) {
        return nullptr;
    }

    ++counter->live;
    ++counter->total;
    auto* bytes = static_cast<uint8*>(block.Value());
    memcpy(bytes, &size, sizeof(size));
    return bytes + IcuBlockHeader;
}

void U_CALLCONV CountingFree(const void* context, void* memory) {
    auto* counter = static_cast<CountingMemory*>(const_cast<void*>(context));
    if (memory == nullptr) {
        return;
    }

    --counter->live;
    (void)counter->allocator.Deallocate(static_cast<uint8*>(memory) - IcuBlockHeader);
}

void* U_CALLCONV CountingRealloc(const void* context, void* memory, size_t size) {
    void* resized = CountingAlloc(context, size);
    if (resized == nullptr || memory == nullptr) {
        return resized;
    }

    size_t previous = 0;
    memcpy(&previous, static_cast<uint8*>(memory) - IcuBlockHeader, sizeof(previous));
    memcpy(resized, memory, previous < size ? previous : size);
    CountingFree(context, memory);
    return resized;
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
    u_cleanup();
}

TEST(ThirdParty_IcuDataSubsetBoundariesAndBidi) {
    SystemAllocator fileAllocator;
    Array<uint8> data(fileAllocator);
    CHECK(ReadFile(IcuDataPath, data));
    if (data.Size() == 0) {
        return;
    }

    CountingMemory memory;
    UErrorCode status = U_ZERO_ERROR;
    u_setMemoryFunctions(&memory, CountingAlloc, CountingRealloc, CountingFree, &status);
    CHECK(U_SUCCESS(status));
    udata_setCommonData(data.Data(), &status);
    CHECK(U_SUCCESS(status));

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

    CHECK(memory.total > 0);
    // u_cleanup은 ICU cache를 모두 놓고 데이터 등록·할당 hook도 되돌린다(뒤 시험이 다시 등록할 수
    // 있다).
    u_cleanup();
    CHECK(memory.live == 0);
}

} // namespace third_party_icu_tests_file
