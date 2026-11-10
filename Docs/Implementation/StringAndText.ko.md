# Rocks 문자열·식별자·지역화 경계

기준일: 2026-11-10. C13의 String/StringView·Name intern과 별도 Localization의 Text 기반을 구현했다. `FString`/`FText`와 같은 역할 분리는 필요하되 이름과 구현은 Rocks의 소유권·오류 규약을 따른다. 엔진 소유 Runtime과 공개 API에 STL 문자열을 사용하지 않는다.

## 역할과 의존성

| 타입/계층 | 역할 | 소유권·경계 | 현재 단계 |
|---|---|---|---|
| `rocks::StringView` | 길이 기반 UTF-8 바이트 열 참조 | 원본을 소유하지 않음, 비동기 보관 시 소유 복사 필요 | Core 구현 범위 |
| `rocks::String` | 유효 UTF-8 소유 문자열 | 명시 Allocator, 실패 가능한 복사·변경은 Result, byte 수와 마지막 NUL 분리 | Core 구현 범위 |
| `rocks::Name` | 자주 비교하는 이름의 intern 식별자 | 명시 pool 수명, 저장/전송에는 원문 또는 stable ID | NamePool/NameLease 구현 |
| `rocks::Text` | 번역·서식 이력을 유지하는 사용자 표시 문구 | 안정 ID·불변 원문/인자 소유, 언어/catalog snapshot과 함께 resolve | 별도 Localization 구현 |
| `Path`/플랫폼 adapter | OS 경로 및 외부 API 변환 | Windows UTF-16, macOS 경로 정책을 명시 변환 | 후속 PlatformFile |
| UI text layout | grapheme, BiDi, 글꼴 fallback, shaping | 검증된 외부 text 라이브러리를 adapter로 격리 | 후속 UI |

상위 소비자가 의존하는 방향은 `UI/Studio → Localization → Core.Text`다. Core가 번역 파일·폰트·UI에 의존하지 않는다. 문자열 자체가 GC Object일 필요는 없다. Core는 지역화와 관계없는 UTF-8 버퍼 수명만 책임진다.

## String/StringView 계약

- UTF-8 내부 표현, byte 단위 길이를 명시한다. byte 수를 사용자에게 보이는 글자 수로 해석하지 않는다.
- `StringView`의 pointer/count는 호출자가 유효한 실제 메모리 범위를 제공해야 한다. 뷰만으로 원본 수명을 검증할 수 없다. null+0은 빈 값이며 null+nonzero는 생성 시 Fatal로 처리하는 프로그래머 계약 위반이다. UTF-8 내용 오류는 `ValidateUtf8`/문자열 변경 API의 Result로 반환한다.
- `String`의 변경 진입점은 UTF-8을 검사한다. overlong, 잘린 sequence, 잘못된 continuation, surrogate, U+10FFFF 초과를 거부한다. 정규화나 대체문자 삽입은 암묵적으로 수행하지 않는다.
- 문자열 내부 NUL을 허용한다. 길이 기반 API에서는 데이터로 취급하되 C 문자열 소비자는 첫 NUL까지만 읽는다는 차이를 명시한다. OS 경로/API adapter는 해당 경계에서 내부 NUL을 거부한다.
- `CStr`의 종단 NUL은 ByteCount에서 제외한다. 빈 문자열도 읽을 수 있는 종단 포인터를 제공한다.
- move-only 소유와 명시적 `TryClone`을 사용하여 숨은 할당 실패를 피한다. Assign/Append/Reserve 실패 시 기존 문자열과 유효 뷰의 상태를 유지한다. 성공 시 재할당 또는 내용 변경에 따른 뷰 무효화 규약을 헤더에 명시한다.
- 자기 문자열 전체/부분 뷰를 Assign/Append하는 경우 원본을 잃기 전에 필요한 읽기를 완료한다. OOM/overflow 회귀로 이를 검증한다.
- 동시에 읽으려면 내용과 저장소 수명을 고정한다. 같은 문자열의 읽기/변경을 동기화 없이 병행하지 않는다. 다른 스레드로 소유권을 넘길 때 Allocator도 해당 사용을 지원해야 한다.
- 일반 byte slice와 UTF-8 문자 경계 slice를 구분한다. 문자 경계를 보존해도 grapheme 경계를 보장하지는 않는다.

## Text 설계와 현재 구현 경계

`Text`를 `using Text = String`으로 시작하면 번역 키와 서식 이력을 잃기 쉽다. 아래 장기 계약을 유지하고 별도 모듈에서 기본 불변 이력·catalog snapshot·named substitution을 구현했다. 정확한 API·수명·기본 한도는 [Name·Text 구현 계약](NameTextImplementation.ko.md)을 따른다. plural/select·날짜·통화·cook/loader·UI는 후속이며 완료 상태로 선언하지 않는다.

1. **안정 ID:** namespace+key 또는 관리된 Guid를 source manifest에 저장한다. 원문 변경과 타입/에셋 rename으로 번역 정체성을 임의 변경하지 않는다. source revision을 함께 기록해 오래된 번역을 찾는다.
2. **문구 종류:** localized key+source fallback, 언어 독립 invariant literal, 이름 있는 인자의 format expression을 구분한다. 일반 String에서 Text로의 암묵 변환은 제공하지 않는다.
3. **인자 소유:** 숫자·시간·다른 Text 등 타입 있는 값 인자를 불변 소유한다. 임시 StringView나 플러그인 코드 포인터를 잡아 두지 않는다. 재귀 format의 깊이·순환·출력 크기·할당 한도를 검사한다.
4. **resolve:** 명시한 culture와 불변 catalog revision snapshot을 사용해 소유 String 또는 명시 lease가 있는 resolved view를 반환한다. 실패는 Result로 전파하고 누락 키는 원문 fallback과 진단을 제공한다. 프로세스 전역 locale 변경에 숨은 의존을 만들지 않는다.
5. **언어 변경:** 언어/catalog 교체는 새 snapshot을 게시한다. 기존 frame/비동기 reader가 해제될 때까지 이전 snapshot을 유지한다. UI는 revision 변경으로 layout을 갱신한다.
6. **지역 규칙:** plural/select, 숫자·통화·날짜·시간 formatting은 검증된 라이브러리 adapter로 제공한다. Core에 임의 언어 규칙을 직접 구현하지 않는다. UI shaping/IME는 별도 시스템이다.
7. **저장·쿠킹:** 에셋은 Text 정체성과 typed 인자를 저장한다. resolve된 언어별 표시 String을 영속 정체성으로 사용하지 않는다. 추출→번역→검증→catalog cook→DDC key에 locale·schema·source revision을 포함한다.
8. **헤드리스·서버:** 폰트/윈도 없이 키 추출·카탈로그 검증·format 테스트가 가능해야 한다. 게임 규칙·네트워크 동기화는 번역된 표시 문자열의 비교에 의존하지 않는다.

## 검증 기준

String/StringView는 한글·ASCII·4-byte code point·결합 문자·빈 값·내부 NUL, 잘못된 UTF-8 분류별 golden bytes, allocator N-fail, 자기 alias, overflow, move/clone, 반복 생성·종료와 마지막 live allocation 0을 검사한다. OS의 locale이나 폰트 설치 여부에 의존하지 않는다. UTF-8 golden은 구현의 분기를 그대로 복사한 oracle을 쓰지 않는다.

현재 Text는 누락/오래된 키, named argument 불일치, culture 교체와 reader 유지, catalog OOM·재진입·병행 읽기를 시험한다. plural/select 언어별 golden, 비동기 취소·reload, cook 재현성, RTL 및 UI integration은 후속 gate로 구분한다. String 테스트 통과를 Text/IME/shaping 완료로 합산하지 않는다.
