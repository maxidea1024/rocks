# Name: 비교, 표시와 None

`Name`은 `NamePool`의 한 실행 세션에서 발급한 이름 토큰이다. 일반 이름은 pool domain과 비교 ID, 번호로 비교하고 대소문자가 다른 표기는 표시 ID를 따로 보관한다. 현재 ASCII 문자만 대소문자를 무시하며 비ASCII 문자의 Unicode case folding과 정규화는 지원하지 않는다. 원문과 비교 ID는 영속 파일·네트워크 식별자가 아니다.

```cpp
rocks::NamePool names(allocator);
if (!names.Initialize()) {
    return;
}

auto created = names.TryIntern("Actor_7");
if (!created) {
    return;
}

rocks::Name name = created.Value();
if (!name.IsNone() && name.GetNumber() == 7) {
    auto reader = names.Acquire(name);
    if (reader) {
        rocks::StringView original = reader.Value().View(); // Actor_7
        rocks::StringView base = reader.Value().BaseView(); // Actor
        // original과 base는 reader의 수명을 넘겨 보관하지 않는다.
    }
}
```

위 예는 pool에 대한 이후 사용과 reader가 모두 끝난 뒤 `Shutdown()`을 호출해야 한다. 실패 처리와 종료 처리는 호출 환경에 맞게 제공한다. `Name`은 pool을 소유하지 않으며 `IsValid()`는 필드 형태만 검사한다. 실제 발급·소유와 만료 여부는 `Acquire()`로 확인한다. 서로 다른 pool에서 발급한 일반 이름은 같은 표기여도 서로 다르다. raw ID를 보존해 재시작 후 복구하지 않는다.

기본 `Name{}`, 빈 입력, 대소문자를 무시한 `None`은 같은 None이다. `Name{}.IsValid()`와 `IsNone()`은 모두 true이며 번호가 없다. 기본 None이나 `None` 원문은 별도 entry를 할당하지 않고 `Acquire(Name{})`로 얻은 lease의 표기는 `None`이다. `none` 등의 다른 대소문자 표기는 원문 보존을 위해 해당 pool의 display entry에 저장될 수 있으므로 예산과 할당 오류가 적용된다. 동일한 None은 pool이 달라도 동등하고 해시도 같다. 표시 entry의 `Acquire`는 해당 pool과 세션을 검증한다. stale 표시 토큰이 기본 None과 같다고 해서 `Acquire` 검증을 통과하는 것은 아니다.

UE 5.8의 기본 번호 접미사 규칙(`UObject/NameTypes.h`의 `UE::Core::Private::ParseNumberFromName` — 5.6의 `UnrealNames.cpp` `FNameHelper::ParseNumber`에서 옮겨졌고 규칙은 같다)에 따라 `Actor_0`의 번호는 0이고 `Actor_2147483646`까지 숫자로 인식한다. `Actor_00`, `Actor_2147483647` 등은 원문 전체가 이름이며 번호가 없다. `_0`처럼 유효 번호만 있고 base가 비면 기본 None이 된다. `None_7`은 번호가 7인 이름이므로 `IsNone()`이 false이며 `none_7`과 동등하지만 각 표기는 유지한다. `View()`는 번호를 포함한 원문이고 `BaseView()`는 인식된 번호 접미사를 제외한다. embedded NUL은 길이 기반 이름의 일부이며 C 문자열처럼 조기에 잘리지 않는다.

## intern 없는 조회와 원문 비교

새 이름을 만들면 안 되는 자리(사용자 입력, Map<Name> 키 조회, Sparks 런타임 문자열)는 `NamePool::TryFind`를 쓴다. 이미 게시된 이름만 찾고 entry·예산·할당을 만들지 않는다. 없으면 `NotFound`, 잘못된 UTF-8은 `InvalidArgument`, 닫힌 pool은 `ShuttingDown`이다. 결과는 같은 원문을 `TryIntern`한 결과와 동등하다. byte가 같은 표시가 있으면 그 표시를, 없으면 비교 이름과 번호가 같은 표시 가운데 먼저 게시된 표시를 가리킨다. 표시 entry가 번호까지 담으므로 `Actor`만 있으면 `Actor_7`은 찾지 못한다. 기본 None(빈 문자열·`None`·`_7`)은 entry 없이 언제나 찾는다. 번호 없는 `none` 등도 언제나 찾으며 None이다(byte가 같은 표시 entry가 있으면 그 표시, 없으면 기본 None 토큰). `GetNumber()`는 번호가 없으면 `Name::NoNumber`(-1)다.

```cpp
auto found = names.TryFind(userInput);
if (!found) {
    // NotFound: 그런 이름은 pool에 없다(새로 만들지 않았다)
}
```

토큰 없이 두 원문을 비교할 때는 `NameTextEquals(left, right)`를 쓴다. 같은 pool에 intern한 두 결과의 `==`와 같은 답이고, true면 `TryMakeNameKey`의 key도 같다(hash가 필요하면 key를 쓴다). 할당과 pool이 필요 없고 UTF-8은 검사하지 않는다. `SplitNameText(text)`는 위 번호 접미사 규칙으로 base와 번호(`NameTextParts::base`·`number` — `Name::number`와 같은 표현)를 나눈다. 문법만 보므로 `_7`은 base 빈 문자열·번호 7이며, Name으로서 None인지는 `NameTextEquals(text, "")`로 판정한다. 표시 byte가 같은지(리플렉션 차이 저장의 같음)는 이 비교가 아니라 `StringView`의 `==`로 본다.

기본 None의 문자열은 정적 저장소에 있지만 `Acquire`로 얻은 lease는 다른 이름과 동일하게 pool 수명을 지킨다. lease가 살아 있으면 `Shutdown()`은 admission을 닫고 `Busy`를 반환한다. 반환 후 재시도해야 한다. 닫힌 pool은 `TryIntern`·`TryFind`·`Acquire`·lease 복제를 거부한다(`ShuttingDown`). 문자열 view는 살아 있는 lease보다 오래 보관하지 않는다. pool 객체 파괴 전 모든 호출과 lease를 종료하며 pool의 allocator는 pool보다 오래 살아야 한다.

이 동작은 이전의 “기본 Name은 invalid, 빈 이름은 별도 entry” 의미를 변경한다. 이전 raw 토큰의 ABI·저장 호환성은 제공하지 않는다. 오브젝트·에셋 직렬화에 Name을 연결할 때는 pool ID가 아닌 안정적인 문자열·번호 인코딩과 버전 정책을 별도로 정의해야 한다. 리플렉션 `Name` 필드의 영속 규칙(표시 문자열 wire, UTF-8 NFC 3,072 byte 상한, 복원 2a의 일괄 intern, `NamePool::TryFind`·pool 없는 비교 `NameTextEquals`)은 [리플렉션 B9](../Design/ReflectionExtensions.ko.md) 4절이 정한다. Unicode case folding, 문화권 의존 대소문자 변환, 실제 파일 이름 동등성은 이 기능이 보장하지 않는다.
