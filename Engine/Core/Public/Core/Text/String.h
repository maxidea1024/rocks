#pragma once

#include <Core/Memory/Allocator.h>
#include <Core/Text/StringView.h>

namespace rocks {

// Allocator를 빌리고 유효한 UTF-8 payload와 마지막 NUL을 소유한다.
// 소유권은 이동으로만 전달한다. allocator는 문자열과 진행 중 호출보다 오래 살아야 한다.
// allocator 콜백에서 같은 String을 변경하는 재진입은 금지한다.
// 같은 String의 동시 읽기/쓰기는 caller가 동기화하며 allocator의 thread 계약도 지킨다.
class String {
public:
    explicit String(Allocator& allocator) noexcept;
    ~String() noexcept;

    String(const String&) = delete;
    String& operator=(const String&) = delete;
    String(String&& other) noexcept;
    String& operator=(String&& other) noexcept;

    // 다른 allocator에 명시적으로 복제한다. 실패해도 이 문자열은 변하지 않는다.
    Result<String> TryClone(Allocator& allocator) const noexcept;

    // 유효하지 않은 UTF-8, overflow, OOM이면 내용·capacity·기존 주소를 보존한다.
    // 자기 전체/부분 view도 허용하며 성장 시 source를 읽은 뒤 이전 storage를 해제한다.
    Result<void> TryAssign(StringView source) noexcept;
    Result<void> TryAppend(StringView source) noexcept;

    // 검색과 분리는 길이 기반 byte 연산이다. 포함 NUL도 검색하며 기본 대소문자 무시는
    // ASCII에 한정된다. 실패할 수 없는 질의는 Result 없이 바로 if에 사용할 수 있다.
    usize Find(StringView needle, StringComparison comparison = StringComparison::IgnoreAsciiCase,
               StringSearchDirection direction = StringSearchDirection::FromStart,
               usize startByte = StringView::NotFound) const noexcept {
        return View().Find(needle, comparison, direction, startByte);
    }
    bool Contains(StringView needle,
                  StringComparison comparison = StringComparison::IgnoreAsciiCase) const noexcept {
        return View().Contains(needle, comparison);
    }
    bool
    StartsWith(StringView prefix,
               StringComparison comparison = StringComparison::IgnoreAsciiCase) const noexcept {
        return View().StartsWith(prefix, comparison);
    }
    bool EndsWith(StringView suffix,
                  StringComparison comparison = StringComparison::IgnoreAsciiCase) const noexcept {
        return View().EndsWith(suffix, comparison);
    }
    // 반환 view는 byte 단위이므로 delimiter가 UTF-8 문자 중간이면 유효한 UTF-8이 아닐 수 있다.
    bool Split(StringView delimiter, StringView& left, StringView& right,
               StringComparison comparison = StringComparison::IgnoreAsciiCase,
               StringSearchDirection direction = StringSearchDirection::FromStart) const noexcept {
        return View().Split(delimiter, left, right, comparison, direction);
    }

    // byte 위치가 UTF-8 code point 경계가 아니면 InvalidArgument다. 결과는 이 문자열의
    // storage를 빌리므로 원본 변경·재할당·파괴 전에만 사용한다.
    Result<StringView> TryLeftBytes(usize count) const noexcept {
        return View().TryLeftBytes(count);
    }
    Result<StringView> TryRightBytes(usize count) const noexcept {
        return View().TryRightBytes(count);
    }
    Result<StringView> TryMidBytes(usize start, usize count = MaxSize) const noexcept {
        return View().TryMidBytes(start, count);
    }

    // 문자(code point) 단위 조회다. 계약은 StringView의 같은 함수와 같고 결과는 이 String을 빌린다.
    Result<usize> TryCountCodePoints() const noexcept {
        return View().TryCountCodePoints();
    }
    Result<StringView> TryLeftCodePoints(usize count) const noexcept {
        return View().TryLeftCodePoints(count);
    }
    Result<StringView> TryRightCodePoints(usize count) const noexcept {
        return View().TryRightCodePoints(count);
    }
    Result<StringView> TryMidCodePoints(usize start, usize count = MaxSize) const noexcept {
        return View().TryMidCodePoints(start, count);
    }

    // ASCII 공백만 제거하거나 ASCII 대소문자만 제자리 변환한다. 저장소를 새로 할당하지
    // 않으며 Unicode 공백/대소문자는 변경하지 않는다. 기존 내용 view는 변경 후 다시 얻는다.
    void TrimAsciiStart() noexcept;
    void TrimAsciiEnd() noexcept;
    void TrimAscii() noexcept;
    void ToUpperAscii() noexcept;
    void ToLowerAscii() noexcept;

    // 비중첩 일치 부분을 왼쪽에서 오른쪽으로 치환하고 치환 수를 반환한다. 빈 검색어는
    // 변경 없이 0이다. 대소문자 구분 비교에서 from과 to가 같으면 무할당으로 0이다.
    // from/to는 이 String의 view여도 된다. 잘못된 UTF-8, 크기 초과,
    // 할당 실패는 기존 내용·주소·capacity를 보존한다. ASCII 밖은 byte 그대로 비교한다.
    // 치환 결과가 저장소를 교체하면 기존 view는 무효다. StackAllocator는 String의 기존
    // 성장 경로와 동일한 LIFO 제약을 따르므로 이 교체 연산에 범용 backing을 사용한다.
    Result<usize>
    TryReplace(StringView from, StringView to,
               StringComparison comparison = StringComparison::IgnoreAsciiCase) noexcept;

    // capacity는 마지막 종단 NUL을 제외한 payload byte 수다. 실제 재할당 성공 시
    // 이전 pointer/view는 무효다. 공간이 이미 충분하거나 실패하면 주소도 보존한다.
    Result<void> TryReserve(usize byteCapacity) noexcept;

    // payload를 비우고 종단 NUL을 복원한다. buffer는 재사용하며 기존 내용 view는 무효다.
    void Clear() noexcept;

    // storage와 allocator를 함께 교환한다. 할당/해제 callback 없이 commit할 때 사용한다.
    // 기존 pointer는 원래 storage를 계속 가리키지만 소유 String이 서로 바뀐다.
    void Swap(String& other) noexcept;
    Allocator& GetAllocator() const noexcept;

    const char* Data() const noexcept;
    usize ByteCount() const noexcept;

    // ---- STL 호환 별칭과 byte 반복자 ----
    // UTF-8 불변식을 지키기 위해 읽기 전용 byte 반복자(const char*)만 제공한다. 문자(code point)
    // 단위 순회는 View().CodePoints()를 사용한다. 내용 변경·재할당은 반복자를 무효화한다.
    using value_type = char;
    using size_type = usize;
    using difference_type = isize;
    using const_pointer = const char*;
    using const_reference = const char&;
    using const_iterator = const char*;
    using iterator = const char*;

    const char* begin() const noexcept {
        return Data();
    }

    const char* end() const noexcept {
        return Data() + ByteCount();
    }

    const char* cbegin() const noexcept {
        return begin();
    }

    const char* cend() const noexcept {
        return end();
    }
    usize ByteCapacity() const noexcept;
    bool IsEmpty() const noexcept;
    StringView View() const noexcept;

    // 항상 non-null이고 ByteCount() 위치에 NUL이 있다. embedded NUL도 payload에 허용하므로
    // C API는 첫 NUL까지만 볼 수 있다. 전체 payload를 전달하려면 View/ByteCount를 사용한다.
    // 반환 pointer는 빌린 값이며 재할당·Clear·대입·소멸에 따른 무효화를 지켜야 한다.
    const char* CStr() const noexcept;

private:
    friend class Path;

    // Path가 UTF-8·NUL·산술·용량 검사를 마친 뒤에만 호출한다. 원본 suffix를 먼저
    // 이동하므로 기존 종단 NUL 뒤의 자기 버퍼를 빌려도 내용을 잃지 않는다.
    void AppendPathInPlace(StringView suffix, bool separator) noexcept;
    Result<char*> AllocateBuffer(usize byteCapacity) noexcept;
    usize GrowthCapacity(usize required) const noexcept;
    void FreeBuffer(char* buffer) noexcept;
    void Release() noexcept;

    Allocator* allocator_;
    char* data_ = nullptr;
    usize bytes_ = 0;
    usize capacity_ = 0;
};

// UE 스타일 경로 결합: 기준 String의 allocator를 빌려 새 문자열을 반환한다.
// 구분자는 필요할 때만 '/'를 추가하고, 그 외 원본 표기·'.'·'..'는 바꾸지 않는다.
// 빈 오른쪽은 비어 있지 않은 왼쪽 끝에 구분자를 추가한다. 루트가 있는 오른쪽도
// 리셋하거나 제거하지 않고 그대로 붙인다. NUL·무효 UTF-8 또는 할당 실패는 Result 오류다.
// 사용 예: auto result = root / "Content" / "Textures"; if (!result) { ... }
// 연쇄 중 최초 오류는 유지된다. 양쪽이 String이면 왼쪽 allocator를 쓰고,
// StringView / String은 오른쪽 allocator를 쓴다. 원본과 allocator는 호출 중 살아 있어야 한다.
Result<String> operator/(const String& left, StringView right) noexcept;
Result<String> operator/(const String& left, const String& right) noexcept;
Result<String> operator/(StringView left, const String& right) noexcept;
Result<String> operator/(Result<String>&& left, StringView right) noexcept;
Result<String> operator/(Result<String>&& left, const String& right) noexcept;

// 실패 시 왼쪽의 내용·버퍼 주소·allocator는 그대로다. 오른쪽이 왼쪽의 view여도 된다.
// 여유 용량 내에서는 자기 buffer를 빌린 suffix도 할당 없이 처리하고 주소를 보존한다.
// 새 저장소가 필요한 연산은 기존 String의 allocator 반환 순서 규약을 따른다.
// 특히 StackAllocator는 버퍼 성장 시 LIFO 반환이 어려우므로 미리 용량을 확보해야 한다.
// 반환 결과를 확인해야 하며, 객체 수명과 외부 접근 동기화는 String 규약을 따른다.
Result<void> operator/=(String& left, StringView right) noexcept;
Result<void> operator/=(String& left, const String& right) noexcept;

} // namespace rocks
