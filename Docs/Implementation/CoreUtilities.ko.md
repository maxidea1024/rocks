# Core 범용 유틸리티 — 값·소유권·반복자·문자·인코딩·URI

기준일: 2026-11-11. 이 문서는 2026-11-11에 추가한 Core 범용 기능의 사용법과 계약을 기록한다. 실행 플랫폼과 미구현 범위는 각 절에 명시한다. 수학·기하·색·난수는 [수학 구현](Math.ko.md)을 따른다.

## 1. 값과 범위 정리

| 헤더 | UE 5.6 대응 | 계약 요약 |
|---|---|---|
| `Core/Optional.h` | `Misc/Optional.h` `TOptional` | `HasValue`(UE `IsSet`), 빈 상태의 `Value()`·`*`·`->`는 Fatal, `ValueOr`, `TryGet`(UE `GetPtrOrNull`), `Reset`, `Emplace`. 이동 전용 T 지원, 이동 후 원본은 보유 여부 유지(UE와 같음). `Emplace`는 새 값을 먼저 만든 뒤 교체하여 인자가 현재 값을 참조해도 안전하다 |
| `Core/ScopeExit.h` | `Misc/ScopeExit.h` `ON_SCOPE_EXIT` | `MakeScopeExit(f)` 또는 `ROCKS_ON_SCOPE_EXIT { ... };`. 정리 함수는 noexcept, `Dismiss()`로 취소, 복사·이동 불가 |

## 2. 소유권

`Core/Memory/UniquePtr.h`와 `Core/Memory/SharedPtr.h`는 Rocks 할당기 계약(`Allocator`)을 따른다. 전역 `new`를 쓰지 않고, 할당기는 모든 포인터보다 오래 살아야 하며, 저장소 반환 실패는 누수를 숨기지 않고 Fatal이다. GC 객체 참조(`RootRef`/`WeakRef`)와는 별개이며 Object 시스템 객체에 쓰지 않는다.

```cpp
auto created = rocks::SharedPtr<Texture>::TryMake(allocator, width, height);
if (!created) { return created.Error(); }
rocks::SharedPtr<Texture> texture = created.Value();
rocks::WeakPtr<Texture> observer = texture;
if (auto locked = observer.Lock()) { Use(*locked); }
```

- `UniquePtr<T>`(UE `TUniquePtr`): `TryMake(allocator, args...)`, 이동 전용, 가상 소멸자가 있는 기반 타입으로 이동 가능하며 원래 할당 주소를 보존해 다중 상속에서도 올바른 주소로 반환한다.
- `SharedPtr<T>`/`WeakPtr<T>`(UE `TSharedPtr`/`TWeakPtr` ThreadSafe 모드): 객체와 제어 블록을 한 번에 할당한다. 계수는 원자적이며(증가 relaxed, 감소 acq_rel), `WeakPtr::Lock`은 strong이 0이 아닐 때만 증가해 소멸 중 객체를 되살리지 않는다. 파생→기반 변환은 원래 타입 소멸 함수를 유지하므로 기반에 가상 소멸자가 없어도 된다. 강한 순환은 회수되지 않는다.
- `EnableSharedFromThis<T>`(UE `TSharedFromThis`): 객체는 weak 토큰만 보유한다. `SharedPtr::TryMake`로 만든 객체에서만 연결되며, 그 밖·생성자·소멸자 안에서는 빈 포인터를 반환한다(UE는 check 실패).
- `SharedRef<T>`(UE `TSharedRef` ThreadSafe 모드): 기본 생성이 없는 null 불가 공유 소유권이다. `TryMake`, `TryFrom(SharedPtr)`(빈 포인터는 InvalidArgument), `SharedPtr::ToSharedRef()`(빈 포인터는 Fatal, UE의 check 대응)로 만든다. UE와 같이 이동도 복사라 원본이 비지 않는다. `SharedPtr`로 암묵 변환되고 `WeakPtr`를 바로 만들 수 있으며 파생→기반 변환을 지원한다. 음성 대조 `shared-ref-from-empty`가 Fatal 진단을 확인한다.
- 후속: 사용자 deleter, aliasing 생성, `SharedFromThis`의 SharedRef 반환 변형.

## 3. STL 호환 반복자

엔진 런타임과 공개 API는 STL을 쓰지 않지만, 사용자 코드가 STL 알고리즘·`std::ranges`와 함께 쓸 수 있도록 컬렉션에 `begin()/end()/cbegin()/cend()`와 STL 중첩 타입(`value_type`, `size_type`, `difference_type`, `reference`, `iterator`, `const_iterator` 등)을 제공한다. `std::` 반복자 태그 없이 연산만으로 C++20 iterator concept이 성립한다.

| 타입 | 반복자 | concept(검증됨) | 무효화 |
|---|---|---|---|
| `Array<T>`, `Span<T>` | `T*` | `std::contiguous_iterator`, `std::span` 생성 가능 | 재할당·삽입·삭제·Reset |
| `String`, `StringView` | `const char*`(UTF-8 byte, 읽기 전용) | contiguous | 내용 변경·재할당 |
| `BitArray` | `bool`을 읽는 반복자 | `std::random_access_iterator` | 크기 변경·재할당. 쓰기는 `operator[]` |
| `HashMap<K, V>` | `{key, value}` 참조 쌍(`for (auto [key, value] : map)`) | `std::forward_iterator` | 구조 변경(삽입·삭제·Clear·Reserve·Compact·Reset·이동) 후 사용은 **Fatal** |
| `HashSet<K>` | `const K&` | `std::forward_iterator` | HashMap과 같음 |

- HashMap 반복자의 역참조는 값으로 반환되는 참조 쌍이므로 `auto` 또는 `const auto&`로 받는다. 구조 변경은 revision으로 탐지하며 `ForEach`와 달리 사전 차단이 아니라 다음 반복 연산에서 Fatal로 드러낸다(음성 대조 `hash-map-iterator-modified`).
- 검증: `Tests/Core/ContainerIteratorTests.cpp`(STL 없이), `Tests/Core/StlCompatibilityTests.cpp`(이 파일에서만 STL 헤더를 포함해 `std::sort`, `std::accumulate`, `std::ranges::*`, `std::span`, `std::find_if`와 concept `static_assert`로 확인). MSVC STL을 예외 없이 쓰려면 `_HAS_EXCEPTIONS=0`이 필요하다. macOS libc++ 경로는 미검증이다.

## 4. 문자(code point) 단위 문자열 처리

UTF-8 byte 인덱스 API(`TryLeftBytes` 등)와 별도로 `StringView`·`String`에 Unicode code point 단위 API를 제공한다.

```cpp
rocks::StringView name("가나다abc");
auto firstTwo = name.TryLeftCodePoints(2);   // "가나"
auto count = name.TryCountCodePoints();      // 6
auto points = name.TryCodePoints();          // for (char32_t point : points.Value())
```

- `TryCountCodePoints`, `TryLeftCodePoints`, `TryRightCodePoints`, `TryMidCodePoints`, `TryByteOffsetOfCodePoint`, `TryCodePoints`(char32_t 순회 range).
- 전체 UTF-8을 먼저 검증하며 잘못된 UTF-8은 InvalidArgument다. count·start는 byte API와 같이 길이로 제한한다.
- UE `Left/Right/Mid`는 TCHAR(Windows UTF-16 code unit) 단위라 BMP 밖 문자를 둘로 자를 수 있지만 이 API는 code point를 자르지 않는다.
- 화면상 한 글자가 여러 code point인 경우(결합 문자, 이모지 ZWJ 시퀀스, 국기)는 나뉜다. grapheme cluster 분할은 UAX #29 데이터가 필요하여 후속이다.

## 5. 인코딩·URI

| 헤더 | 내용 | 계약 |
|---|---|---|
| `Core/Encoding/Crc32.h` | zlib 호환 CRC-32(`"123456789"` → `0xCBF43926`), 분할 계산 | 256개 상수 표, 표는 테스트가 다항식 독립 생성 결과와 전부 대조. 비암호 |
| `Core/Encoding/Base64.h` | RFC 4648 Base64 인코딩·디코딩 | 엄격 디코딩(패딩·알파벳·비정규 bit 거부), 실패 시 출력 불변, 호출자 버퍼 |
| `Core/Text/Uri.h` | RFC 3986 URI 참조 분해(`TryParseUri`), 상대 참조 해석(`TryResolveUriReference`, §5.2–5.3 strict), percent encode/decode, query 파라미터 조회 | 할당 없이 입력을 빌리는 view, IPv6/IPvFuture literal·포트 범위·`%HH` 검증, IRI(비ASCII) 미지원, form 방식 `+`는 명시 모드. 해석은 dot segment를 제자리에서 제거하고 정규화(대소문자·percent·기본 포트)는 하지 않는다. 출력 버퍼는 결과 길이가 아니라 `ResolvedUriMaxSize`(base + reference + 2) 이상이어야 하며 실패 시 버퍼를 바꾸지 않는다. authority 없는 결과 path가 `//`로 시작하면 `/.`를 붙인다(RFC 미규정, WHATWG와 같은 처리). UE 5.6에서는 URL 함수가 Core가 아닌 HTTP 모듈(`FGenericPlatformHttp`)에 있다 |

검증(URI 해석): RFC 3986 §5.4.1·§5.4.2 예제 43개 전체, 경계·실패 사례, 그리고 RFC §5.2 의사코드를 그대로 옮긴 Python 참조 구현과 무작위 8,000쌍(절반 이상이 dot segment 포함) 차등 비교에서 불일치 0.

후속: URI 정규화(§6 대소문자·percent·기본 포트), URL-safe Base64, 압축·streaming.

## 6. 파일 위치 변경

`Path`는 문자열 처리 기능이므로 `Core/Path/Path.h`·`Private/Path/Path.cpp`에서 `Core/Text/Path.h`·`Private/Text/Path.cpp`로 옮겼다. API와 동작은 바뀌지 않았다.

## 7. 검증과 미검증

Windows x64에서 독립 러너(`/O2`·`/Od`)와 저장소 빌드 테스트로 실행했다. 실행 수치는 [Foundation 상태](FoundationStatus.ko.md)의 2026-11-11 기록을 따른다. macOS·ARM64 실행, `SharedPtr` 경합의 장시간 stress와 TSan은 미검증이다.
