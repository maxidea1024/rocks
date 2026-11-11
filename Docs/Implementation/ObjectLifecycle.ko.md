# 객체 생명주기와 실행 문맥

Rocks 객체의 식별자와 생명주기 상태는 `ObjectRegistry`가 소유한다. `Object`는 소유하지 않는 레지스트리 연결을 이용하여 파생 클래스에서 `Destroy()`, `IsDestroyRequested()`, `InspectLifecycle()`을 사용할 수 있다. 기본 설정에서는 별도의 `ObjectAccessScope`를 만들지 않아도 객체가 속한 Registry 문맥을 사용한다.

```cpp
// root와 registry는 이 코드보다 오래 살아야 한다.
auto borrowed = registry.Resolve<MyObject>(worldAccess, root.Handle());
if (!borrowed) {
    return borrowed.Error();
}

auto object = borrowed.Value().Get();
if (!object) {
    return object.Error();
}

if (object.Value()->IsDestroyRequested()) {
    return rocks::ErrorCode::Destroyed;
}

auto requested = object.Value()->Destroy();
if (!requested) {
    return requested.Error();
}

// 같은 Borrow를 유지한 동안에는 상태 조회와 멱등 Destroy 재호출만 허용한다.
auto state = borrowed.Value().InspectLifecycle();
if (!state) {
    return state.Error();
}
```

위 예시는 `ErrorCode`를 반환할 수 있는 함수 내부에서 실행한다. `IsDestroyRequested()`는 실제 상태를 `bool`로 반환하므로 조건문에서 바로 사용할 수 있다. 조회 자체가 실패하면 오류명을 남기고 모든 빌드에서 즉시 중단한다. 오류를 직접 처리할 필요가 있으면 다음과 같이 `TryIsDestroyRequested`를 사용한다. 실패 시 출력 변수는 바뀌지 않는다.

```cpp
bool destroyRequested = false;
auto query = object.Value()->TryIsDestroyRequested(destroyRequested);
if (!query) {
    return query.Error();
}
if (destroyRequested) {
    return rocks::ErrorCode::Destroyed;
}
```

유효한 Borrow로 접근 중인 객체는 `object->GetType()`으로 실제 등록 타입을 조회한다. 조회 오류를 직접 처리하려면 `object->TryGetType()`을 사용한다. 전자는 오류 원인을 진단하고 중단하며, 후자는 `Result<TypeLease>`로 실패를 반환한다. 반환한 lease는 타입의 등록·모듈 수명을 유지하지만 객체의 수명은 연장하지 않는다. 이름과 스키마는 lease를 변수로 보유한 동안 사용한다. 파괴 요청 후에는 새 타입 lease를 얻지 못한다. 실행 문맥 없는 호출의 원래 World를 식별할 수 없다는 아래 제한도 그대로 적용된다.

기본 설정에서 scope가 없으면 객체 또는 Borrow가 속한 Registry의 문맥을 사용한다. 이 경로는 thread-local scope를 만들거나 task로 전파하지 않는다. 이때 호출자가 실제로 어느 World에서 실행 중인지는 알 수 없어, 다른 World에서 scope 없이 전달받은 포인터의 오용을 구분할 수 없다. 명시적인 `ObjectAccessScope`가 있으면 그 문맥을 우선 적용하며 다른 World의 객체는 `WrongContext`로 거부한다. 다른 스레드는 항상 `WrongThread`다. 이 검사는 호출자 실수를 막기 위한 문맥 검사이며 보안 인증이 아니다.

World·PIE dispatcher가 항상 실행 문맥을 설정하는 구성은 `RegistryOptions::requireLifecycleScope = true`를 지정할 수 있다. 이 경우 Registry에 연결된 객체를 소유 스레드에서 호출할 때 scope가 없으면 `InspectLifecycle`, `TryIsDestroyRequested`, `TryGetType`은 `WrongContext`를 반환하고, `IsDestroyRequested`와 `GetType`은 `WrongContext`를 진단한 뒤 중단한다. 이 옵션은 명시적 `ObjectAccessContext`를 받는 Registry API에 적용되지 않는다. 현재 World dispatch와 task/worker 문맥 자동 전파는 아직 없으므로 엄격 모드 사용자는 진입 경계에서 scope를 설정해야 한다. `ObjectAccessScope`는 비복사·비이동이며 생성한 스레드에서 역순으로 파괴한다. 중첩 scope가 끝나면 이전 문맥을 복원한다.

scope 객체의 주소를 범위 밖에 보관하거나 `longjmp` 등으로 소멸자를 건너뛰는 사용은 지원하지 않는다. 생성 스레드가 아닌 곳에서 파괴하거나 역순을 어기면 현재 thread-local 문맥이 손상될 수 있으므로 즉시 Fatal로 중단한다.

`InspectLifecycle`은 한 시점의 스냅샷으로 수명이나 이후 접근을 보장하지 않는다. `Alive`는 당시 논리 접근이 허용된 상태, `DestroyRequested`는 명시적인 파괴 요청으로 접근이 닫힌 상태다. `Unreachable`은 증분 GC cutoff 후 아직 `Retired`로 분류되지 않은 white 객체다. `Retired`는 논리적으로 회수 대상이며 명시 파괴 또는 GC 중 어느 경로인지는 표현하지 않는다. GC cutoff에 걸리면 내부 슬롯이 아직 `DestroyRequested`여도 `Unreachable`로 표시한다. `IsDestroyRequested`는 `Alive`/`DestroyRequested`에서 false/true를 반환하고, 원인을 복원할 수 없는 `Unreachable`/`Retired`에서는 중단한다. 오류 처리용 `TryIsDestroyRequested`는 같은 경우 `Destroyed`를 반환한다.

파괴 요청은 일반 접근을 즉시 차단한다. 이미 보유한 `Borrow`를 포함해 이후 `Get`/새 `Resolve`/`WeakRef::TryPin`은 실패한다. 이미 얻은 객체 포인터는 같은 Borrow가 유지되는 동안 수명 상태 조회와 멱등 `Destroy` 재호출에만 사용할 수 있다. 임의의 객체 필드·게임플레이 메서드는 접근하지 않는다. 원시 포인터를 다시 사용하지 않는 `Borrow::InspectLifecycle()`을 권장한다. Borrow를 반환한 뒤 원시 포인터를 쓰면 C++의 수명 규약 위반이며, 이미 회수된 객체에서는 메서드의 유효성 검사 자체가 정의되지 않는다. Registry는 슬롯 전체의 Borrow 개수만 검사할 수 있으므로 호출자가 실제 그 Borrow를 유지했는지 식별하지 못한다. 다른 Borrow를 이용한 유출 포인터까지 차단한다고 보장하지 않는다.

기존 root는 회수를 늦추고 기존 borrow는 GC safepoint를 막는다. backend lease는 저장소 회수만 늦추며 논리 접근이나 자식 수명을 보장하지 않는다. 모두 반환한 뒤 GC 또는 `DrainRetired`가 저장소를 회수한다. 회수된 handle은 과거 상태를 영구 보관하지 않고 `StaleHandle`이다. 같은 슬롯 재사용 시 generation이 달라 이전 핸들은 새 객체를 가리키지 않는다.

외부에서 핸들의 상태를 확인할 때는 `registry.InspectLifecycle(access, handle)`을 사용한다. `RequestDestroy`는 기존 `DestroyRequested` 또는 `Retired` 슬롯에 다시 요청하면 성공한다. 이미 파괴 요청된 객체가 cutoff에 걸려 `Unreachable`로 조회되어도 이 재요청은 성공한다. 반면 파괴를 요청하지 않았던 cutoff white 객체에 새 파괴 요청은 `Destroyed`다.

객체 생성자가 실행되는 동안 registry 연결은 아직 없어 오류 처리 API는 `StaleHandle`을 반환하고 `IsDestroyRequested`는 해당 원인을 진단하며 중단한다. 생성자는 반환한 뒤 Registry에 연결되지만 `ConstructionContext`의 미공개 슬롯도 일반 수명 조회 및 파괴 요청을 허용하지 않는다. 생성자 실행 중에는 앞서 설명한 `StaleHandle`이며, 그 외 소유 스레드에서 유효한 scope를 유지한 채 Registry의 생성 처리·trace·소멸 callback에 재진입하면 `Busy`, shutdown 시작 이후에는 `ShuttingDown`이다. 기본 설정에서는 scope가 없어도 같은 소유 스레드와 Registry 문맥으로 검사한다. 엄격 모드에서는 scope가 없으면 먼저 `WrongContext`가 반환된다. 병렬 trace worker에서는 재진입 상태보다 소유 스레드 검사가 먼저이므로 `WrongThread`다. 정상 종료 전에 모든 root·borrow·backend lease를 반환하고 Registry가 그보다 오래 살아야 한다.
