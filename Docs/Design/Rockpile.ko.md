# Rockpile — 에셋 너겟 서버

기준일: 2026-11-13, 개정 2(1차 교차검증 — 독립 설계 1·반례 검토 3·사실 확인 3 — 과 2차 최종 검증 2를 반영, 19절). UE 5.6 Zen Storage Server에 대응하는 Rocks의 에셋 너겟 서버를 정한다. 에디터·쿠커·명령행이 함께 쓰는 사용자별 로컬 캐시 프로세스이고, 같은 프로그램을 팀 공유 서버로도 띄운다. 구현은 .NET 10·C#이다(사용자 결정 — 13절). 너겟 키·값의 의미는 [에셋 시스템](AssetSystem.ko.md) 9절, 계층 구조는 [구현 계획](EngineImplementationPlan.ko.md) 9.1·27.2절을 따른다. 이 문서가 바꾸는 다른 문서의 내용은 17절에 모은다(아직 반영하지 않았다).

원칙: 기반이 부족하면 우회하지 않고 보강한다. 동작하지 않는 기능을 완성처럼 표시하지 않는다. 상태 표기는 AssetSystem과 같다 — **구현**(동작·시험), **계약**(API·형식이 있고 호출하면 `Unsupported`·`UnsupportedCapability`), **후속**(API도 없음). 지금은 설계 단계이며 구현은 없다.

## 1. 이름·역할·불변식

**Rockpile(록파일)** — 돌무더기. 빌드 결과를 쌓아 두었다가 다시 꺼내 쓰는 곳이라는 뜻이고 Rocks에서 이어지는 이름이다(사용자 결정). 실행 파일 `rockpile`, C++ 클라이언트 모듈 `RockpileClient`, C# 프로젝트 `Rocks.Rockpile.*`.

Rockpile은 **파생 데이터 저장소**다. 원본 에셋·원본 bulk의 권위 저장소가 아니며(계획 §9.4, A21) 모든 항목은 지워져도 다시 만들 수 있다.

| 불변식 | 내용 |
|---|---|
| R-0 틀린 적중 0건 | 손상된 byte·다른 키의 값·오래된 값·서로 맞지 않는 값의 조합을 적중으로 돌려주지 않는다. 데이터 손실은 허용한다(미적중이 될 뿐이다) |
| R-1 캐시 실패는 빌드 실패가 아니다 | 전송·저장 오류는 미적중 + 진단이다. 설정 `required`일 때만 오류 |
| R-2 한 프로세스는 한 저장소만 본다 | 빌드 도중 다른 저장소로 옮겨 가며 값을 섞지 않는다(12.1절) |

R-0을 이루는 갈래와 각각을 보장하는 쪽:

| 갈래 | 보장하는 쪽 | 수단 |
|---|---|---|
| 키 완전성 — 결과에 영향을 주는 입력이 모두 키에 있다 | 클라이언트(처리기) | AssetSystem 9절 키 규칙. 서버는 검증할 수 없다 |
| 조합 일관성 — 함께 쓰는 값들이 같은 판에서 왔다 | 클라이언트 | 세션 digest 고정·소비 입력 대조(5.2·12.1절) |
| 내용 무결성 — 받은 byte가 게시된 byte와 같다 | 서버·클라이언트 | 저장 byte SHA-256을 받을 때·내보낼 때 검사(6.7절), 클라이언트의 원본 해시 검사 |
| 출처 신뢰 — 값이 정당한 생산자에게서 왔다 | 운영 정책 | 쓰기 권한 제한·출처 기록·purge·세대(11.4절) |

## 2. 단계

| 단계 | 내용 | 상태 | 완료 기준 |
|---|---|---|---|
| R0 기반 | C++: PlatformBase `Process`(분리 실행·정체 확인)·`LocalChannel`(named pipe·UDS 클라이언트, 서버 정체 확인), Core SHA-256 하드웨어 경로. 형식: 명세 파일·골든 파일(8.3절). C#: Record TLV 구현 | 설계 | 15절 R0 행 |
| R1a 로컬 저장·프로토콜 | 저장 엔진(6·7절), 로컬 전송·일괄 RPC(8절), 프로세스 모델(10절), 로컬 보안(11.2절), C++ `NuggetRockpileBackend`와 에셋 너겟 레코드 계약(12절) | 설계 | 15절 R1a 행. AssetSystem A3 시험을 NuggetFileSystemBackend·NuggetRockpileBackend 양쪽에서 통과 |
| R1b 쿠킹 통합 | 쿠커의 일괄·비동기 사용, 소비 입력 대조, `--verify-nuggets`·`nugget-gc` 연동, SCC Record | 설계(AssetSystem A4·A5 뒤) | 15절 R1b 행. 빈/찬 캐시 쿠킹 출력 byte 동일 |
| R2 공유 | 공유 서버(HTTPS·토큰·namespace 권한·출처·purge 로그), upstream(읽기 통과·내구 쓰기 큐·drain), Linux systemd, 지표 | 설계 | 15절 R2 행 |
| R3 project store | 쿠킹 결과 저장과 기기 스트리밍(Zen project store·Zen Streaming 대응) | 후속 — 런타임 네트워크(NetCore)·Android VFS와 함께. namespace 이름 `rocks.project.*`만 예약 | — |
| R4 코드 산출물 | BuildArtifactCache(계획 §27.2) namespace와 Rocks Build 클라이언트 | 후속 — Rocks Build action cache와 함께. namespace 이름 `rocks.artifacts`만 예약 | — |

로컬을 먼저 한다. 여러 프로세스 공유와 중복 제거만으로 이득이 있고, 네트워크 지연이 섞이기 전에 저장 엔진 결함을 분리해 시험할 수 있으며, NuggetFileSystemBackend를 정답 기준(oracle)으로 대조할 수 있다.

## 3. UE 5.6 Zen과의 대조

근거는 UE 5.6 브랜치와 거기 포함된 zen v5.6.6 소스, Epic 공식 문서다(1차, 19.3절).

| 항목 | Zen(UE 5.6) | Rockpile | 차이의 이유 |
|---|---|---|---|
| 위치 | 5.4부터 로컬 기본 저장소. 공유 Zen은 5.5에서 Windows 호스트 production ready[S1][S2][S6] | 같음(로컬 기본 + 공유) | — |
| 구현 | C++ zenserver(별도 저장소). 공유·클라우드의 Unreal Cloud DDC와 Horde 서버는 C# ASP.NET Core(net8.0)[S3][S7] | C#(.NET 10) 하나로 로컬·공유 | 저장소의 기존 C# 도구 체계·형식 구현 공유(13절) |
| 로컬 전송 | loopback HTTP/1.1, 기본 포트 8558, 인증 없음[S2][S4] | named pipe(Windows)·UDS(POSIX) + 길이 프레임, OS 권한 + 클라이언트의 서버 정체 확인 | 같은 기계의 다른 사용자가 포트를 가로채 틀린 값을 주입하는 경로를 막는다(11.2절) |
| 공유 보안 | 인증 없음, namespace 권한 강제 없음, LAN 전용[S2]. Cloud DDC는 OIDC·namespace ACL[S3] | 토큰 인증·namespace 권한·출처·purge(11.3·11.4절) | 공유 캐시 오염이 팀 전체의 틀린 적중이 된다 |
| 해시 | IoHash = BLAKE3-256 앞 20 byte[S5] | SHA-256 32 byte 전체 | Rocks 키·원본·쿠킹 해시가 모두 SHA-256. 해시를 자르지 않는다 |
| 값 식별 | CompressedBuffer의 원본(raw) 해시. 헤더에 raw hash, 서버가 코덱을 앎[S5] | 저장 byte의 해시(ContentHash). 원본 해시는 Record 정보이고 클라이언트가 검사 | 사용자 등록 압축 형식(Compression 14절)을 서버가 가질 수 없고, 코덱·컨테이너를 C#에 중복 구현하지 않는다(5.4절) |
| 직렬화 | Compact Binary·CbPackage[S5] | Rocks Record TLV + RCPK Package(8절) | 엔진의 기존 구조화 형식 |
| 같은 키 재기록 | Record는 "같은 키 = 같은 값"을 요구, 메타는 달라도 됨[S5] | 논리 digest 기준 첫 쓰기 우선 + Conflict 진단(6.9절) | 결정성 버그·키 누락을 드러낸다 |
| CachePolicy | QueryLocal·QueryRemote·StoreLocal·StoreRemote·SkipData·SkipMeta·PartialRecord·KeepAlive[S5] | 같은 비트 + 조합 의미표(5.6절) | 조합의 빈틈을 명시한다 |
| RPC | `POST /z$/$rpc`, Put/Get CacheRecords·Get/Put CacheValues·GetCacheChunks. UE 클라이언트 일괄 8개[S4] | 일괄 RPC(8.4절), byte 예산 기반 일괄 | — |
| CAS | tiny < 1 KiB(256 MiB 블록), small < 1 MiB(1 GiB 블록), large ≥ 1 MiB 개별 파일. 인덱스 snapshot + append log[S4] | 블록 < 1 MiB(256 MiB 블록), large ≥ 1 MiB. 메모리 인덱스 + WAL + snapshot(6절) | 같은 방향. 블록 계층은 하나로 둔다 |
| GC | 접근 기준 만료(캐시 14일), 참조 수집 → 미참조 CAS 정리 → 사용률 60% 미만 블록 압축, 디스크 여유 하한, 예비 파일 `reserve.gc`[S4] | 같은 구성 + lease·epoch로 동시 Put과의 경합을 막는다(7절) | — |
| scrub | v5.6.6 compact CAS scrub은 헤더 raw hash만 대조(내용 재해시는 TODO)[S4] | 모든 읽기에서 내용 검증, scrub은 전체 재해시(6.7·7.5절) | R-0 |
| 로컬 최적화 | 큰 첨부를 `DuplicateHandle`로 넘긴 파일 handle 또는 경로·offset으로 전달[S4] | 후속(측정 뒤, 18절) | 서버 밖 handle이 삭제·압축 규칙(6.8절)을 바꾼다 |
| 프로세스 | 에디터 자동 실행, `%LOCALAPPDATA%/UnrealEngine/Common/Zen/Install`에 설치·버전 비교 뒤 교체, 공유 메모리의 sponsor pid 목록, `CREATE_BREAKAWAY_FROM_JOB`(CI에서 종료되지 않게)[S4] | 자동 실행·설치 사본·sponsor·breakaway 같음. major마다 인스턴스를 따로 둔다(10절) | 두 엔진 버전이 한 기계에서 서로를 끄지 않게 한다 |
| Project store | 쿠킹 출력 저장 5.6 production ready, Zen Streaming 5.6 Beta[S6] | R3 후속 | — |

## 4. 구성

```text
Editor·Cooker·Commandlet (C++)                    Rocks Build (C#, R4)
  └ NuggetCache ─ Memory ─┬─ NuggetRockpileBackend ─┐       └ Rocks.Rockpile.Client ─┐
                          └─ NuggetFileSystemBackend │                                │
                             (배타 선택)    LocalChannel(pipe·UDS)                     │
                                                     ↓                                ↓
                                   rockpile (로컬 — 사용자·프로토콜 major·저장 형식 major마다 1개)
                                                  │ HTTPS
                                                  ↓
                                        rockpile (공유, 팀·사이트) ─(선택)→ rockpile (중앙)
```

- C++는 **같은 사용자의 로컬 Rockpile하고만** 통신한다. upstream·TLS·토큰은 로컬 Rockpile이 맡는다. 그래서 C++에 TCP·TLS·HTTP를 넣지 않는다(12.6절).
- backend는 프로세스 시작 때 한 번 고르고 세션 동안 바꾸지 않는다(12.1절).
- 셰이더 컴파일·쿠킹 worker 프로세스는 캐시를 직접 쓰지 않는다. 조회·게시는 그 작업을 나눠 준 coordinator(에디터·쿠커 본 프로세스)가 하고 worker는 순수 입력 manifest만 받는다(계획 §27.7).
- CI 정확성 시험은 NuggetFileSystemBackend나 job 전용 제어 디렉터리의 빈 Rockpile(6.1절)로 결정적으로 돌린다. 공유 캐시를 채우는 것은 보호 브랜치의 병합 뒤 작업뿐이다(11.3절).

## 5. 데이터 모델

### 5.1 식별

| 개념 | 형식 | 비고 |
|---|---|---|
| Namespace | `[a-z0-9][a-z0-9._-]{0,63}` | 보존·용량·권한·내구 등급의 단위. `rocks.nuggets`, 예약: `rocks.artifacts`(R4), `rocks.project.*`(R3) |
| Bucket | `[a-z][a-z0-9_]{0,63}`(소문자 고정) | 처리기 종류(`texture`, `shader`, 처리기가 섞인 SCC는 `scc`). 식별이 아니라 관측·정책·purge 단위(키에 처리기 Guid가 이미 있다) |
| Key | 32 byte | 너겟 키(SHA-256, AssetSystem 9절) |
| ValueId | u32 | 처리기가 정하는 값 번호(텍스처는 mip·slice 부분) |
| ContentHash | 32 byte | **저장 byte**의 SHA-256 |
| RawHash | 32 byte | 해제한 원본의 SHA-256 |
| RecordDigest | 32 byte | 논리 digest(5.3절) |
| 판 번호 | u64 | 같은 키의 Record가 교체될 때마다 서버가 올리는 번호(digest 밖) |

프로젝트마다 namespace를 나누지 않는다. 키에 결과를 바꾸는 입력이 모두 들어 있어 프로젝트 사이 공유가 안전하다(AssetSystem 9절). 권한이 다른 팀이 한 공유 서버를 쓸 때만 나눈다.

### 5.2 Record

Record는 키 하나의 결과 묶음이며 불변이다. 형식은 Record TLV(schema `RockpileRecord`)이고 필드 순서를 명세가 정한다(서버가 순서·중복·한도를 검사하고 어긋나면 `NonCanonical`). 크기 상한 1 MiB.

| 필드 | 내용 |
|---|---|
| echo | namespace·bucket·key. 인덱스·일괄 응답의 오배선을 읽을 때 잡는다 |
| values | 고정폭 표(Bytes 필드 하나, ValueId 오름차순·중복 금지). 행 160 byte = { ValueId u32, encoding u8, reserved u8×3(0), rawSize u64, storedSize u64, indexLength u64, RawHash, ContentHash, IndexHash, ChunkTableHash(각 32) } |
| inputs | 이 Record를 만들 때 소비한 다른 Record의 { bucket, key, RecordDigest } 정렬 목록(없으면 비어 있다) |
| meta | 처리기 정보(≤ 64 KiB). **결정적 정보만** 넣는다(시각·기계·사용자 금지) |

- `encoding`: `Identity`(원본 그대로), `Rcbk`(RCBK v2 블록 컨테이너, Compression 9절), `Codec`(저장 byte 앞에 머리 { magic `RCDC`, nameLength u8, name UTF-8, revision u32 }를 붙인 사용자 등록 형식 — Compression 14.1절의 이름·revision 식별. 그 코덱이 없는 클라이언트는 미적중으로 다룬다). `Codec`은 namespace 정책 `allowCodec`이 켜진 경우만 받는다(공유 namespace 기본 끔).
- `indexLength`·`IndexHash`: `Rcbk` 값의 헤더 + 블록 표 길이(48 + 8n)와 그 byte의 SHA-256. 부분 읽기에서 클라이언트가 색인을 범위로 받아 검사한다(8.6절). 그 밖의 encoding은 0.
- `ChunkTableHash`: storedSize ≥ 1 MiB인 값의 조각 해시 표(저장 byte 1 MiB 조각마다 SHA-256)의 SHA-256. writer가 업로드 때 계산하고 서버가 받으며 검사한다. 조각 해시 표는 Record가 참조하는 blob이다(GC 루트에 들어간다 — 7.1절). 값 하나의 상한은 16 GiB라 표는 항상 512 KiB 이하다.
- reserved는 0이어야 한다(의미가 다른 값이 같은 digest로 `Exists` 처리되지 않게 표현 밖 비트를 두지 않는다).
- 값 개수 상한은 Record 1 MiB 예산으로 정한다(meta 64 KiB일 때 약 6,000행). 이보다 많은 부분을 만드는 처리기는 부분을 묶는다.
- 출처(작성자·시각·도구 빌드 ID·origin)는 Record 밖에 둔다(11.4절). 같은 빌드는 같은 Record byte를 만들어야 "다른 digest = 실제 비결정성"이 성립한다.
- 기존 `.rnug`는 이 Record + 값 byte의 v2 형식으로 바꿔 NuggetFileSystemBackend와 Rockpile이 Record schema를 공유한다(17절).

### 5.3 논리 digest와 저장 표현

`RecordDigest = SHA-256(정규 인코딩: echo, ValueId 순 { ValueId, rawSize, RawHash }, inputs, SHA-256(meta))`.

- **저장 표현**(encoding·storedSize·indexLength·ContentHash·IndexHash·ChunkTableHash)은 digest에 넣지 않는다.
- 압축은 두 종류다. **결과 압축** — 처리기가 결과의 일부로 거는 압축(블록 압축 텍스처, 쿠킹 데이터 안 압축). 그 이름·revision·수준·사전은 너겟 키에 들어간다(AssetSystem 9절, Compression 14.1절). **저장 압축** — 에셋 너겟 계층(`NuggetCache`)이 값을 저장할 때 거는 RCBK 압축. 키에 넣지 않는다. 그래서 같은 키·같은 digest라도 기계·라이브러리 버전에 따라 저장 byte가 다를 수 있고, 그것은 결정성 위반이 아니다.
- 같은 digest의 다른 표현은 `Exists`이며 먼저 게시된 표현을 유지한다. 예외는 6.9절.

### 5.4 서버는 코덱·컨테이너를 모른다

서버는 값을 불투명 byte로 다룬다. ContentHash·조각 해시 표를 받는 동안 계산해 검사하고, 범위는 저장 byte offset으로만 다룬다. RCBK 해석·해제·RawHash 검사는 클라이언트가 한다.

- 버린 대안 — Zen처럼 원본 해시로 식별: 서버가 받은 값을 검증하려면 해제해야 하는데, 사용자 등록 형식은 C++ 프로세스 안 플러그인이라 서버가 가질 수 없고(라이선스상 프로젝트가 따로 들이는 코덱 포함), 내장 코덱도 C#에 다시 구현해야 한다. 검증 없이 받으면 손상 byte가 정당한 해시로 게시된다.
- 버린 대안 — 서버가 RCBK 블록 표를 읽어 원본 범위를 저장 범위로 바꿔 주기: 블록 표 전체(4 KiB 블록·16 GiB면 32 MiB)를 검증 없이 믿어야 하고, 악성 표로 큰 할당·overflow를 일으킬 수 있으며, RCBK 해석을 C#에 중복 구현하게 된다.
- 대가: 같은 원본을 다르게 압축한 값은 중복 제거되지 않는다. 같은 라이브러리·설정이면 압축 출력이 결정적이므로(Compression 9.1절) 손실은 작다.
- writer 계약: 게시 전에 저장 byte를 왕복 해제해 RawHash를 확인한다(압축기 결함으로 틀린 byte에 맞는 CRC가 붙어 부분 읽기를 통과하는 것을 막는다). `Rcbk` 헤더의 rawSize와 행의 rawSize가 같아야 한다.

### 5.5 Blob

내용 주소 저장소(CAS)의 단위 = `ContentHash → byte`. 값 byte·조각 해시 표·Record 본문이 모두 blob이다. 여러 Record가 같은 blob을 참조하면 한 번 저장한다. 물리 저장은 namespace 사이에 공유하되 blob마다 소유 namespace 집합을 기록한다. 다른 namespace에만 있는 blob을 참조하려면 업로드(소유 증명)를 요구한다 — 해시만 알아내 권한 없는 namespace의 내용을 읽는 경로를 막는다(11.3절). 소유 집합은 mark 단계에서 다시 계산한다(7.1절).

### 5.6 CachePolicy

UE와 같은 비트다. 요청 전체와 항목별로 주고 항목별 값이 우선한다. C++ 타입 이름은 `NuggetPolicy`(계획 §27.2의 action 캐시 타입 `CachePolicy`와 구분).

| 비트 | 뜻 |
|---|---|
| `QueryLocal`·`QueryRemote` | 로컬 저장소·upstream을 조회 |
| `StoreLocal`·`StoreRemote` | 로컬·upstream에 기록. `StoreRemote`는 `StoreLocal`을 함의한다(전파 큐는 로컬 blob을 읽는다) |
| `SkipMeta`·`SkipData` | 응답에서 meta·값 byte를 뺀다 |
| `PartialRecord` | 일부 값만 있어도 `Partial`로 돌려준다(Get 전용. Put은 항상 완전한 Record만 게시 — 계획 §27.2) |
| `KeepAlive` | 접근 시각만 갱신 |

조합 규칙:

| 조합 | 의미 |
|---|---|
| upstream 적중 + `StoreLocal` | 모든 값 blob을 받고 해시를 검사한 뒤에만 로컬에 게시한다. `SkipData`·`PartialRecord` 결과는 로컬에 게시하지 않는다 |
| upstream 적중을 응답하기 직전 | shard 잠금 아래에서 로컬 Record를 다시 본다. 있으면 **로컬 값**을 돌려준다(한 기계 안에서 결과를 수렴). 없으면 키 자리를 upstream digest로 예약한 뒤 응답한다(6.4절) |
| `SkipData` 적중 | "지금 있었다"는 뜻일 뿐 나중에 값을 받을 수 있다는 보장이 아니다. 호출자는 값을 못 받으면 재생성한다 |
| 같은 키 동시 조회 합치기 | Query·Store 비트는 합집합, Skip 비트는 교집합(값이 필요한 요청이 값을 못 받는 일이 없게) |
| `StoreRemote`인데 토큰에 쓰기 권한 없음 | `Info`가 알려 준 권한에 따라 로컬 서버가 전파를 끈다. 401·403은 설정 오류로 진단하고 읽기를 막지 않는다 |
| upstream 사이 역채움 | 하지 않는다(upstream #2의 값을 #1에 쓰지 않는다) |

## 6. 저장 엔진

### 6.1 디스크 배치

제어 디렉터리(사용자 전용)와 저장소 루트(옮길 수 있음)를 나눈다.

| OS | 제어 디렉터리 기본값 | 저장소 루트 기본값 |
|---|---|---|
| Windows | `%LOCALAPPDATA%\Rocks\Rockpile` | `%LOCALAPPDATA%\Rocks\Rockpile\Stores` |
| macOS | `~/Library/Application Support/Rocks/Rockpile` | `~/Library/Caches/Rocks/Rockpile`(Time Machine 제외 대상) |
| Linux(공유 서버) | 서비스 설정 경로 | 서비스 설정 경로 |

```text
<제어>/
  config.json                         사용자·기계 설정(upstream·토큰 파일 경로·저장소 루트)
  instances/p<P>-s<S>/                인스턴스별(10.2절)
    lock                              인스턴스 잠금(지우지 않는다)
    state.json                        상태·pid·시작 시각·endpoint·버전(사용자 전용 권한)
    last-used                         실행 중 서버가 하루 한 번 갱신
  install/<실행 파일 SHA-256>/rockpile[.exe]
  logs/
<저장소 루트>/p<P>-s<S>/              저장소(인스턴스와 1:1)
  lock                                저장소 단일 기록자 잠금(지우지 않는다)
  format.json                         저장 형식 major·저장소 Guid
  wal/<seq:016x>.log                  WAL 세그먼트
  snapshot/<seq>.snap                 인덱스 snapshot
  blocks/<id:08x>.blk                 블록 파일(id 단조 증가, 재사용 금지)
  large/<h0h1>/<h2h3>/<hash>.<gen>    큰 blob(세대 번호로 경로 재사용 금지)
  tmp/  quarantine/  reserve          업로드 중, 격리, 예비 공간
```

- 저장소 경로에 프로토콜 major(P)와 저장 형식 major(S)를 모두 넣어 인스턴스와 저장소를 1:1로 둔다. 그래도 설정으로 두 인스턴스가 같은 경로를 가리킬 수 있으므로 저장소 안의 `lock`을 따로 잡는다(Windows 공유 모드, POSIX `flock` + 잡은 뒤 경로의 dev/ino 재확인 — AssetSystem 8절 `FileLock`과 같은 방식). 저장소 잠금을 못 잡은 인스턴스는 시작하지 않는다.
- 제어 디렉터리와 저장소 디렉터리는 **둘 다** 만들 때 사용자 전용 권한(Windows 보호 DACL, POSIX 0700·0600 + 배타 생성)을 걸고, 열 때 소유자·권한을 검사해 어긋나면 거부한다. 다른 사용자가 쓸 수 있는 저장소에는 헤더 해시가 일관된 가짜 Record를 써 넣을 수 있어서다.
- 네트워크 파일 시스템의 저장소는 거부한다(호스트 사이 잠금 보장이 없다). 잠금 파일에 호스트 id를 기록한다.
- CI의 job 격리: 환경 변수 `ROCKS_ROCKPILE_HOME`으로 제어 디렉터리를 job마다 바꾼다(같은 권한 검사). 저장소 루트 기본값은 제어 디렉터리 아래가 된다.
- `config.json`의 저장소 루트를 바꾸면 다음 서버 시작부터 적용된다. 실행 중 세션은 저장소 Guid 고정으로 보호한다(12.1절).

### 6.2 인덱스

메모리 인덱스 + WAL + 주기 snapshot이다.

- 표: `records`(ns·bucket·key → Record blob 위치·RecordDigest 앞 8 byte·판 번호·접근 시각 버킷·상태), `blobs`(ContentHash → 위치·크기·소유 namespace·상태·mark epoch). 해시 첫 byte로 256 shard, shard마다 잠금.
- GC 부담을 피하려고 항목은 참조 없는 struct를 큰 배열(open addressing)에 둔다. 작은 blob은 해시 앞 16 byte만 두고 나머지는 블록 항목 헤더로 확인한다(앞 16 byte 충돌은 전체 해시 보조 표). 큰 blob은 경로를 만들어야 하므로 전체 해시와 세대를 둔다. 메모리 추정: Record ≈ 80 byte, 작은 blob ≈ 40 byte. 공유 서버 Record 1천만 + blob 3천만 ≈ 2 GB, 로컬 Record 100만 ≈ 0.2 GB. 실측(RT16)으로 확정한다.
- **키는 32 byte 전체**를 둔다. 키를 자르면 틀린 적중이 생길 수 있다.
- WAL 항목 = { seq u64, type, length, payload, CRC32C }. snapshot은 임시 파일 → flush → 교체 → 디렉터리 동기화 뒤 그 seq 이하 WAL 세그먼트를 지운다. 다음 블록 id·large 세대·WAL seq의 high-water mark를 WAL·snapshot에 함께 기록한다.
- 버린 대안: 내장 B-tree KV(redb 류) — 접근 시각 갱신의 쓰기 증폭, commit 오류 뒤 "결과 모름" 상태, 긴 읽기 트랜잭션의 파일 증가를 따로 다뤄야 하고 .NET에는 성숙한 순수 관리 코드 선택지가 적다. SQLite — 네이티브 의존과 큰 blob 부적합. 인덱스는 `IIndexStore` 뒤에 두어 측정 결과에 따라 디스크 B-tree로 바꿀 수 있게 한다.

### 6.3 Blob 배치

| 크기 | 배치 |
|---|---|
| < 1 MiB | 블록 파일(최대 256 MiB)에 이어 쓰기. 항목 = { magic, ContentHash 32, size, 헤더 CRC32C } + payload, 16 byte 정렬. **group은 4 KiB 경계에서 시작**한다(앞 group의 마지막 page를 다시 쓰지 않는다) |
| ≥ 1 MiB(상한 16 GiB) | blob마다 파일 `<hash>.<gen>`, 원본 byte 그대로. 조각 해시 표는 별도 작은 blob(5.2절) |
| Record 본문 | 작은 blob과 같이 블록에 저장 |

임계값은 Zen과 같은 1 MiB에서 시작하고 측정으로 조정한다.

### 6.4 쓰기 경로

**blob 경로**

1. 인증·크기 검사를 본문을 읽기 전에 한다. 공간을 미리 예약한다(7.2절).
2. 받는 동안 SHA-256(과 1 MiB 이상이면 조각 해시)을 계산하며 블록 버퍼나 `tmp/`에 쓴다. 선언한 ContentHash와 다르면 버리고 `HashMismatch`.
3. 중복 제거는 **해시 검증이 끝난 blob**에만 한다. 아직 받는 중인 같은 해시가 있으면 받은 byte를 버리지 않고 보관한 채 기다리고, 앞 업로드가 실패하면 보관한 byte로 직접 쓴다. 격리된 blob과 같은 해시가 오면 받은 byte로 다시 쓴다(자가 치유).

**Record 경로 — 판정**

4. shard 잠금 아래에서 판정한다. 기존 Record가 있으면 6.9절 순서로 `Stored`(교체 포함)·`Exists`·`Conflict`를 정한다. 참조 blob(값·조각 해시 표)이 모두 있고(내구화 전이라도 검증이 끝났으면 인정) 격리되지 않았는지 확인하고 현재 GC epoch로 mark한다. 없으면 `NeedBlobs`.
5. `Stored`면 키 자리를 **게시 대기**로 예약한다. 게시 대기 Record는 즉시 읽기에 보이고 GC 루트다.

**내구화 — group commit**(기본 50 ms 또는 64 MiB)

6. 블록 파일 flush → 큰 파일 flush·이름 붙이기(`tmp` → `large/…/<hash>.<gen>`) → 새로 만든 파일이 있는 디렉터리(`blocks/`·`wal/`·`large/…`)와 새 하위 디렉터리의 부모를 동기화 → WAL에 blob 위치와 **판정을 통과한 Record만** append·flush → 게시 대기를 확정으로 바꾼다.
7. 같은 group의 blob 데이터가 WAL보다 먼저 flush되고, Record는 그 blob을 받은 뒤에 판정되므로 Record의 WAL 항목은 참조 blob과 같거나 뒤 group에 들어간다.

**응답 시점**(namespace의 내구 등급)

| 등급 | 응답 | 용도 |
|---|---|---|
| `cache` | 4·5단계 판정 뒤(flush 전). 결과 상태는 정확하고, 전원 차단·프로세스 크래시로 응답한 Put이 사라질 수 있다(미적중이 될 뿐) | 로컬 기본 |
| `strong` | 그 Record와 참조 blob이 들어간 group의 6단계 뒤 | 공유 서버, R3 project store |

큰 blob의 flush는 수신 작업마다 따로 하고, group에는 이름 붙이기·디렉터리 동기화·WAL만 넣는다(16 GiB 파일의 flush가 작은 Put을 막지 않게).

### 6.5 IO 오류

- flush(fsync) 실패를 재시도로 성공 판정하지 않는다(Linux는 실패한 page를 clean으로 처리한다). WAL·블록·디렉터리 flush 실패는 **저장소 재시작**으로 다룬다: 쓰기를 멈추고, 메모리 인덱스와 게시 대기를 버리고, 6.6절 복구 경로로 다시 연다(PostgreSQL의 fsync 실패 처리와 같은 방침). 복구 경로는 시험으로 가장 많이 검증되는 경로다.
- 쓰다 실패한 블록은 봉인한다. **한 번 쓴 블록 offset·블록 id·large 세대·WAL 세그먼트는 다시 쓰지 않는다.**
- 재시작 횟수 상한: 10분 안에 3번을 넘으면 **저하 모드**로 고정한다 — 검증된 읽기만 하고 Put은 `Unavailable`, 상태는 `degraded`로 알린다. 수동 `rockpile restart` 또는 다음 서버 실행까지 유지한다.

| 오류 | 처리 |
|---|---|
| flush 미지원(파일 시스템이 동기화를 지원하지 않음) | 그 저장소로는 시작하지 않는다(진단). 내구성 의미를 정할 수 없다 |
| EIO 등 장치 오류 | 저장소 재시작, 상한을 넘으면 저하 모드 |
| 권한 거부 | 저하 모드(설정 오류 진단) |
| 공간 부족 | 7.2절 압박 처리, 실패하면 저하 모드 |

### 6.6 복구

1. 인스턴스·저장소 잠금을 잡고 endpoint를 연 뒤 `recovering` 상태로 요청을 받는다(그 동안 Get은 `Recovering` → 클라이언트는 미적중으로 다루고 backend는 유지 — 10.3절).
2. snapshot을 읽고 WAL을 **세그먼트 단위로** 재생한다. 세그먼트 안에서는 CRC가 처음 틀린 곳에서 자르고, 다음 세그먼트는 계속 재생한다. 다른 세그먼트의 항목이 사라져 매달린 Record는 읽기 검증·자가 치유로 처리한다(6.7절).
3. 새 WAL 세그먼트와 새 블록으로만 쓴다. 크래시 전에 열려 있던 블록·세그먼트는 봉인한다. 다음 블록 id·large 세대·WAL seq = max(기록된 high-water, 디스크 파일 번호) + 1.
4. 인덱스가 가리키는 블록 파일이 없거나 기록된 길이보다 짧으면 그 블록의 항목을 `damaged`로 표시한다. 참조 Record는 다음 읽기에서 미적중·자가 치유된다.
5. 인덱스에 없는 블록·large·WAL 파일 중 **복구 시점 high-water 이하 번호만** 백그라운드에서 지운다(새로 만든 파일을 지우지 않는다). `tmp/`도 지운다. 시작을 막지 않는다.
6. snapshot·WAL이 열리지 않으면 저장소를 `p<P>-s<S>.corrupt`로 옮기고(하나만 보관, 공간 압박이면 옮기지 않고 지운다) 새 저장소 Guid로 빈 저장소를 만든다(캐시는 비지만 틀리지 않는다). 블록을 훑어 인덱스를 재구성하는 repair는 두지 않는다 — Record를 잃으므로 얻는 것이 reset과 같고, 경계 오인·커밋 안 된 꼬리를 섞는 위험만 남는다.

### 6.7 읽기 검증

서버는 **내보내는 모든 byte를 검증**한다. 쓰기 쪽 순서가 OS·장치·구현 결함으로 깨져도 결과가 틀린 적중이 아니라 미적중이 되게 하는 마지막 방어선이다.

| 경로 | 검증 |
|---|---|
| 작은 blob·Record 본문 | 블록 항목 헤더의 해시·크기를 요청과 대조 + payload 전체 SHA-256 |
| 큰 blob 전체·범위 | 조각 해시 표(ChunkTableHash로 검증)로 보내는 조각마다 SHA-256 |
| Record | 본문 해시 검증 + echo(ns·bucket·key)를 요청과 대조 |
| upstream 통과 | 로컬이 upstream에서 받은 byte를 같은 규칙으로 검증한 뒤 보낸다. 범위 읽기는 조각 해시 표를 먼저 받아 ChunkTableHash로 검증하고 범위를 그 표로 검증한다 |
| 블록 압축 복사·scrub | 복사·검사하면서 다시 해시한다 |

검증 실패: 그 blob을 `quarantined`로 표시하고(행은 남긴다) 참조 Record는 `Miss`. 큰 blob의 조각 해시 표가 틀리면 값 blob도 함께 격리한다(다시 올리면 둘 다 치유된다). 다음 Put이 같은 해시·digest를 가져오면 받은 byte로 교체한다.

클라이언트도 검사한다: 값 전체는 RawHash, 부분 읽기는 받은 프레임의 (ContentHash, offset, length)를 행·요청과 대조하고 RCBK 색인은 IndexHash, 블록은 RCBK 블록 CRC. 실패하면 `ReportCorrupt`(7.5절).

비용: SHA-256(OS 암호 라이브러리)은 코어당 GB/s 수준이라 10 GbE 송신에 코어 하나 정도다. 실측(RT16)으로 확인한다. 정확성 기능이라 끄는 설정을 두지 않는다.

### 6.8 동시성

- 쓰기 결정(중복 제거·Record 판정·GC 삭제·압축 위치 갱신)은 shard 잠금 아래에서 현재 값을 다시 확인하고 조건부로 바꾼다("아직 (블록 5, X)를 가리킬 때만").
- 블록 압축은 Put과 같은 파이프라인(복사 → flush → WAL에 위치 이동)으로만 게시한다. 손상 항목은 옮기지 않는다. 옛 블록은 WAL에 `retired`로 기록하고 프로세스 안 읽기 참조가 0이 되면 지운다(재시작 때 남은 retired 블록 삭제). 읽기가 지워진 블록을 만나면 인덱스를 다시 보고 한 번 재시도한 뒤 미적중.
- 큰 파일은 세대 번호가 붙은 경로라 삭제가 늦어져도(Windows의 백신·색인기 handle, delete-pending) 같은 해시의 새 쓰기와 부딪히지 않는다.
- 저장소에 쓰는 프로세스는 하나다(저장소 잠금). 클라이언트는 저장소 파일을 직접 열지 않는다.

### 6.9 같은 키의 다른 값

Put 판정 순서(6.4절 4단계):

1. 기존 Record가 불완전(참조 blob 없음·격리)하거나 읽을 수 없거나 `교체 허용` 표시(7.5절)가 있으면 → 교체(`Stored`, 판 번호 증가).
2. 같은 digest, 같은 표현 → `Exists`.
3. 같은 digest, 다른 표현 → `Exists`. 단 기존이 `Codec`이고 새 것이 내장 encoding이면 교체한다(코덱이 없는 클라이언트도 읽을 수 있게).
4. 다른 digest → 기존 유지, `Conflict{existingDigest}`. 오류가 아니라 진단이다. namespace·bucket별 `conflict` 지표와 크기 제한 진단 표(양쪽 digest·출처)를 남긴다. 결정성 CI gate(Conflict 0건)의 대상이다.

- 교체된 옛 판은 10분 동안 (key, digest)로 계속 읽을 수 있게 남긴다(옛 판과 그 blob에 lease). 진행 중인 세션이 같은 판을 끝까지 읽게 한다(12.1절).
- 로컬에서 만든 Record를 upstream으로 전파했는데 upstream이 다른 digest로 `Conflict`를 답하면 로컬 Record를 upstream 값으로 바꾼다(최선 노력의 수렴). 서로 맞지 않는 값의 섞임을 막는 장치는 수렴이 아니라 소비 입력 대조다(5.2·12.1절) — 읽기 전용 토큰의 개발자 기기는 전파를 하지 않으므로 수렴이 일어나지 않는다.
- 운영자의 정정 수단은 purge·세대 올리기다(11.4절). 덮어쓰기 API는 두지 않는다.

### 6.10 플랫폼 내구성

PlatformBase `FileSystem::SyncDirectory`·`Replace`(`Platform/File.h`, AssetSystem A0-P)와 같은 의미로 정하고, .NET 기본 flush가 아니라 오류를 모두 돌려받는 자체 호출로 구현한다(13.3절).

| OS | 파일 flush | 디렉터리 동기화 | 비고 |
|---|---|---|---|
| Windows | `FlushFileBuffers` | 항목 추가 권한으로 연 디렉터리 `FlushFileBuffers` | 같은 볼륨 rename의 영속 보장 범위는 문서로 확정되지 않았다. 불변식은 6.7절 읽기 검증으로 지킨다 |
| macOS | `F_FULLFSYNC`(`fsync`는 장치 캐시를 비우지 않는다) | 디렉터리 fd `F_FULLFSYNC` | `cache` 등급의 비용을 측정해 group 간격을 정한다 |
| Linux | `fdatasync`(데이터)·`fsync`(메타데이터) | 디렉터리 fd `fsync` | 6.5절 fsync 실패 규칙 |

내구성 약속이 깨지는 경우(장치 캐시 거짓말, 미영속 rename)의 결과는 "Record는 있는데 blob이 없음·틀림"이고, 이는 읽기 검증 → 미적중 → 자가 치유로 처리하는 정상 경로다.

## 7. GC·용량·무결성

### 7.1 도달성 — epoch mark-sweep

refcount를 쓰지 않는다. 크래시·부분 실패 속에서 카운트가 맞음을 증명하기 어렵고, mark-sweep은 매번 처음부터 다시 계산해 스스로 교정된다.

- 루트: 확정·게시 대기 Record(값 blob·조각 해시 표·Record 본문), 교체된 옛 판의 lease, Put 세션 lease, `Pending` ticket, upstream 전파 큐, (R3) project store 보존 commit.
- 순서: epoch를 올린다 → 루트를 훑어 blob을 현재 epoch로 mark하고 소유 namespace 집합을 다시 계산한다 → shard 잠금 아래에서 "현재 epoch 미mark ∧ lease 없음 ∧ 유예 경과"만 지운다.
- **Put 세션 lease**: `HasBlobs`·`PutRecords`가 "있음"이라 답한 blob과 받은 blob에 건다. 그 세션의 Record가 게시되거나 세션 TTL(10분)이 지나면 풀린다. 긴 업로드 중에는 클라이언트가 세션을 갱신한다(프레임·청크를 받을 때마다 서버가 갱신하고, 업로드가 없는 대기 구간은 `Touch(session)`). 압박 모드에서도 lease는 지킨다. 읽기 전용 토큰의 `HasBlobs`는 lease를 걸지 않는다(무한 pin 방지).
- 안전 논증: Record 게시(6.4절 4단계)는 참조 blob의 shard 잠금 아래에서 존재를 확인하고 현재 epoch로 mark한다. sweep은 같은 잠금 아래에서 mark·lease를 확인한다. 게시가 먼저면 mark가 보호하고, sweep이 먼저면 게시가 `NeedBlobs`를 답한다. 여러 shard에 걸친 게시 도중 epoch가 오르면 이미 mark한 blob은 Put 세션 lease와 게시 대기 Record(루트)가 보호한다.
- 유예(기본 1시간)와 lease는 이 프로세스의 **단조 시계**로 잰다. 단조 시계 값은 영속하지 않는다. 재시작하면 lease는 사라지고 모든 미참조 blob의 유예는 재시작 시각부터 다시 센다(보수적).
- 검증: 모델 검사와 negative control(규칙 하나씩 끈 빌드는 반드시 실패, 15절). 유한 시험은 증명이 아니므로 읽기 검증(6.7절)이 마지막 방어선이다.

### 7.2 축출·용량

| 정책(namespace별) | 로컬 기본 | 공유 기본 | 근거 |
|---|---|---|---|
| `maxUnusedDays`(마지막 접근 기준) | 14 | 10 | Zen 캐시 14일, 공유 템플릿 10일[S4][S2] |
| `maxBytes`(물리 사용량: 유예 blob·블록 빈 byte·인덱스 포함) | 0(제한 없음) | 설정 | — |
| 디스크 여유 하한(이하면 Put 거부) | 2 GiB | 설정 | UE 자동 실행 값[S4] |
| GC 주기(전체 / 축출만) | 1시간 / 15분 | 8시간 / 1시간 | Zen 기본·공유 템플릿[S4][S2] |

- 축출은 접근 시각 버킷(1시간 단위) 히스토그램으로 기준 시각을 정해 오래된 Record부터 지운 뒤 mark-sweep한다(근사 LRU). 종료 조건은 "실제로 회수 가능한 byte"다 — 다른 namespace가 함께 소유한 blob은 지워지지 않으므로 그 namespace의 사용량에서 빼지 않는다. 공유 서버는 수위(90%에서 시작, 80%까지)를 둔다.
- 디스크 압박: 예비 파일 `reserve`(기본 256 MiB, Zen `reserve.gc`와 같은 역할)를 지워 GC·snapshot 공간으로 쓰고, 압박 중에는 lease 없는 미참조 blob을 유예 없이 회수한다. 업로드는 선언 크기(HTTP `Content-Length`, 로컬 프레임 길이)로 공간을 미리 예약한다. 하한 아래면 Put을 `InsufficientStorage`로 거부한다(기다리게 하지 않는다).
- 토큰별 쓰기 할당량(공유) — 쓰레기 값으로 LRU를 밀어내는 것을 막는다.

### 7.3 블록 압축

살아 있는 비율이 60% 미만(Zen `compactblockthreshold`와 같은 값)이고 만든 지 1시간이 지난 블록이 대상이다. 6.8절 파이프라인으로 옮기고 IO 예산을 두어 꼬리 지연을 제한한다.

### 7.4 접근 시각

메모리에 1시간 단위로 두고 snapshot(15분 주기)에 실어 영속한다. 접근마다 WAL을 쓰지 않는다. 크래시로 잃는 것은 축출 순서뿐이다. 로컬 적중은 공유 서버 접근 시각을 갱신하지 않으므로, 로컬은 upstream에서 온 Record의 적중을 모아 하루 한 번 일괄 `Touch`를 보낸다(많이 쓰는 항목이 공유 LRU에서 먼저 지워지지 않게).

### 7.5 Scrub·손상 보고

- scrub: IO 예산 아래 모든 blob·Record를 다시 해시한다. 진행 위치를 영속하고 유휴 시 우선한다.
- `ReportCorrupt{ bucket, key, 판 번호, RecordDigest, 관측한 ContentHash, 종류(StoredHash·RawHash·Index·Block) }`: (key, 판 번호)가 현재 판과 같을 때만 처리한다(늦게 도착한 보고가 치유된 판을 다시 건드리지 않는다 — 멱등).
  - 서버가 그 blob을 즉시 다시 해시해 **틀리면** 격리한다(6.7절).
  - 해시가 **맞으면**(저장 byte는 정상인데 클라이언트가 RawHash 불일치를 봄 — 작성자 결함 또는 그 클라이언트 기계의 결함): 로컬 서버는 그 판에 `교체 허용`을 표시한다(계속 내보내되 다음 Put이 교체할 수 있다, 6.9절). 공유 서버는 `의심` 횟수만 센다 — 서로 다른 작성자 3명 이상 또는 쓰기 권한자의 보고일 때만 `교체 허용`으로 바꾼다. 토큰별 보고 속도를 제한한다.
  - upstream에는 보고만 전파한다.
- 격리된 blob은 `quarantine/`(크기 제한)으로 옮겨 진단 자료로 남긴다.

## 8. 프로토콜

### 8.1 전송 — 같은 RPC, 두 봉투

| 경로 | 봉투 | 이유 |
|---|---|---|
| 로컬(C++·C# 클라이언트 ↔ 로컬 서버) | named pipe(Windows)·UDS(POSIX) 위의 프레임: { magic `RPF1`, flags u32, requestId u64, length u64 } + Package. 연결마다 요청 하나씩, 클라이언트가 연결 풀(기본 4–8개)을 둔다 | C++에 HTTP 구현을 넣지 않는다. 브라우저는 pipe·UDS에 닿을 수 없어 Host·CORS 방어가 필요 없다 |
| 공유(로컬 서버·도구 ↔ 공유 서버) | HTTPS(HTTP/1.1·HTTP/2) | 프록시·부하 분산·관측 도구 생태계 |

공유 HTTP 경로:

| 경로 | 용도 |
|---|---|
| `POST /v1/cache/{ns}/rpc` | 일괄 RPC(8.4절), 본문 = Package |
| `PUT /v1/cache/{ns}/blobs/{hash}` | 큰 blob 스트리밍 업로드(8.5절) |
| `GET /v1/health` | 생존·준비 상태(서비스 감시용) |
| `GET /metrics` | Prometheus 텍스트(인증 또는 별도 포트) |

같은 Package와 같은 RPC 처리기를 쓴다. `requestId`는 응답 대조와 로그 상관(14절)에 쓴다. 취소는 연결을 닫는 것이다.

### 8.2 Package(RCPK v1)

모든 정수는 little-endian이다.

| offset | 크기 | 필드 |
|---|---|---|
| 0 | 4 | magic `RCPK` |
| 4 | 2 | version = 1 |
| 6 | 2 | flags = 0 |
| 8 | 8 | controlLength |
| 16 | controlLength | 제어 메시지(Record TLV) |
| … | 4 | frameCount |
| … | 프레임 반복 | { kind u8(`Blob`·`Range`), reserved u8×3, ContentHash 32, offset u64, length u64, payload[length] } |

- 프레임 길이가 앞에 오므로 받는 쪽은 해시를 계산하며 `tmp`에 흘려 쓰고 전체를 메모리에 올리지 않는다.
- (kind, ContentHash, offset, length)가 같은 프레임 중복, 제어 메시지가 참조하지 않는 프레임은 거부한다.
- **버전 규칙**: 와이어(프레임·RCPK·메시지 schema·한도)가 바뀌면 프로토콜 major를, 저장소 byte 배치가 바뀌면 저장 형식 major를 올린다. 같은 major = 같은 와이어·같은 저장 형식이다. 초기 단계이므로 하위호환을 두지 않고 다른 major는 다른 인스턴스로 공존한다(10.6절). `Info`가 명세 파일 해시를 돌려주며, 같은 major인데 다르면 빌드 결함이므로 클라이언트는 그 세션을 "캐시 없음"으로 내린다.

### 8.3 메시지 형식과 단일 정본

- 제어 메시지는 Rocks Record TLV(Serialization `Record.h`)와 `RecordList`로 쓴다. 엔진의 기존 구조화 형식이며 한도·예산 검사를 갖췄다.
- 한도(명세가 정하고 양쪽이 같은 값으로 검사): 제어 메시지 8 MiB(`ArchiveLimits.maxTotalBytes` 16 MiB 안, `maxValueBytes`·`RecordListLimits.maxPayloadBytes` = 8 MiB), 목록 항목 1 MiB + 64 KiB(Record 1 MiB와 echo·상태 등 감싸는 필드), Record 1 MiB, meta 64 KiB, 값 하나 16 GiB.
- 메시지 schema Guid·필드 Guid·한도는 명세 파일 하나 `Engine/Source/Developer/RockpileClient/Protocol/RockpileProtocol.json`에서 C++ 상수(생성 헤더를 커밋하고 시험이 명세와 일치를 확인)와 C# 상수(빌드 때 생성)를 만든다.
- 골든 파일: Record TLV 일반 인코딩은 Serialization 모듈 `Tests/` 소유(Core). Rockpile 메시지·RCPK 골든은 `RockpileClient/Tests/` 소유(Engine — Core는 Engine을 참조하지 않는다). C#은 두 곳의 파일을 읽어 byte 단위로 대조한다(Guid는 high u64·low u64 각각 LE — Serialization 구현 문서의 wire 표).
- Record TLV는 필드 순서를 강제하지 않으므로(Record.h) 명세가 순서를 정하고 받는 쪽이 검사한다. RecordDigest는 5.3절 정규 인코딩으로 직접 계산한다.
- C# Record TLV 구현은 `Rocks.Rockpile.Protocol`에 두고 Rocks Build(R4)가 같은 구현을 쓴다. 언어 경계마다 구현 하나씩(C++ Serialization, C# Rocks.Rockpile.Protocol)이며 정본은 명세 파일과 골든 파일이다.

### 8.4 연산

| 연산 | 내용 |
|---|---|
| `GetRecords` | 항목 = { bucket, key, policy, ValueId 목록(비면 전체), 기대 RecordDigest(선택) }. 결과에 echo·판 번호·digest·출처·값 프레임. 기대 digest가 있으면 그 판(현재 또는 lease 중인 옛 판)만 돌려준다 |
| `PutRecords` | 항목 = { Record, 인라인 blob 프레임 }. 결과 `Stored`·`Exists`·`Conflict`·`NeedBlobs`·`NonCanonical` |
| `PutBlobs` | `Blob` 프레임만 싣는다(로컬 2단계 업로드) |
| `GetChunks` | 항목 = { bucket, key, **기대 RecordDigest·판 번호**, ValueId, **기대 ContentHash**, 저장 byte 범위 목록 } — 8.6절 |
| `HasBlobs` | ContentHash 목록 중 없는 것. "있음"에 세션 lease(7.1절) |
| `Touch` | 접근 시각 일괄 갱신, Put 세션 lease 갱신 |
| `ReportCorrupt` | 7.5절 |
| `Info` | 서버 id·버전·실행 파일 해시·명세 파일 해시·저장소 Guid·upstream 구성 해시·토큰 권한·한도·상태(`ready`·`recovering`·`degraded`) |
| `Stats`·`Conflicts`·`Sponsor`·`Drain`·`Shutdown` | 관리(10·14절). 공유는 admin 권한 |
| `Purge`·`PurgeLog` | 공유 admin의 purge, 로컬이 가져가는 purge 로그(11.4절) |

항목 결과 상태: `Hit`·`Miss`·`Partial`·`Stored`·`Exists`·`Conflict`·`NeedBlobs`·`NonCanonical`·`HashMismatch`·`DigestChanged`·`RepresentationChanged`·`Corrupt`·`Denied`·`InsufficientStorage`·`Pending`·`Deferred`·`Recovering`·`Unavailable`.

- 결과 항목은 요청 항목을 되돌려 준다(echo: ns·bucket·key, `GetChunks`는 ValueId·범위까지). 클라이언트는 대조하고 하나라도 어긋나면 그 일괄 전체를 미적중으로 다룬다. 받은 값 byte의 검사(6.7절 클라이언트 쪽)는 의무다.
- 일괄 한도는 **byte 예산**이다: 응답 전체 기본 256 MiB. 넘는 항목은 413 대신 `Deferred`로 답해 클라이언트가 이어서 요청한다. **일괄의 첫 항목은 예산과 무관하게 보낸다**(예산보다 큰 값이 영원히 미뤄지지 않게 — 프레임은 디스크에서 흘려 보내므로 메모리 예산과 무관하다). 한도는 `Info`로 공개한다.
- upstream을 기다려야 하는 항목은 로컬 결과를 먼저 보내고 `Pending{ticket}`으로 답한다. 서버는 결과를 ticket에 60초 보관하고, 클라이언트는 ticket으로 다시 요청한다(최대 5초 long-poll). 보관이 끝났거나 보관할 수 없으면 `Miss`.
- 모든 연산은 멱등이다(`ReportCorrupt`는 판 번호로, `PutRecords`는 판정 규칙으로).

### 8.5 Put 협상

1. `PutRecords`에 Record와 64 KiB 이하 blob을 인라인한다. 전체가 4 MiB 미만이면 모두 인라인해 왕복 한 번으로 끝낸다(로컬은 대부분 이 경우).
2. 서버는 Record를 검사(schema·순서·echo·한도)하고, 인라인 blob을 검증·저장한 뒤 없는 해시를 `NeedBlobs`로 답한다. 이미 있는 blob에는 세션 lease를 건다.
3. 클라이언트는 없는 blob을 `PutBlobs`(로컬)나 `PUT /v1/cache/{ns}/blobs/{hash}`(공유, 스트리밍)로 보낸다. 1 MiB 이상이면 조각 해시 표 blob도 함께 보낸다.
4. 같은 `PutRecords`를 다시 보내면 6.4절 판정 뒤 `Stored`.

### 8.6 부분 읽기

서버는 저장 byte 범위만 다룬다. 원본 범위 → 저장 범위 계산은 클라이언트가 한다.

1. 클라이언트는 `GetRecords`로 받은 행에서 표현(ContentHash·encoding·indexLength·IndexHash)을 고정한다.
2. `Rcbk` 값: `GetChunks`로 범위 [0, indexLength)를 받아 SHA-256을 IndexHash와 대조하고, 기존 `CompressedBlocksReader` API(Compression 9.2절 — `TryOpen`의 컨테이너 길이 = 행의 storedSize)로 필요한 블록의 저장 범위를 구한다. 색인은 ContentHash별로 캐시한다.
3. `GetChunks`로 그 저장 범위들을 받아 해제한다. `Identity` 값은 원본 범위 = 저장 범위다. `Codec` 값은 `UnsupportedCapability`(전체 읽기만).
4. 서버는 기대 RecordDigest·판 번호·ContentHash가 현재(또는 lease 중인 옛 판)와 다르면 `DigestChanged`(다른 Record) 또는 `RepresentationChanged`(같은 digest의 다른 표현 — 새 행을 함께 돌려준다)로 답한다. 이것은 손상이 아니다. 클라이언트는 그 값 읽기를 처음부터 다시 한다(12.1절).

무결성: 서버는 보내는 범위를 조각 해시 표로 검증하고(6.7절), 클라이언트는 색인을 IndexHash로, 블록을 RCBK CRC로 검사한다. 클라이언트 쪽 블록 검사는 우발 손상 수준이며, 압축기 결함은 writer 왕복 검증(5.4절)이 막는다. 이 보장 범위를 AssetSystem 9절에 적는다(17절).

### 8.7 HTTP 규칙(공유)

- 인증·크기 검사는 본문을 읽기 전에 한다. 오류 응답에는 `Connection: close`. 큰 업로드는 `Expect: 100-continue`.
- 요청 헤더 수신 timeout, 최소 본문 수신 속도(slowloris), 연결 수 상한, 429·503 + `Retry-After`.
- 응답에 `Cache-Control: no-store`.
- 클라이언트 재시도는 연결 수립 실패에만 한다(전송 중 실패한 큰 Put을 자동으로 반복하지 않는다).

## 9. Upstream

| 항목 | 규칙 |
|---|---|
| 설정 | 로컬 `config.json`의 `upstreams: [{ url, namespaces, read, write, timeoutMs }]`, 토큰은 파일 경로·환경 변수(allowlist)로. 순서대로 조회 |
| 읽기 | 로컬 미적중 + `QueryRemote` → upstream 조회. 같은 키 동시 조회는 5.6절 규칙으로 합치고 upstream 조회 수명을 개별 요청과 분리한다. 받은 Record는 echo·한도·순서를 다시 검사하고 digest를 직접 계산한다. 응답 직전 로컬 재확인·예약(5.6절) |
| 로컬 게시 | 모든 값 blob을 받아 해시를 검사한 뒤에만(5.6절). blob 기록은 비동기로 해도 되지만 키 자리 예약은 응답 전에 한다 |
| 출처 | upstream 응답의 출처(서버 id·작성자 id·도구 빌드 ID·시각)를 로컬 Record의 출처로 함께 저장한다 |
| 음성 캐시 | upstream 미적중을 60초 기억한다(결과는 미적중뿐이다) |
| 쓰기 | **이 인스턴스에서 만든 Record만** 전파한다(upstream에서 받은 것은 다시 올리지 않는다 — 순환·권한 세탁 방지). WAL에 기록하는 내구 큐, 상한을 넘으면 가장 오래된 것부터 버리고 지표 |
| drain | `rockpile drain --timeout=<초>`: 큐를 비우고 끝낸다. 버린 항목·재시작으로 잃은 항목이 있으면 건수를 보고하고 종료 코드 1. CI 작업의 마지막 단계에 넣는다(VM이 사라지기 전에) |
| purge 로그 | 로컬은 접속할 때와 1시간마다 upstream의 `PurgeLog`(커서 이후)를 받아 같은 기준을 upstream 출처 Record에 적용한다(11.4절) |
| 장애 | 요청별 timeout, 읽기·쓰기 circuit breaker를 따로 둔다. upstream 오류는 미적중 |
| 수렴 | 전파가 `Conflict`면 로컬 Record를 upstream 값으로 바꾼다(6.9절, 최선 노력) |
| 계층 | 공유 서버도 upstream을 가질 수 있다(사이트 → 중앙). 전파할 때 원 작성자의 신원을 함께 보내고 상위는 그 신원으로 권한을 판정한다(사이트 쓰기 권한이 곧 중앙 쓰기 권한이 되지 않게) |

## 10. 프로세스 모델

### 10.1 실행 파일과 설치 사본

- 개발 트리: Rocks.Build가 C++ 대상(Commandlet·에디터)을 빌드할 때 `rockpile`을 RID별로 게시해 C++ 실행 파일과 같은 `<NativeOutput>/bin/`에 둔다(빌드 그래프 단일 소유 — 계획 §27.1). 설치 SDK도 같은 배치다.
- `RockpileClient`의 탐색 순서: 명령행 `--rockpile-exe=…` → 환경 변수 `ROCKS_ROCKPILE_EXE` → 자기 실행 파일 옆. 없으면 "캐시 없음" + 진단.
- 실행할 때는 `<제어>/install/<실행 파일 SHA-256>/`에 복사본을 두고 그것을 실행한다(엔진 빌드 출력의 실행 파일을 잠그지 않는다 — Zen과 같은 이유). 임시 이름으로 복사 → 해시 확인 → 덮어쓰지 않는 이름 바꾸기. 이미 있으면 검증만 한다. 디렉터리 이름이 해시라 같은 버전 문자열의 다른 빌드, x64·arm64 사본이 부딪히지 않는다.
- macOS 배포본은 서명·공증한다. 실행 정책(AppLocker·WDAC 등)이 사본 실행을 막는 환경을 위해 "제자리 실행" 설정을 둔다.

### 10.2 인스턴스 정체성

인스턴스 = (사용자, 제어 디렉터리, 프로토콜 major, 저장 형식 major). 저장소 경로도 같은 조합이다(6.1절).

- Windows endpoint: `\\.\pipe\rocks-rockpile-<서버 시작 때 고른 128 bit 난수>`. 이름은 사용자 전용 `state.json`으로만 알린다(다른 사용자가 이름을 미리 계산해 선점할 수 없다).
- POSIX endpoint: `<제어>/instances/p<P>-s<S>/rockpile.sock`(0700 디렉터리라 다른 사용자가 선점할 수 없다). macOS UDS 경로 길이 한도(104 byte)를 넘으면 사용자 전용 `$TMPDIR` 아래 디렉터리로 옮기고, 서버가 60초마다 소켓 파일의 dev/ino를 확인해 없어졌으면 다시 bind한다.

### 10.3 시작·종료 상태 기계

`state.json` = { state(`starting`·`recovering`·`ready`·`degraded`·`stopping`), pid, 프로세스 시작 시각, instanceId, endpoint, 버전, 실행 파일 해시, 저장소 Guid }. 원자적으로 교체하고 클라이언트는 공유 삭제 허용으로 읽는다.

| 단계 | 동작 |
|---|---|
| 시작 | 인스턴스 잠금 획득(실패하면 다른 인스턴스가 있으므로 바로 끝) → 저장소 잠금 획득(실패하면 진단 후 끝) → endpoint 생성(Windows는 first-instance) → `starting` 기록 → 복구(6.6절, `recovering`) → `ready` |
| 유휴 종료 | 상태 기계 하나로: 새 연결 수락 중지 → sponsor·연결을 다시 확인 → 남아 있으면 종료 취소, 아니면 `stopping`. 전파 큐는 내구 기록이므로 기다리지 않는다(다음 실행이 이어서 보낸다) |
| 종료 | 새 요청 `Unavailable` → 진행 중 요청 마무리 → group commit·snapshot → `state.json` 무효화 → 잠금 해제(무효화가 해제보다 먼저) |

### 10.4 접속과 자동 실행

1. 클라이언트는 `state.json`의 endpoint에 접속하고 **서버 정체를 확인**한다(11.2절). 실패하거나 서버가 다른 사용자면 접속하지 않는다.
2. 접속이 안 되면 분리 실행한다: `rockpile serve --instance=… --sponsor=<pid>:<시작 시각>`.
   - Windows: handle 상속 금지, `DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP`, `CREATE_BREAKAWAY_FROM_JOB`(Zen과 같이 — CI·IDE의 job 종료에 끌려가지 않게. breakaway가 거부되면 job 안에서 실행하고 진단을 남긴다).
   - POSIX: `setsid`, stdio를 `/dev/null`, close-on-exec. 잠금을 못 잡고 끝난 자식은 회수한다(zombie 방지).
   - 인자는 배열로 넘긴다(공백·Unicode 경로).
   - **권한 상승된 프로세스는 자동 실행하지 않는다**(있는 서버에만 접속, 없으면 "캐시 없음"). 서버도 상승된 상태로는 `--allow-elevated` 없이 시작하지 않는다(상승된 서버의 정체를 일반 권한 클라이언트가 확인하지 못할 수 있다).
3. 상한(기본 10초) 안에서 backoff하며 다시 접속한다. `recovering`이면 그 요청은 미적중으로 하고 backend는 유지한다.
4. 접속 실패·`Unavailable`은 backoff 뒤 다시 실행을 시도한다. 세션 전체를 "캐시 없음"으로 고정하는 것은 연속 실패 상한을 넘었을 때, 정체 확인이 실패했을 때, 저장소 Guid·upstream 구성 해시가 바뀌었을 때(12.1절)다.
5. R1a의 자동 실행은 C++ `RockpileClient`만 한다(13.5절).

### 10.5 수명

- sponsor = (pid, 프로세스 시작 시각). 서버는 Windows에서 sponsor 프로세스 handle을 열어 두고(handle이 열려 있는 동안 pid가 재사용되지 않는다), POSIX에서는 시작 시각을 함께 비교한다. pid만으로 판정하지 않는다.
- 모든 sponsor가 끝나고 유휴 시간(기본 10분)이 지나면 10.3절 유휴 종료.
- 서비스 모드(공유 서버)는 sponsor 없이 계속 실행한다. 정식 호스트는 Linux systemd(전용 계정). Windows 공유 서버는 서비스 모드(후속) 전까지 콘솔·예약 작업으로 실행한다.

### 10.6 버전 공존

- 프로토콜·저장 형식 major가 다른 엔진은 **다른 인스턴스와 다른 저장소**를 쓴다(6.1·10.2절). 서로를 끄지 않으므로 교체 경합·핑퐁이 없고, 대가는 같은 기계에 캐시가 둘이 되는 것뿐이다(캐시이므로 허용).
- 같은 major 안의 새 빌드(실행 파일 해시가 다름)는 와이어가 같으므로(8.2절 버전 규칙) 실행 중인 옛 빌드 서버를 그대로 쓴다. 새 빌드는 sponsor가 모두 끝난 뒤 다음 실행부터 뜬다. 저장 엔진 결함 수정처럼 즉시 바꿔야 하면 `rockpile shutdown` 뒤 다시 실행한다.
- 정리: 실행 중인 서버는 하루 한 번 `last-used`를 갱신한다. 새 인스턴스는 30일 동안 쓰이지 않은 다른 인스턴스·저장소를, 그 인스턴스 잠금과 저장소 잠금을 **비차단으로 잡았을 때만** 지운다. 잠금 파일은 지우지 않는다(지운 뒤 다시 만든 파일에 다른 프로세스가 잠금을 잡아 서버가 둘이 되는 것을 막는다).

## 11. 보안

### 11.1 위협 모델

| 대상 | 범위 |
|---|---|
| 같은 기계의 다른 사용자(원격 데스크톱·공유 빌드 기계) | 막는다 |
| 브라우저(DNS rebinding·CSRF) | 막는다(로컬 TCP를 열지 않는다) |
| 공유 서버의 권한 없는 사용자·쓰기 권한자의 실수·침해된 빌드 에이전트 | 막거나 피해를 한정하고 회수한다 |
| 같은 사용자의 다른 프로세스 | 범위 밖(그 사용자의 파일을 이미 바꿀 수 있다) |

### 11.2 로컬

- Windows pipe: 이름에 난수(10.2절), 소유자를 현재 사용자 SID로 명시하고 현재 사용자만 허용·`NETWORK` 거부하는 DACL, first-instance. 구현은 13.4절.
- 클라이언트의 서버 정체 확인(C++·C# 모두): Windows는 pipe 객체의 소유자 SID와 `GetNamedPipeServerProcessId`로 얻은 서버 프로세스 토큰의 사용자 SID가 모두 자기 SID와 같은지. POSIX는 소켓 디렉터리의 소유자·권한(0700) + 접속한 peer의 uid(macOS `getpeereid`, Linux `SO_PEERCRED`)가 자기 uid와 같은지.
- 서버도 peer 사용자를 확인한다. 로컬에는 토큰이 없다(OS 권한과 상호 정체 확인으로 충분하다).
- 제어·저장소 디렉터리 권한은 6.1절.

### 11.3 공유

- TLS 1.2 이상(서버 내장 또는 앞단 프록시에서 종료). macOS 로컬 Rockpile이 클라이언트이므로 1.3 전용으로 두지 않는다(13.2절).
- 인증: 256 bit 불투명 bearer 토큰. 서버는 토큰의 SHA-256만 저장하고 설정 재적재로 폐기한다. OIDC는 후속.
- 권한: namespace별 `read`·`write`·`admin`. **쓰기는 보호 브랜치에 병합된 뒤 도는 CI만**. PR CI는 신뢰할 수 없는 코드를 실행하므로 읽기 전용 토큰을 쓴다. 개발자 기기의 쓰기는 로컬에만 들어간다(필요하면 신뢰가 낮은 별도 namespace).
- blob 소유 namespace(5.5절): 다른 namespace에만 있는 blob 참조는 업로드를 요구하고, `HasBlobs`는 요청 namespace 기준으로 답한다.
- 입력 검증: 이름 규칙, Record·메시지 한도, 프레임 수·크기 상한, 동시 업로드 상한, 토큰별 속도·할당량·`ReportCorrupt` 속도. 지표 레이블은 서버가 정한 bucket 목록으로 제한한다(레이블 폭증 방지).

### 11.4 오염 대응

서버는 값이 키에 맞는 결과인지 증명할 수 없다. 그래서 출처를 기록하고 회수할 수 있게 한다.

| 수단 | 내용 |
|---|---|
| 출처 기록 | Record마다 서버가 증명한 작성자(토큰 id)·시각·클라이언트가 보낸 도구 빌드 ID·origin(로컬 생성·upstream 서버 id)을 Record 밖 표에 둔다. 응답에 함께 싣는다 |
| purge | 작성자·시간 창·도구 빌드 ID·bucket 단위 제거. 소비 입력(5.2절)을 따라 그 Record를 입력으로 만든 파생 Record까지 지운다(파생 폐포). 공유 서버는 purge를 `PurgeLog`에 남기고 로컬은 그것을 받아 upstream 출처 Record에 같은 기준을 적용한다(9절). 로컬에서 직접 쓰는 `rockpile purge`도 같다 |
| 세대 | 너겟 키 공통 항목에 **에셋 너겟 세대 Guid**(엔진 상수, 프로젝트가 재정의 가능)를 넣는다. 올리면 모든 키가 바뀐다 — 회수가 보장되는 유일한 수단(17절) |
| 표본 재생성 | 신뢰 builder가 공유 적중의 일부를 다시 만들어 대조한다(`--verify-nuggets`의 공유 확장). 불일치는 경보와 purge로 |
| Conflict 감시 | 6.9절 지표 |

처리기 버전 Guid를 올리지 않은 수정이 사용자별 로컬 캐시를 통해 다른 worktree로 퍼지는 위험은 남는다. 정정 수단은 처리기 버전 올리기·세대 올리기·로컬 purge이며, 공유 쓰기를 보호 브랜치 CI로 제한해 팀 전체로 퍼지지 않게 한다.

## 12. C++ 클라이언트와 에셋 너겟 계약

AssetSystem 9절의 `NuggetBackend { Get, Put, Remove, Touch }`(`.rnug` 통째·단건·동기)를 레코드 모델의 일괄·비동기 계약으로 바꾼다. A3은 아직 구현되지 않았으므로 처음부터 새 계약으로 만든다.

### 12.1 계층과 세션 고정

- `NuggetCache` = Memory(프로세스 안) → 선택된 backend 하나(`NuggetRockpileBackend` 또는 `NuggetFileSystemBackend`).
- 선택: `Config/AssetNuggets.json`의 `backend`. `rockpile`·`filesystem`·`off`는 그대로, `auto`는 프로세스 시작 때 Rockpile에 접속(필요하면 자동 실행)되면 `rockpile`, 아니면 `filesystem`(경고). 결정은 프로세스 시작 때 한 번이고, 자식 프로세스는 부모의 결정을 물려받는다. 도중 실패는 "캐시 없음"으로만 내려간다(R-2). 서로 다른 최상위 프로세스(에디터와 별도 쿠킹 명령)는 다른 결정을 할 수 있으며, 각각은 일관된다.
- 세션 고정: 첫 접속의 (저장소 Guid, upstream 구성 해시)를 고정한다. 바뀌면(서버 재시작으로 다른 저장소·upstream이 열림, 6.6절 빈 저장소 교체 포함) 그 세션은 "캐시 없음"으로 내린다.
- key → (RecordDigest, 판 번호, 값별 표현)을 세션 동안 고정한다.
  - 한 Record의 값은 한 판에서만 모은다. 값을 나눠 받을 때는 기대 digest·표현을 싣는다(8.4·8.6절).
  - `DigestChanged`를 받으면 그 Record에서 이미 받은 부분을 모두 버리고 Record 전체를 다시 얻거나 다시 만든다. 다시 만든 결과가 세션 고정을 대신한다. `RepresentationChanged`는 그 값 읽기만 처음부터 다시 한다.
  - 쿠커는 패키지를 직렬 commit하므로(계획 §27.7) 버린 부분이 출력에 섞이지 않는다.
- 소비 입력 대조: 적중한 Record의 `inputs` 각 항목을 세션 고정과 대조한다. 고정이 없으면 그 digest로 고정하고(이후 그 키는 기대 digest로 요청한다), 어긋나면 그 적중을 미적중으로 다룬다. 의존 결과를 소비해 Record를 만드는 처리기는 소비한 (bucket, key, digest)를 `inputs`에 기록한다.
- 프로젝트별 FileSystem 저장소에서 사용자별 Rockpile로 바꾸면 기존 적중을 잃는다(이전하지 않는다).

### 12.2 API 형태

Core Task 관례를 따른다(Compression 5절 `CompressionTask`와 같은 수명 규칙): 호출자가 소유하는 고정 주소 요청 객체, `const WorkerTaskHandle& Handle() const`로 의존 연결, 소멸자는 `RequestCancel` 뒤 완료를 기다린다. 입력은 호출 시 snapshot한다(계획 §27.2 "Submit은 입력을 소유 저장소로 snapshot"). 전송 완료는 외부 이벤트이므로 `WorkerTaskEvent`로 작업 그래프에 잇는다. 결과는 handle 완료 뒤 읽는다(콜백을 두지 않는다). worker 작업 안에서 결과를 blocking으로 기다리지 않는다.

```cpp
// 형태 스케치 — 이름·인자는 A3 구현 단계에서 확정한다.
struct NuggetGetItem {
    NuggetBucket bucket;                   // 고정 소문자 ASCII 상수(Name 아님 — 대소문자 무시·번호 접미사 규칙과 맞지 않는다)
    Sha256Digest key;
    NuggetPolicy policy = NuggetPolicy::Default;
    Span<const uint32> values;                 // 비면 전체. Start가 복사하므로 호출 동안만 유효하면 된다
};

class NuggetGetRequest {                   // 호출자 소유, 시작 뒤 이동 금지
public:
    ~NuggetGetRequest() noexcept;          // RequestCancel 뒤 완료를 기다린다
    Result<void> Start(NuggetCache&, Span<const NuggetGetItem>, WorkerScheduler&) noexcept;
    void RequestCancel() noexcept;             // 진행 중 전송은 연결을 닫아 끊는다
    const WorkerTaskHandle& Handle() const noexcept;
    Span<const NuggetGetResult> Results() const noexcept;   // Handle 완료 뒤
};
```

- 결과 상태: `Hit`·`Miss`·`Partial`·`Stored`·`Exists`·`Conflict`(비결정성·키 누락 진단으로 올린다)·`DigestChanged`·`Corrupt`·`Cancelled`·`Unknown`(commit 여부를 모르는 취소된 Put)·`Dropped`.
- Put은 값 버퍼의 소유권을 넘겨받는다. 대기 byte 상한(기본 1 GiB)을 넘는 Put은 즉시 `Dropped`(캐시이므로 버린다, 진단 지표).
- 엔진 종료 순서(계획 §3.2·§28.3)에 Put drain(시간 상한, 넘으면 진단 후 버림)을 넣는다. `Shutdown()`은 모든 요청의 완료를 보장하고, 그 뒤 시작한 요청은 즉시 `Cancelled`다. Commandlet이 쿠킹 직후 끝나도 Put이 사라지지 않게 한다.
- `ReportCorrupt`를 계약에 둔다. `Remove`는 관리 명령으로만 남긴다.
- 전송은 `LocalChannel`의 완료 기반 API를 쓴다(12.6절).

### 12.3 SCC

Build 의존 순환의 SCC 구성원은 함께 만들어지므로 **SCC 하나를 Record 하나**로 둔다.

- key = SCC key(AssetSystem 9절). 구성원 key(= SHA-256(SCC key, AssetId))로 찾는 의존 쪽은, 구성원 key를 계산하는 과정에서 SCC key를 이미 알므로 SCC Record를 조회해 그 구성원의 값을 고른다(NuggetCache가 감싼다).
- ValueId = (구성원 순번 << 16) | 부분 번호. meta에 구성원 AssetId 목록(16 byte씩 — meta 64 KiB로 최대 4,096 구성원). 넘는 SCC는 캐시하지 않고 진단한다.
- 처리기가 섞인 SCC의 bucket은 `scc`.
- 한 Record라 구성원 일부만 적중하는 상태가 없다. 나눠 받을 때는 12.1절 세션 고정 규칙을 따른다.

### 12.4 명령·설정

| 대상 | 규칙 |
|---|---|
| `Config/AssetNuggets.json`(버전 관리, 프로젝트) | `backend`(12.1절), `required`, `localPath`(FileSystem). **너겟 키·cookKey에 들어가지 않는다** |
| 사용자·기계 설정 | 제어 디렉터리 `config.json`과 환경 변수 allowlist(`ROCKS_ROCKPILE_HOME`·`ROCKS_ROCKPILE_EXE`·`ROCKS_ROCKPILE_TOKEN_FILE` 등): upstream URL·토큰 파일 경로·저장소 루트·자동 실행 끄기 |
| `--verify-nuggets` | 적중 결과와 재생성 결과를 대조. 불일치면 두 digest와 출처를 보고한다(어느 쪽이 틀렸는지 판정하지 않는다). 대상 계층은 `--verify-nuggets-source=local`·`=remote`(기본 둘 다, AssetSystem 13절 옵션 문법) |
| `nugget-gc` | NuggetFileSystemBackend 전용. Rockpile이면 `rockpile gc`로 위임하고 옵션 대응(`--max-bytes` → 물리 `maxBytes`, `--unused-days` → `maxUnusedDays`)을 따른다 |
| 사용자 명령 | 실행 파일 `rockpile`: serve·status·stats·conflicts·gc·scrub·purge·drain·restart·shutdown·bench. `status --json` 제공. 종료 코드는 Commandlet과 같은 0(성공)·1(실행 오류)·2(사용법). 통합 `rocks` 명령이 생기면 그 아래로 노출하고(계획 2.4절 명명 규칙) 그때 계획 §28.4 종료 코드로 옮긴다 |

### 12.5 모듈

- `RockpileClient`(Engine/Source/Developer): LocalChannel 연결 풀, 프레임·Package·메시지 인코딩(기존 `RecordBuilder`·`RecordList`), 자동 실행(10.4절), 서버 정체 확인, 명세 파일과 생성 헤더, Rockpile 골든 파일. AssetNugget(Developer)이 의존한다. 메시지 인코딩은 R3에서 런타임 클라이언트가 필요해지면 런타임 모듈로 옮긴다.
- 의존 방향: `AssetNugget → RockpileClient → { Serialization, Task, Core, PlatformBase }`.

### 12.6 C++ 기반 보강(R0)

| 기반 | 위치 | 근거 |
|---|---|---|
| `Process`: 분리 실행(10.4절 규칙)·(pid, 시작 시각) 정체·생존 확인·권한 상승 여부 | PlatformBase | PlatformLayer P1 "프로세스와 pipe", UE `FPlatformProcess`. 임포트 worker process(AssetSystem 17절)도 쓴다. Android는 `Unsupported` |
| `LocalChannel`: Windows named pipe·POSIX UDS 클라이언트, 완료 기반 API(`WorkerTaskEvent`로 이음), 서버 정체 조회(pipe 소유자 SID·서버 프로세스 SID / peer uid) | PlatformBase | UE `FPlatformNamedPipe`가 Core HAL에 있다. 구현은 Windows overlapped IO(AsyncFile과 같은 IOCP), POSIX는 v1에서 전용 IO 스레드(후속 kqueue). TCP·UDP 소켓은 넣지 않는다(NetCore 계획 — 계획 §3.1) |
| SHA-256 하드웨어 경로(SHA-NI·ARMv8 SHA2) + 스칼라 차등 시험 | Core Encoding | 클라이언트는 Put마다 저장 byte·원본·조각을 해시한다. 지금 Core 구현은 스칼라뿐이다 |
| TLS·HTTP | 넣지 않는다 | 원격은 로컬 Rockpile이 대신한다 |

## 13. .NET 구현

근거는 19.3절 [S7]–[S12](dotnet/runtime·dotnet/aspnetcore `release/10.0` 소스와 Microsoft 문서).

### 13.1 배치

저장소의 C# 도구 관례(`Programs/Rocks.*`, 규칙 어셈블리가 이 폴더들을 제외한다)를 따른다.

```text
Programs/
  Rocks.Rockpile.Protocol/   Record TLV·RCPK·메시지(명세 파일에서 생성한 상수). 외부 의존 없음. Rocks Build(R4)도 참조
  Rocks.Rockpile.Store/      저장 엔진: 인덱스·WAL·snapshot·블록·large·GC·scrub. IStorageIo로 장애 주입, 스케줄 주입 지점
  Rocks.Rockpile.Server/     RPC 처리기, 로컬 endpoint(pipe·UDS 수락 루프), 공유 HTTP(Kestrel), upstream, sponsor, 지표
  Rocks.Rockpile.Client/     C# 클라이언트(upstream·명령행·시험·Rocks Build)
  Rocks.Rockpile/            실행 파일 rockpile
  Rocks.Rockpile.Tests/      단위·모델·장애 주입·동시성·통합 시험
```

`Programs/Directory.Build.props`(net10.0, nullable, `TreatWarningsAsErrors`, 결정적 빌드)를 그대로 받는다.

### 13.2 런타임 구성

| 항목 | 결정 | 근거·버린 대안 |
|---|---|---|
| 로컬 endpoint | 자체 수락 루프: Windows `NamedPipeServerStream`(비동기, 첫 인스턴스는 `FirstPipeInstance`), POSIX `Socket(AddressFamily.Unix)` | Kestrel 로컬 전송은 쓰지 않는다 — named pipe 전송은 원격 클라이언트 거부를 표현할 수 없고, UDS 전송은 소켓 파일 권한을 설정하지 않고 남은 소켓 파일을 지우지 않으며[S8], 로컬 봉투는 HTTP가 아니라 프레임이다(8.1절) |
| 공유 endpoint | Kestrel HTTPS, HTTP/2는 ALPN. blob 업로드 경로만 `MaxRequestBodySize`(기본 30 MB)를 올리고 헤더 timeout·최소 수신 속도 기본값 유지 | [S8] |
| TLS | 최소 TLS 1.2. 공유 서버 정식 호스트는 Linux(OpenSSL, TLS 1.3) | macOS의 .NET 서버 `SslStream`은 SecureTransport라 TLS 1.3 서버를 지원하지 않는다. 클라이언트도 macOS 기본 경로는 TLS 1.2다(Network.framework는 클라이언트 opt-in)[S9] |
| upstream 클라이언트 | `HttpClient`(SocketsHttpHandler) 연결 풀 | — |
| GC | 로컬: Workstation GC(concurrent) + `GCConserveMemory`(값은 측정). 공유: Server GC + DATAS(기본) | ASP.NET Core 기본인 Server GC는 작업 집합이 크다(문서 예시 500 MB → Workstation 70 MB)[S10] |
| 인덱스 메모리 | 참조 없는 struct 배열(LOH). GC가 내용을 훑지 않는다 | 6.2절 |
| SHA-256 | `IncrementalHash` → OS 암호 라이브러리(Windows CNG, Linux OpenSSL, macOS CommonCrypto) | 하드웨어 가속 여부는 OS 구현 몫이라 RT16에서 실측[S11] |
| CRC32C(WAL·항목 헤더) | `BitOperations.Crc32C`(기본 라이브러리, 하드웨어 명령) | `System.IO.Hashing`은 NuGet이고 10.0.x에는 Crc32C가 없다[S11]. RCBK의 zlib CRC-32는 서버가 계산하지 않는다(클라이언트가 검사) |

### 13.3 저장 IO

- 읽기·쓰기: `RandomAccess.Read`·`Write`(Unix `pread`·`pwrite`)와 `SafeFileHandle`.
- flush: 자체 `DurableIo`가 P/Invoke로 부른다 — Windows `FlushFileBuffers`, macOS `fcntl(F_FULLFSYNC)`, Linux `fdatasync`(데이터)·`fsync`(메타데이터). 오류는 하나도 무시하지 않는다(6.5절). .NET `Flush(true)`·`RandomAccess.FlushToDisk`를 쓰지 않는 이유: Linux에서 EROFS·EINVAL·ENOTSUP를 무시하고, macOS에서는 10.0.14까지 `F_FULLFSYNC` 실패를 조용히 무시한다(10.0.15에서 수정)[S12].
- 디렉터리 동기화: .NET 공개 API가 없고 Unix `SafeFileHandle`은 디렉터리를 열지 못한다. P/Invoke로 PlatformBase `FileSystem::SyncDirectory`와 같은 의미를 구현한다(POSIX 디렉터리 fd `fsync`·macOS `F_FULLFSYNC`, Windows `FILE_FLAG_BACKUP_SEMANTICS`·항목 추가 권한 handle의 `FlushFileBuffers`).
- 이름 바꾸기: 대상이 있을 수 없는 경로(세대 번호)로만 옮기므로 `File.Move(overwrite: true)`(Unix `rename`)를 쓴다. `overwrite: false`는 Unix에서 사전 `lstat` 뒤 `rename`이라 원자적이지 않다[S12]. 설치 사본(10.1절)은 디렉터리 이름 바꾸기로 놓고 대상이 있으면 검증만 한다.
- 잠금: `FileStream(FileShare.None)` — Windows 공유 모드(강제), Unix `flock(LOCK_EX|LOCK_NB)`. 잡은 뒤 경로의 dev/ino를 다시 확인한다. `FileStream.Lock`은 macOS에서 지원되지 않아 쓰지 않는다[S12].
- 삭제: Windows의 열린 파일 삭제 의미(POSIX 의미 여부)가 빌드마다 문서화되어 있지 않으므로 기대지 않는다. 세대 경로와 읽기 참조 수(6.8절)로 처리한다.

### 13.4 로컬 endpoint 보안 구현

- Windows pipe: `PipeSecurity` = 소유자 현재 사용자 SID(권한 상승된 토큰의 기본 소유자 Administrators를 쓰지 않게 명시), 현재 사용자 허용, `NETWORK`(S-1-5-2) 거부. 관리 API로 `PIPE_REJECT_REMOTE_CLIENTS`를 줄 수 없어 DACL로 대신한다(필요하면 P/Invoke로 만든 handle을 `NamedPipeServerStream`으로 감싼다). 다른 사용자는 DACL 때문에 인스턴스를 더할 수 없다. 같은 사용자의 인스턴스 추가는 위협 모델 밖이다(11.1절).
- POSIX: 인스턴스 잠금을 잡은 뒤 남은 소켓 파일을 지우고 0700 디렉터리 안에서 bind한다. 접속한 peer의 uid는 raw socket option(Linux `SO_PEERCRED`, macOS `LOCAL_PEERCRED`)으로 읽는다.
- C# 클라이언트(`Rocks.Rockpile.Client`)는 pipe를 `PipeOptions.CurrentUserOnly`로 열어 pipe 소유자 SID를 확인하고(.NET이 접속 뒤 검사한다), 서버 프로세스 SID는 P/Invoke(`GetNamedPipeServerProcessId`)로 확인한다. UDS는 디렉터리 소유자·권한과 peer uid를 확인한다. C++ 클라이언트는 같은 검사를 `LocalChannel`에서 한다(12.6절).

### 13.5 프로세스

- sponsor 감시: `Process.GetProcessById` + `StartTime` 대조, `WaitForExitAsync`. Windows는 `Process` 객체가 handle을 쥐므로 pid가 재사용되지 않는다[S12].
- 자동 실행은 R1a에서 C++ `RockpileClient`만 한다(10.4절). .NET 10 `Process.Start`는 Windows에서 handle 상속을 끌 수 없다(`bInheritHandles` 고정). 분리 실행 API(`StartDetached`·`InheritedHandles`)는 .NET 11에 생겼다[S12]. C# 쪽 자동 실행(R4)은 P/Invoke 생성기(`CreateProcessW`·`posix_spawn` + setsid)나 그 API를 쓸 수 있는 LTS로 옮긴 뒤 한다.
- 서비스 모드: systemd `Type=exec` 단위 + `/v1/health`. `Microsoft.Extensions.Hosting.Systemd`·`WindowsServices`는 공유 프레임워크가 아닌 NuGet이라 지금은 쓰지 않는다.

### 13.6 배포

- 기본: RID별(win-x64·osx-arm64·linux-x64) framework-dependent single-file `rockpile[.exe]`. .NET SDK가 있는 개발 기계에는 ASP.NET Core 런타임이 함께 설치된다. 공유 서버는 ASP.NET Core 런타임 컨테이너 이미지.
- 런타임 요구: 10.0.15 이상(macOS `F_FULLFSYNC` 오류 처리 수정을 포함한 보안 패치). framework-dependent 앱은 설치된 아무 10.0.x에서나 돌 수 있으므로 시작할 때 `Environment.Version`을 검사해 미달이면 시작을 거부하고 진단을 남긴다. 개발 기계가 이 런타임을 갖도록 저장소 `global.json`(SDK 10.0.302 = 런타임 10.0.10)을 올린다(17절).
- ReadyToRun: 자동 실행 대기 시간을 측정한 뒤 정한다(교차 컴파일 가능).
- NativeAOT: 기본으로 쓰지 않는다 — Windows에서 MSVC(Visual Studio C++ 워크로드)가 필요하고(저장소 C++은 Clang 도구 체인), OS 사이 교차 컴파일이 안 되며, ASP.NET Core 지원이 부분적이다[S10]. 로컬 데몬 메모리가 문제가 되면 다시 판단한다(18절).

### 13.7 빌드·규칙·공급망

- 빌드: 프로젝트를 C# 도구 묶음(`Programs/Rocks.Tools.csproj`)에 더해 기존 `BootstrapTools` 단계에서 빌드한다. RID별 게시는 Rocks.Build가 맡는다(10.1절). 새 도구 체인·CI 설치 단계가 없다.
- 규칙: MS C# 규칙(Allman 중괄호, 한 줄 if도 중괄호, 멤버·블록 사이 빈 줄, 한 줄 한 문장), 공개 API XML 문서 주석(한국어 — 목적·인자·반환·예외·스레드·수명). 서식은 기존 C# 서식 검사 경로.
- 의존: 공유 프레임워크(`Microsoft.NETCore.App`·`Microsoft.AspNetCore.App`)만 쓴다. NuGet이 필요해지면 `packages.lock.json` + `RestoreLockedMode`, NuGetAudit(net10 기본 `all`), 라이선스 고지 묶음에 넣는다[S12].
- 시험 도구: 속성 기반·결정적 동시성·퍼징 패키지는 R1a 착수 때 유지 상태를 보고 정한다(후보: CsCheck Apache-2.0, Coyote MIT — NuGet 최신판이 2024-03에 멈춰 있다, SharpFuzz MIT). 결정적 동시성 시험이 패키지 없이도 되도록 저장 엔진에 스케줄 주입 지점을 둔다.
- CI: 기존 Windows·macOS 작업에 `Rocks.Rockpile.Tests` 실행 단계를 더하고(시험 프로젝트마다 단계를 두는 기존 방식), Linux 작업(공유 서버 시험·다른 사용자 권한 행렬 컨테이너)을 더한다.
- 선례: UE 5.6의 Unreal Cloud DDC(`Jupiter.csproj`)와 Horde 서버는 `Microsoft.NET.Sdk.Web`(ASP.NET Core)이다[S3][S7].

## 14. 관측

- 로그: 구조화 로그(JSON lines), `<제어>/logs/` 일 단위 회전·범주별 속도 제한. 토큰은 기록하지 않는다. 요청 상관 id(로컬 프레임 `requestId`, 공유 `X-Rocks-Request-Id`)로 Rocks Lens 로그와 잇는다.
- 지표(namespace·bucket별): 요청·적중(로컬·upstream)·미적중·byte·지연 분포, `conflict`·`corrupt`·`hash_mismatch`·`digest_changed`·`dropped_put`, GC 실행·회수·소요, 블록 압축, scrub 진행, 살아 있는 byte·빈 byte, 인덱스 메모리, WAL 크기, 저장소 재시작·저하 모드, upstream 큐·실패·circuit 상태, 연결 수, 인증 실패.
- 조회: `rockpile status`·`stats --bucket=…`·`conflicts`(로컬), `GET /metrics`(공유). 웹 대시보드는 후속(zen은 5.5.8 판부터 내장[S4]).
- 미적중 이유(없음·손상·upstream 불가·Conflict·DigestChanged·소비 입력 불일치)는 NuggetCache가 Rocks Lens에 보고한다. 어느 입력이 바뀌었는지는 클라이언트의 키 기록 비교 몫이다 — 선택 기능으로 디버그 모드에서 KeyWriter 원상 byte를 로컬 진단 표에 남긴다.

## 15. 시험

| ID | 단계 | 시험 | 판정 |
|---|---|---|---|
| RT01 | R0 | 메시지·Package·Record 골든(C++ ↔ C#), 한도 경계·잘린 입력·중복·순서 위반, 악성 입력 퍼징(Package·Record·프레임) | 두 언어 byte 일치, 예외·무한 할당 0 |
| RT02 | R1a | 모델 기반 상태 시험: 참조 모델과 실제 저장소에 무작위 put·get·replace·gc·compact·crash·restart | Get 결과는 {모델의 현재·lease 중인 판, 미적중}뿐, **다른 값 0** |
| RT03 | R1a | 크래시 일관성: 장애 주입 파일 계층이 매 IO 지점에서 "죽는다". flush 안 된 쓰기의 소실·부분 기록·파일 사이 순서 바뀜·0 또는 이전 내용 꼬리·디렉터리 동기화 없는 생성·이름 바꾸기 소실·flush 실패 뒤 clean 처리·세그먼트 사이 소실 모사 | 재시작 뒤 RT02 불변식, `strong` 응답 항목 보존, 번호 재사용 0 |
| RT04 | R1a | 실제 강제 종료(Windows TerminateProcess, POSIX SIGKILL)를 부하 중 반복(양 OS). page cache가 살아남으므로 fsync 누락은 RT03이 맡는다 | 재시작·검증 통과 |
| RT05 | R1a | IO 장애: 디스크 가득 참, 부분 read·write, EIO, flush 미지원, Windows 공유 위반(백신), rename 실패, 재시작 반복 | 저장소 재시작·저하 모드 전환·정리 |
| RT06 | R1a | 동시성: 게시·sweep·lease·압축·교체 경합의 결정적 동시성 시험, N 클라이언트 × M 스레드 같은 키·blob Put/Get + GC + 압축 | 틀린 데이터·매달린 Record 0 |
| RT07 | R1a | negative control: 해시 검증·읽기 검증·mark 규칙·lease·flush 순서·echo 대조를 각각 끈 시험 빌드 | 해당 시험이 **반드시 실패** |
| RT08 | R1a | 같은 키 다른 값·같은 digest 다른 표현·불완전 Record 교체·옛 판 lease 읽기·`Codec` 교체 | 6.9절 규칙 |
| RT09 | R1a | 손상 주입(값·조각 해시 표·Record 본문) → 미적중 1회 → `ReportCorrupt`·자가 치유 → 다음 적중. 늦게 도착한 보고 | 반복 미적중 없음, 치유된 판 불변 |
| RT10 | R1a | 프로세스: 동시 자동 실행 경합, 유휴 종료와 새 접속 경합, 오래된 `state.json`·pid 재사용, breakaway 실패, CI 단계 종료(출력 handle 상속 없음), 권한 상승 | 인스턴스 하나, 접속 성공, 부모 단계 정상 종료 |
| RT11 | R1a·R2 | 보안: 다른 사용자 접속·pipe 선점·서버 위장(양 OS, Linux 컨테이너), 제어·저장소 디렉터리 권한 변조, 공유 권한 행렬·namespace 우회·해시 위조 Put·크기 초과·읽기 전용 토큰의 `ReportCorrupt` 남용 | 모두 거부·한정 |
| RT12 | R2 | 네트워크 장애 프록시: 지연·끊김·부분 쓰기·slowloris·upstream 단절/복귀·늦은 응답·일괄 오배선·`Pending` ticket 만료 | 빠른 미적중, 큐 복구, 오배선 검출 |
| RT13 | R1a | 버전 공존: 다른 major 두 인스턴스·저장소 동시 실행, 30일 정리와 실행 중 인스턴스, 저장소 Guid 변경 시 세션 내림 | 서로 간섭 없음 |
| RT14 | R1a·R1b | 통합: AssetSystem A3 시험을 NuggetFileSystemBackend·NuggetRockpileBackend 양쪽에서. (R1b) 빈/찬 캐시 쿠킹 출력 byte 동일, `--verify-nuggets`, 소비 입력 불일치 시 미적중, SCC Record | 양쪽 통과, 동일 byte |
| RT15 | R1a | 플랫폼: Unicode·공백·긴 경로, macOS UDS 경로 길이, Windows·macOS 행렬 | 양 OS 통과 |
| RT16 | R1a·R2 | 성능: 작은 Get 일괄(1000), 큰 blob 스트리밍, 읽기 검증 비용, GC·압축 중 p99, 인덱스 메모리, 자동 실행 대기 | 기준선 기록 뒤 목표 확정, 회귀 추적 |
| RT17 | R2 | purge·purge 로그·파생 폐포·세대 올리기 | 오염 Record와 파생이 로컬·공유에서 사라짐 |

실패한 seed·최소 재현·로그를 보존한다. flaky를 재시도로 숨기지 않는다.

## 16. 위험

| 위험 | 대응 |
|---|---|
| 키 완전성·공유 캐시 오염(최대 위험) | 보호 브랜치 CI만 공유 쓰기, 출처·purge·세대, 표본 재생성, Conflict 0 gate |
| 비결정 처리기로 인한 값 조합 불일치 | 세션 digest 고정·소비 입력 대조(12.1절), Conflict 0 gate |
| GC와 동시 게시의 경합 | epoch mark + 세션 lease + 게시 시 잠금 아래 재확인, 모델 검사·negative control, 읽기 검증 |
| 플랫폼별 내구성 의미 차이 | 6.10절 표, 장애 주입 파일 계층, 내구성 실패를 읽기 검증 → 자가 치유로 처리 |
| .NET GC와 대형 메모리 인덱스 | 참조 없는 struct 배열, 실측 기준선(RT16), `IIndexStore`로 디스크 인덱스 교체 가능 |
| 읽기 검증 CPU 비용 | OS 암호 라이브러리 SHA-256, 실측. 정확성 기능이라 끄는 설정을 두지 않는다 |
| 두 언어의 형식 구현 분기 | 명세 파일 하나에서 상수 생성, 골든 파일 양방향 대조 |
| 로컬 자동 실행·수명 경합 | 상태 기계, (pid, 시작 시각) sponsor, RT10 |
| 처리기 버전 올리기 누락이 사용자 캐시로 퍼짐 | 11.4절 마지막 문단 |

## 17. 다른 문서 개정 사항

이 설계를 승인하면 함께 고친다(아직 반영하지 않았다).

- **AssetSystem.ko.md**
  - 1절 A3 행: 레코드 계약·NuggetFileSystemBackend·NuggetRockpileBackend, R단계 연결.
  - 2절 "프로세스: 없음" 행 → PlatformBase `Process`·`LocalChannel`(R0).
  - 3절 상태("공유·원격 backend 계약" → "NuggetRockpileBackend + Rockpile upstream", C++ 직접 원격 backend 없음)와 의존 그래프(`AssetNugget → RockpileClient`).
  - 8절 "에셋 너겟은 내용 주소 저장소라 잠금 없이 동시 기록" → NuggetFileSystemBackend 한정으로.
  - 9절: 키 공통 항목에 에셋 너겟 세대 Guid, 결과 압축(키 포함)과 저장 압축(키 밖) 구분, `.rnug` v2(= Rockpile Record + 값 byte), Record의 소비 입력(`inputs`)과 적중 때 대조, 읽기 검증 보장 범위(부분 읽기는 블록 CRC 수준), 논리 digest와 Conflict, 쓰기·정리 문장, 인터페이스 문장(12절), SCC Record(12.3절).
  - 13절 명령·옵션(`--verify-nuggets-source=…`, `nugget-gc` 위임), 14절 시나리오 9·13에 Rockpile 경로.
  - 17절 개정 항목, 18절 계약 목록.
- **EngineImplementationPlan.ko.md**: 머리말 개정 기록, 2.4 도구 명명 체계에 Rockpile, 3.1 모듈 표(PlatformBase 행에 프로세스·로컬 채널, AssetNugget·RockpileClient 행), 3.2 트리(`Engine/Source/Developer`, `Programs/Rocks.Rockpile.*`)와 종료 순서에 Put drain, 9.1 에셋 너겟 문단, 27.2(BuildArtifactCache가 Rockpile namespace를 쓴다, `CachePolicy` 이름 구분), 27.7(캐시 조회·게시는 coordinator, worker는 순수 manifest), 27.9·27.10(배포물·고지), 28.3 종료 순서·28.4 service mode·종료 코드, 31절 위험 R17, 33절 교차검증 기록.
- **Compression.ko.md**: 213–214행·1268–1270행에 결과 압축과 저장 압축 구분, 14.1절에 `Codec` 저장 머리(`RCDC`) 형식.
- **RepositoryLayout.ko.md**: `Programs/Rocks.Rockpile.*` 트리.
- **PlatformLayer.ko.md**: P1 프로세스·로컬 채널 행.
- **CoreDataFoundations.ko.md** 132행: `AssetNuggets.json`은 키에 들어가지 않는다.
- **Serialization 구현 문서**: 언어 사이 정본(명세 파일·골든 파일 위치).
- **README.md**: 설계 문서 목록.
- **global.json**: SDK를 런타임 10.0.15 이상을 포함하는 패치로 올린다(13.6절).
- 빌드·검사 코드: `Programs/Rocks.Tools.csproj`, Rocks.Build의 `rockpile` 게시 action, CI workflow(Windows·macOS 시험 단계, Linux 작업), 설치 SDK 출력(실행 파일).

## 18. 후속

R3 project store, R4 BuildArtifactCache, OIDC, 로컬 파일 참조(큰 blob을 handle·경로로 넘겨 복사 생략 — Zen 방식, 측정 뒤. 들어오면 6.8절 "클라이언트는 저장소 파일을 열지 않는다" 전제를 다시 정한다), 웹 대시보드, Windows 서비스 모드, 공유 서버 사이 복제, NativeAOT 배포(13.6절 측정 뒤), 디스크 기반 인덱스(`IIndexStore` 교체), 같은 major 안 새 빌드의 즉시 교체(연결 0 시점 교체 상태).

## 19. 교차검증 기록

### 19.1 1차 — 초안 0(Rust 전제)의 반례 검토 3·독립 설계

초안 0을 세 관점(저장 엔진·프로토콜과 보안·저장소 정합성)의 반례 검토와 초안을 모르는 독립 설계로 대조했다. 그 뒤 사용자가 구현을 .NET으로 바꿨다(19.4절).

| 출처 | 등급 | 지적 | 조치 |
|---|---|---|---|
| 프로토콜·보안 | 차단 | loopback 포트를 다른 사용자가 가로채면 클라이언트가 가짜 서버를 믿는다 | 로컬 전송 pipe·UDS, 서버 정체 확인, 이름 난수(8.1·10.2·11.2절) |
| 프로토콜·보안 | 차단 | 오염 Record를 회수할 수단이 없고 첫 쓰기 우선이 오염을 영구화 | 출처·purge·purge 로그·세대·ReportCorrupt·교체(6.9·7.5·9·11.4절) |
| 프로토콜·보안 | 차단 | 나눠 받기와 backend 전환으로 한 빌드가 같은 키의 다른 Record를 섞음 | 기대 digest·표현 필수, 옛 판 lease, 세션 고정, backend 배타 선택(6.9·8.6·12.1절) |
| 프로토콜·보안, 정합성 | 차단 | 서버의 RCBK 부분 컨테이너 재작성은 블록 선택 오류를 검출 못 함 | 서버는 RCBK를 해석하지 않고 저장 범위만, 클라이언트가 색인을 IndexHash로 검증(5.4·8.6절) |
| 프로토콜·보안 | 차단 | CAS 경로에 namespace가 없어 권한 우회 | blob 소유 namespace(5.5·11.3절) |
| 저장 엔진 | 차단 | 결과가 모호한 commit 뒤 블록 공간 재사용, fsync 재시도 성공 판정 | 저장소 재시작·봉인·번호 재사용 금지(6.5·6.6절) |
| 저장 엔진 | 차단 | 검증 전·내구화 전 blob에 중복 제거하면 앞 쓰기의 실패를 물려받지 않음 | 검증 끝난 blob에만 중복 제거, 받은 byte 보관, Record WAL은 참조 blob과 같거나 뒤 group(6.4절) |
| 저장 엔진 | 차단 | 읽기 경로가 위치를 검증하지 않아 쓰기 순서 버그가 틀린 byte가 됨 | 모든 읽기 검증(6.7절) |
| 저장 엔진 | 차단 | 새 블록 파일 디렉터리 동기화 누락, 블록 id 재사용 | 디렉터리 동기화·high-water·damaged 처리(6.4·6.6절) |
| 저장 엔진 | 차단 | 블록 압축의 flush·위치 갱신 순서 미정, 낡은 값으로 덮어쓰기 | Put과 같은 파이프라인·조건부 갱신·retired 기록(6.8절) |
| 정합성 | 차단 | 저장 압축이 키에 없어 RecordDigest·Conflict가 오진 | 논리 digest, 결과 압축·저장 압축 구분(5.3절) |
| 정합성 | 차단 | Remove 제거 + 첫 쓰기 우선 + 중복 제거로 손상 복구 불가 | ReportCorrupt·격리·교체 허용(6.7·6.9·7.5절) |
| 저장 엔진 | 중요 | refcount 재계산·유예가 진행 중 Put과 경합, 벽시계 | epoch mark-sweep + 세션 lease + 단조 시계(7.1절) |
| 저장 엔진 | 중요 | 지연 삭제한 large 파일이 같은 해시 새 파일을 지움 | 세대 번호 경로(6.3·6.8절) |
| 저장 엔진 | 중요 | 디스크 가득 참에서 회수 불가 | 예비 파일·압박 회수·공간 예약(7.2절) |
| 저장 엔진 | 중요 | upstream 부분 Record를 로컬 게시하면 불변식 위반, 계층마다 승자가 다름 | 완전한 blob 수신 뒤 게시, 소비 입력 대조(5.6·6.9·12.1절) |
| 저장 엔진 | 중요 | repair가 틀린 입력을 막지 못하고 얻는 것이 reset과 같음 | repair 제거(6.6절) |
| 저장 엔진 | 중요 | 플랫폼별 fsync 의미, 부분 page 재기록, 강제 종료 시험의 한계 | 6.10절 표, 4 KiB 경계 group, RT03 |
| 프로토콜·보안 | 중요 | 일괄 응답 위치 대응으로 키 섞임 | echo·대조(5.2·8.4절) |
| 프로토콜·보안, 정합성 | 중요 | "항목 4096 = maxFields"는 오독, 메시지 한도를 Record로 표현 못 함 | byte 예산·`Deferred`·첫 항목 예외·고정폭 값 표·한도 체계(5.2·8.3·8.4절) |
| 프로토콜·보안 | 중요 | upstream 통과의 HOL·중간 실패 | `Pending` ticket, 클라이언트 검사 의무(8.4절) |
| 프로토콜·보안 | 중요 | HTTP 연결 상태·timeout·재시도 규칙 없음 | 8.7절, 로컬은 프레임 |
| 프로토콜·보안 | 중요 | 시작·종료 경합, pid 재사용, 복구가 준비 대기를 넘김 | 상태 기계·(pid, 시작 시각)·`recovering`(10.3–10.5절) |
| 프로토콜·보안, 정합성 | 중요 | 설치 사본 복사 경합·실행 파일 정체성 | 실행 파일 해시 디렉터리(10.1절) |
| 프로토콜·보안 | 중요 | 버전 불일치 시 FileSystem 전환은 사실상 upstream 상실 | major별 인스턴스·저장소 공존(6.1·10.6절) |
| 프로토콜·보안 | 중요 | 저장소 위치 이동 시 권한·네트워크 파일 시스템 | 두 디렉터리 모두 권한 검사, 네트워크 FS 거부(6.1절) |
| 프로토콜·보안 | 중요 | CachePolicy 조합의 빈틈 | 조합 의미표(5.6절) |
| 프로토콜·보안 | 중요 | upstream 전파 손실(CI VM 종료)·권한 세탁 | 내구 큐·drain·로컬 생성 Record만 전파·원 작성자 신원(9절) |
| 프로토콜·보안, 정합성 | 중요 | C++ 계약의 스레드·수명·취소·`Name` bucket | Task 관례·소유권 이전·고정 ASCII bucket(12.2절) |
| 프로토콜·보안 | 중요 | 사용자 단위 공유가 처리기 버전 누락의 피해 범위를 넓힘 | 위험으로 명시(11.4절) |
| 프로토콜·보안, 정합성 | 중요 | CI·터미널에서 분리 실행의 handle 상속·job·신호 | 10.4절 실행 규칙 |
| 정합성 | 중요 | `--verify-nuggets`·`nugget-gc`·AssetNuggets.json 대응 없음 | 12.4절 |
| 정합성 | 중요 | SCC 원자 게시 단위 | SCC 하나 = Record 하나(12.3절) |
| 정합성 | 중요 | 완료 기준이 A4·A5에 의존 | R1a·R1b 분리(2절) |
| 정합성 | 중요 | PlatformBase TCP 소켓은 NetCore 계획과 충돌, C++ HTTP는 범용 기능 중복 | TCP·HTTP를 C++에 넣지 않고 `LocalChannel`·프레임(8.1·12.6절) |
| 정합성 | 중요 | 정본이 둘이 될 위험, 골든 파일 소유 | 명세 파일·골든 소유 분리(8.3절) |
| 정합성 | 중요 | 계획 개정 절 없음, 상태 표기 미적용 | 17절, 2절 상태 열 |
| 정합성 | 중요 | 셰이더 worker가 캐시를 직접 씀, CI 쓰기 정책 | 4절 |
| 정합성 | 경미 | `CachePolicy` 이름 충돌, CLI 명명·옵션 문법·종료 코드 | 5.6·12.4절 |
| 독립 설계 | — | 정확성은 읽기 검증·순서는 가용성용 / 내구 등급 / echo / mark-sweep + lease / 출처·purge / 인스턴스 major 분리 / 큰 blob 원본 그대로 | 채택(6.4·6.7·7.1·10.6·11.4절) |
| 독립 설계 | — | 원본 범위를 위해 블록 색인을 Record에 인라인 | 버림 — 색인 길이·해시만 행에 두고 색인은 범위로 받는다(8.6절) |
| 독립 설계 | — | 로컬도 HTTP/1.1(pipe·UDS 위) | 버림 — C++ HTTP 구현이 범용 기능 중복. 프레임 봉투(8.1절) |
| 사실 확인(.NET) | — | Kestrel named pipe 전송은 원격 거부를 표현 못 하고 UDS 전송은 권한·남은 소켓을 다루지 않는다 | 로컬 endpoint 자체 수락 루프, DACL로 NETWORK 거부(13.2·13.4절) |
| 사실 확인(.NET) | — | `Flush(true)`가 Linux 일부 오류를 무시, macOS 10.0.14까지 `F_FULLFSYNC` 실패 무시. 디렉터리 동기화 API 없음 | 자체 `DurableIo`, 런타임 10.0.15 이상 검사(13.3·13.6절) |
| 사실 확인(.NET) | — | macOS 서버 TLS 1.3 미지원, 클라이언트 기본 TLS 1.2 | 최소 TLS 1.2, 공유 서버 정식 호스트 Linux(13.2절) |
| 사실 확인(.NET) | — | .NET 10 `Process.Start`는 Windows handle 상속을 끌 수 없다 | R1a 자동 실행은 C++만(13.5절) |
| 사실 확인(.NET) | — | Server GC 작업 집합, NativeAOT의 MSVC 요구, `System.IO.Hashing`은 NuGet | 로컬 Workstation GC, framework-dependent single-file, `BitOperations.Crc32C`(13.2·13.6절) |

### 19.2 2차 — 개정 1의 최종 검증 2(새 메커니즘 반례·정합성)

| 출처 | 등급 | 지적 | 조치 |
|---|---|---|---|
| 반례·정합성 | 차단 | 프로토콜 major만 다른 두 인스턴스가 같은 `Store-v<N>`을 각자 잠금으로 연다 | 저장소 경로 `p<P>-s<S>`, 저장소 자체 잠금(dev/ino 재확인)(6.1절) |
| 반례·정합성 | 차단 | `cache` 등급이 Put 판정(Exists·Conflict·NeedBlobs) 전에 응답 | blob 경로와 Record 판정 분리, 판정 뒤 게시 대기 예약 → 응답, WAL에는 판정 통과 Record만(6.4절) |
| 정합성 | 차단 | 옮긴 저장소 디렉터리의 소유자·권한 미검사 | 제어·저장소 모두 권한 검사(6.1절) |
| 정합성 | 차단 | 부분 읽기가 digest만 고정해 같은 digest의 다른 저장 표현을 섞음 | 기대 ContentHash, `RepresentationChanged`(8.4·8.6·12.1절) |
| 반례 | 중요 | fsync 실패 뒤 재시작이 영속 안 된 WAL을 재생하고 이어 씀, 번호 충돌 | 세그먼트 단위 재생·새 세그먼트·high-water 기반 번호·고아 삭제 범위(6.6절) |
| 반례 | 중요 | flush 실패 반복 시 재시작 루프 | 재시작 상한·저하 모드·오류 종류표·`.corrupt` 하나(6.5·6.6절) |
| 반례 | 중요 | 세션 고정과 교체가 부딪힐 때 재생성 결과 처리 없음 | 옛 판 lease 10분, `GetRecords` 기대 digest, `DigestChanged` 시 Record 전체 다시(6.9·8.4·12.1절) |
| 반례 | 중요 | upstream 수렴으로는 의존 결과 섞임을 못 막음(읽기 전용 기기는 전파 안 함) | Record `inputs`와 적중 때 대조(5.2·12.1절) |
| 반례 | 중요 | "Conflict면 로컬 값"과 "적중 먼저·로컬 기록 비동기"의 모순 | 응답 직전 로컬 재확인·키 자리 예약(5.6·9절) |
| 반례 | 중요 | 같은 digest 안 표현 교체의 순서·연속성, `Codec` 영구 미적중 | 판정 순서, `RepresentationChanged`, `Codec` 정책·교체(6.9·8.6·5.2절) |
| 반례·정합성 | 중요 | 조각 해시 표가 GC 루트·자가 치유 밖 | ChunkTableHash를 행에, 표는 Record 참조 blob, 함께 격리·치유(5.2·6.7·7.1절) |
| 반례 | 중요 | Identity 범위·upstream 통과 범위의 검증 공백, GetChunks 프레임 뒤바뀜 | upstream 통과 범위 검증 규칙, echo에 ValueId·범위, 프레임 대조 의무(6.7·8.4절) |
| 반례·정합성 | 중요 | 서버의 RCBK 해석이 헤더로 끝나지 않고 검증되지 않은 블록 표를 믿음 | 서버는 RCBK를 해석하지 않음, `indexLength`·클라이언트 계산(5.4·8.6절) |
| 반례 | 중요 | ReportCorrupt의 원인 오판·비멱등·남용 | 판 번호 멱등, 해시 정상이면 교체 허용·의심 횟수, 속도 제한(7.5절) |
| 반례 | 중요 | lease 범위 모호, 압박 모드 livelock, 단조 시계 영속 | Put 세션 lease·갱신·압박 중 유지·읽기 전용 제외, 단조 시계 비영속(7.1절) |
| 반례·정합성 | 중요 | `Pending`·`Deferred`가 끝나지 않음, 한도 근처 Record 운반 불가, 프레임 중복 규칙 충돌 | ticket 보관·long-poll, 첫 항목 예외, Record 1 MiB·메시지 8 MiB, 중복 키(kind, hash, offset, length)(8.2–8.4절) |
| 반례 | 중요 | 30일 정리가 실행 중 인스턴스를 지움 | 비차단 잠금 획득 시에만, `last-used`, 잠금 파일 보존(10.6절) |
| 반례·정합성 | 중요 | 같은 major 안 버전 변경 시 거부 | 와이어 변경 = major 올림 규칙, 명세 해시 대조(8.2절) |
| 반례 | 중요 | 예측 가능한 pipe 이름 선점, 권한 상승 혼합 | 이름 난수, 상승 프로세스 자동 실행 금지·소유자 명시(10.2·10.4·13.4절) |
| 반례 | 중요 | 재시작 사이 저장소·upstream이 바뀌면 R-2 위반 | 저장소 Guid·upstream 구성 해시 세션 고정(12.1절) |
| 반례·정합성 | 중요 | purge가 파생 Record·로컬 사본에 닿지 않음, 응답에 출처 없음 | 파생 폐포 purge, 응답 출처, `PurgeLog`(9·11.4절) |
| 정합성 | 중요 | 값 행 크기 계산 오류 | 행 160 byte로 다시 정의(5.2절) |
| 정합성 | 중요 | 12.2 스케치가 Task 타입·관례와 다름(`WorkerTaskHandle` 이동 전용, 소멸 규칙, completion lane) | `const WorkerTaskHandle&`, 소멸 = 취소 뒤 대기, `WorkerTaskEvent`, 콜백 없음(12.2절) |
| 정합성 | 중요 | 개발 트리에서 `rockpile`을 찾지 못함 | Rocks.Build가 `<NativeOutput>/bin/`에 게시, 탐색 순서(10.1절) |
| 정합성 | 중요 | 런타임 10.0.15를 global.json으로 강제할 수 없음 | 시작 시 `Environment.Version` 검사(13.6절) |
| 정합성 | 중요 | "저장 압축은 키에 없다"의 근거가 Compression 문서와 다름 | 결과 압축·저장 압축 구분, Compression 개정 항목(5.3·17절) |
| 정합성 | 중요 | 로컬 blob 업로드 연산·HTTP 경로 누락 | `PutBlobs`, HTTP 경로 표, `Info` RPC 통일(8.1·8.4절) |
| 반례·정합성 | 경미 | 상태 목록 누락, 정책 합집합의 Skip 비트, 명령 목록, `backend=auto`, `SyncDirectory` 위치, `TryOpen` 인자, `CachePolicy` 충돌 대상, LocalChannel 구현 모순, C# 정체 확인 범위, 유휴 종료와 큐, 골든 소유(Core↔Engine), [S6] URL, 옵션 문법·종료 코드, `Codec` 머리 형식, 공유 blob 용량 회계, flags 의미, large 경로용 전체 해시, drain 손실 보고, `$TMPDIR` 소켓 정리, Put 상한 동작, SCC 세부, writer 왕복 검증 | 각 절에 반영(5–15절) |
| 정합성 | — | 17절 누락(계획 2.4·3.2 종료 순서·28.3·28.4·27.7, Compression, AssetSystem 8·13, RepositoryLayout C# 관례, CI 단계) | 17절에 추가, C# 프로젝트는 `Programs/Rocks.Rockpile.*`(13.1절) |

### 19.3 사실 출처(2026-11-13 확인, 모두 1차)

- [S1] UE 5.6 문서, Using Derived Data Cache — https://dev.epicgames.com/documentation/en-us/unreal-engine/using-derived-data-cache-in-unreal-engine?application_version=5.6 (5.4부터 로컬 기본 Zen)
- [S2] UE 5.6 문서, Set up Zen Storage Server as Shared DDC — https://dev.epicgames.com/documentation/en-us/unreal-engine/set-up-zen-storage-server-as-shared-ddc-for-unreal-engine?application_version=5.6 (계층 배치, 인증·권한 없음, 공유 템플릿 GC 값, 포트 8558)
- [S3] UE 5.6 문서, Cloud type DDC — https://dev.epicgames.com/documentation/en-us/unreal-engine/how-to-set-up-a-cloud-type-derived-data-cache-for-unreal-engine?application_version=5.6 (Unreal Cloud DDC, OIDC·namespace ACL, `Engine/Source/Programs/UnrealCloudDDC`)
- [S4] EpicGames/zen 소스 v5.6.6(UE 5.6 포함 판, Epic 연동 계정 필요) — `src/zenstore/cas.cpp`·`compactcas.cpp`·`cache/cachedisklayer.cpp`·`gc.cpp`, `src/zenserver/config.cpp`·`cache/httpstructuredcache.cpp`, `src/zenhttp/packageformat.cpp`, `CHANGELOG.md`; UE 5.6 `Engine/Config/BaseEngine.ini`, `Engine/Source/Developer/Zen/Private/ZenServerInterface.cpp`, `Engine/Source/Developer/DerivedDataCache/Private/ZenCacheStore.cpp`
- [S5] UE 5.6 소스 — `Engine/Source/Runtime/Core/Public/IO/IoHash.h`, `Serialization/CompactBinary.h`·`CompactBinaryPackage.h`, `Private/Compression/CompressedBuffer.cpp`, `Engine/Source/Developer/DerivedDataCache/Public/DerivedDataCacheRecord.h`·`DerivedDataCachePolicy.h`·`DerivedDataValueId.h`
- [S6] UE release notes — https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5-5-release-notes?application_version=5.5 , https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5-6-release-notes?application_version=5.6 (공유 Zen Windows production ready(5.5), 쿠킹 출력 저장소 production ready·Zen Streaming Beta(5.6))
- [S7] UE 5.6 소스 — `Engine/Source/Programs/UnrealCloudDDC/Jupiter/Jupiter.csproj`, `Engine/Source/Programs/Horde/HordeServer/HordeServer.csproj`(`Microsoft.NET.Sdk.Web`, net8.0)
- [S8] Kestrel — https://learn.microsoft.com/aspnet/core/grpc/interprocess-namedpipes?view=aspnetcore-10.0 , https://learn.microsoft.com/aspnet/core/grpc/interprocess-uds?view=aspnetcore-10.0 , https://learn.microsoft.com/aspnet/core/fundamentals/servers/kestrel/options?view=aspnetcore-10.0 ; dotnet/aspnetcore `release/10.0` `Transport.NamedPipes/src/NamedPipeTransportOptions.cs`·`Internal/NamedPipeConnectionListener.cs`, `Transport.Sockets/src/SocketConnectionListener.cs`, `Core/src/KestrelServerLimits.cs`
- [S9] .NET TLS(macOS) — https://devblogs.microsoft.com/dotnet/dotnet-10-networking-improvements/ , dotnet/runtime `SslStreamPal.OSX.cs`, https://support.apple.com/guide/security/sec100a75d12
- [S10] GC·배포 — https://learn.microsoft.com/aspnet/core/performance/memory?view=aspnetcore-10.0 , https://learn.microsoft.com/dotnet/standard/garbage-collection/datas , https://learn.microsoft.com/dotnet/core/runtime-config/garbage-collector , https://learn.microsoft.com/dotnet/core/deploying/native-aot/ , https://learn.microsoft.com/aspnet/core/fundamentals/native-aot?view=aspnetcore-10.0
- [S11] 해시 — https://learn.microsoft.com/dotnet/standard/security/cross-platform-cryptography , https://learn.microsoft.com/dotnet/api/system.io.hashing.crc32parameterset , `System.Numerics.BitOperations.Crc32C`
- [S12] dotnet/runtime `release/10.0`·태그 10.0.14/10.0.15 — `FileStreamHelpers.Windows.cs`·`FileStreamHelpers.Unix.cs`·`pal_io.c`(flush), https://github.com/dotnet/runtime/pull/124725 (`F_FULLFSYNC` 오류 처리), `FileSystem.Unix.cs`(Move), `SafeFileHandle.Unix.cs`, Process(Windows `CreateProcess`), https://devblogs.microsoft.com/dotnet/process-api-improvements-in-dotnet-11/ , https://learn.microsoft.com/dotnet/api/system.diagnostics.process.starttime , https://learn.microsoft.com/nuget/concepts/auditing-packages

### 19.4 결정 기록

| 날짜 | 결정 | 근거 |
|---|---|---|
| 2026-11-13 | 이름 Rockpile | 사용자 결정(후보: Cairn·Strata·Rockpile·Pebble·Boulder) |
| 2026-11-13 | 구현 .NET 10·C#(초안 0은 Rust) | 사용자 결정. 근거: 기존 .NET 도구 체계(새 도구 체인·CI·경고 정책 예외 불필요), 공유 프레임워크만으로 HTTP·TLS·pipe·UDS·SHA-256 해결, 형식 구현을 C++·C# 둘로 한정(Rocks Build와 공유), Epic의 공유·클라우드 캐시(Unreal Cloud DDC)도 C#. 대가: GC로 인한 꼬리 지연·메모리 예측성(16절) |
| 2026-11-13 | 빌드 캐시 → **에셋 너겟**(모듈 `AssetNugget`, 계층 `NuggetCache`, `NuggetBackend`·`NuggetPolicy`, 값 파일 `.rnug`, `Config/AssetNuggets.json`, `nugget-gc`·`--verify-nuggets`, namespace `rocks.nuggets`). 부제 "에셋 너겟 서버" | 사용자 결정. "빌드 캐시"가 코드 빌드 캐시로 오해된다. 코드 산출물 캐시 `BuildArtifactCache`(R4)는 다른 개념이라 이름을 유지한다 |
