## 14. 네트워크

replication · RPC:

~~~text
class Weapon : ScriptableComponent {
    @net var Ammo: int = 30;
    @net(OwnerOnly) var Reserve: int = 90;
    let ReloadKey = Action("Gameplay.Reload");

    on Enable() { RefreshAmmo(); }                            // 활성화 전 적용·late join 상태 반영
    on Ammo.Changed() { RefreshAmmo(); }                      // 모든 기기에서 실행

    @rpc(Server, Reliable) fn RequestReload() {               // full 프로필
        if (Reserve == 0) { return; }                         // 서버에서 다시 검증
        Reload(Caller);                                       // Caller: 호출한 connection
    }

    on ReloadKey.Triggered() { Server.RequestReload(); }        // 호출 위치에 원격 호출이 드러난다
}
~~~

- **복제 필드(core):** `@net` 필드는 **권한 구역** 안에서만 쓸 수 있다(SD45). 권한 구역은 `if (Owner.HasAuthority) { … }`(Actor 파생은 `HasAuthority`)의 참 분기, `@authorityOnly` 이벤트의 handler, 서버 RPC 본문, `on Disable`·`on Destroy` 안의 `HasAuthority` 분기다. 권한 구역 밖의 쓰기는 컴파일 오류이며 `if (Owner.HasAuthority)` 감싸기를 수정 예로 제시한다. 실행 시 권한이 없는데 쓰면(권한이 바뀐 경우) 쓰기를 무시하고 위치별 1회 보고한다(fault 아님, §8.4). `@authorityOnly event`는 권한 측에서만 `emit`할 수 있고 위반은 `ContractViolation` fault다. 허용 타입은 저장 가능 타입(§15)과 복제되는 객체 참조다. 활성화 전 적용(초기 복제·late join)은 `Changed`를 발행하지 않으므로 `on Enable`에서 반영한다.
- **RPC(full):** 자기 객체의 RPC만 `Server.`·`AllClients.`·`OwnerClient.` 접두로 호출하며 static manifest(RpcId)로 등록한다(전체 계획 §23.2). 서버 RPC 본문에는 문맥 이름 `Caller: Connection`(호출한 connection)이 주어진다. 객체 인자는 호출자의 interest set 안에 있어야 하며, 네트워크 소유 connection이 아닌 호출자의 RPC는 거부한다. 길이·유한성·enum 값을 검사하고 connection·RpcId별 빈도를 제한한다. 클라이언트가 보낸 `Soft<T>`는 자동 로드하지 않는다(전체 계획 §17.1·§17.3).
- 원격 client가 유발한 fault는 instance·session으로 확대하지 않는다. prediction·replay 대상은 `@deterministic`이다.
