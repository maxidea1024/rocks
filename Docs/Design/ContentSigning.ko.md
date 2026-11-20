# 콘텐츠 서명 — 서명된 쿠킹 매니페스트(청크 표) 설계안

> 상태: 채택(2026-11-15 사용자 결정: 추천안, 구현은 AssetRuntime A1과 함께). 리뷰 1회 반영. 구현 A1f(2026-11-21 — 7절, 시험·coverage는 [AssetSystem](AssetSystem.ko.md) 14.1.11·14.1.12). [Crypto](Crypto.ko.md) 13절 후속 판단과
> 엔진 계획 9.3절을 따른다.

## 1. 범위

- **막는 것.** 배포 경로(CDN·미러·패치 서버·사용자 사이 공유)에서 바뀌거나 섞인 콘텐츠, 손상된 패치, 옛 콘텐츠로 되돌리기(rollback),
  다른 제품·채널(beta 등)의 콘텐츠를 섞어 넣기.
- **막지 않는 것.** 실행 파일까지 고칠 수 있는 로컬 공격자(치트·크랙). 실행 파일 무결성은 OS 코드 서명 몫이고 신뢰 공개키는 그 실행
  파일 안에 있다. 클라이언트가 서버에 알리는 콘텐츠 버전은 자기 신고라 이것으로 검증되지 않는다. 콘텐츠 기밀성(암호화)도 범위 밖이다.
- **이미 있는 것.** [AssetSystem](AssetSystem.ko.md) 10절 `CookedManifest.rman`이 패키지마다 SHA-256·크기를 담고, 로더가 패키지를 열 때
  확인한다. 매니페스트에 서명하면 **패키지 byte는 모두 묶인다.** Release가 읽는 콘텐츠는 매니페스트 항목이거나 실행 파일 안에만
  있어야 한다(매니페스트 밖 파일 — 프로젝트 설정, 프로파일 재정의, 셰이더·지역화 파일 — 은 Release에서 읽지 않거나 매니페스트 항목으로
  넣는다). 이것을 AssetRuntime 요구사항으로 둔다.

## 2. 결정 요약

| 코드 | 질문 | 추천안 | 대안 |
|---|---|---|---|
| CS1 | 서명 위치 | 릴리스 단계에서 파일 하나 `CookedManifest.rsig` = 헤더 ‖ 서명 ‖ 매니페스트 byte(봉투). 교체는 Replace 한 번이고, 로더는 한 번 읽은 버퍼를 검증하고 같은 버퍼를 파싱한다. 매니페스트 byte와 쿠킹 결정성은 그대로다 | rman 옆 분리 서명 파일(교체가 두 번이라 원자적이지 않고, 검증한 byte와 쓰는 byte가 갈릴 수 있다) |
| CS2 | 알고리즘 | Ed25519ph(Crypto 9.2절 증분 검증, `TryFinishAny`로 신뢰 키 여럿) | 순수 Ed25519 |
| CS3 | 신뢰 키 | 프로젝트 설정의 공개키 목록(KeyId·32 byte)을 빌드가 실행 파일에 넣는다. 콘텐츠 서명 전용 키이고 제품·채널마다 따로 둔다. 폐기는 새 실행 파일로 한다 | 신뢰 키 파일 배포(바꿔치기 가능) |
| CS4 | rollback | 서명한 `contentVersion`(u64). 실행 파일의 최소 버전보다 낮으면 거부한다. 패처는 (keyId → 받은 최고 버전)을 기록해 그보다 낮은 패치를 거부하고, 신뢰 목록에서 빠진 키의 기록은 버린다(탈취 키가 최대 버전으로 하한을 오염시켜도 폐기 뒤 풀린다) | 버전 없음 |
| CS5 | 적용 정책 | Release는 봉투가 없거나 틀리면 시작하지 않는다. Development는 경고만, 개발 쿠킹 디렉터리는 서명하지 않는다 | 모든 구성 필수 |
| CS6 | 부분 읽기 | v1은 패키지 전체 SHA-256(현 로더). mip 스트리밍 등 부분 읽기가 생기면 패키지 안 1 MiB 조각 해시 표와 그 SHA-256을 매니페스트 항목에 둔다(Rockpile `ChunkTableHash`와 같은 구성) | 지금 도입(소비자 없음) |

## 3. 봉투 `CookedManifest.rsig` v1

| offset | 크기 | 필드 |
|---|---|---|
| 0 | 4 | magic `RSIG` |
| 4 | 1 | 버전 1(모르면 `Unsupported`) |
| 5 | 1 | 서명 알고리즘 = `SignatureAlgorithm::Ed25519ph`(2) |
| 6 | 2 | 0(예약, 0이 아니면 거부) |
| 8 | 16 | keyId |
| 24 | 8 | contentVersion(LE64) |
| 32 | 32 | scope = SHA-256(productId ‖ 0 ‖ channel) — 실행 파일의 값과 같아야 한다 |
| 64 | 8 | 매니페스트 byte 수(LE64) |
| 72 | 64 | 서명 |
| 136 | n | 매니페스트 byte(`CookedManifest.rman`과 같은 내용) |

서명 메시지(Ed25519ph 입력): `"rocks.content-signature.v1\0" ‖ 봉투[0, 72) ‖ 매니페스트 byte`. libsodium Ed25519ph에는 context 인자가
없으므로 NUL로 끝나는 label 접두가 도메인 분리다(키가 콘텐츠 서명 전용이라는 CS3 규칙과 함께 성립한다). 프로파일 id는 매니페스트 헤더에
있어 이미 서명 범위 안이고 로더가 대조한다.

## 4. 로더 검증 순서

1. 봉투 전체를 버퍼 하나로 한 번 읽는다(크기 상한을 둔다). 이후 파일을 다시 읽지 않는다.
2. magic·버전·알고리즘·예약 → keyId가 신뢰 목록에 있음 → contentVersion ≥ 실행 파일 최소 버전 → scope 일치 → 매니페스트 길이 = 남은 byte.
3. 같은 버퍼로 Ed25519ph 검증. 실패는 `AuthenticationFailed` 하나로 보고한다(단계는 진단 로그에만).
4. 같은 버퍼의 매니페스트 byte를 파싱한다. 이후 패키지는 열 때 크기·SHA-256을 매니페스트와 대조한다.

## 5. 서명 도구·키·패치

- `content-sign` commandlet: 입력 매니페스트, 서명 키(KeyBlob 비밀번호 방식 `Ed25519SigningKey`, Crypto 10.4절), contentVersion,
  productId·channel → 봉투. 같은 입력이면 같은 byte다. 키 생성·공개키 내보내기 명령을 함께 둔다. 서명 키는 빌드 기계 밖(오프라인·HSM)에
  두고 릴리스 때만 쓴다.
- 패치: 새 패키지(내용 주소라 새 이름)를 먼저 놓고, 패처는 새 봉투의 서명·scope·버전과 새 패키지 전부의 SHA-256을 확인한 뒤 봉투를
  Replace 한 번으로 바꾼다. 옛 콘텐츠로 되돌리려면 옛 매니페스트를 더 높은 contentVersion으로 다시 서명해 낸다(엔진 계획 9.3절의
  "구 manifest로 rollback"은 이 재서명 방식으로 고친다).

## 6. 구현 위치와 단계

- Crypto: 새 형식은 두지 않는다. `Ed25519phSigner`·`Ed25519phVerifier::TryFinishAny`·KeyId·KeyBlob을 그대로 쓴다.
- AssetRuntime(A1)과 배포 도구: 봉투 형식 읽기·쓰기, 신뢰 키 목록·최소 버전·scope 생성(빌드 규칙), 4절 검증, CS5 정책, Commandlet
  `content-sign`. AssetRuntime이 아직 없으므로 형식 코드와 도구를 A1과 함께 구현한다.
- 검증: golden 봉투(고정 키), 헤더 모든 필드와 매니페스트 1 bit 변조·자르기·늘리기, 다른 scope, 낮은 버전, 모르는 키·버전·알고리즘,
  예약 byte, 길이 오류, 독립 구현(PyNaCl·`cryptography` Ed25519ph)과 서명 byte 일치.

## 7. 구현에서 정한 점(A1f — 2026-11-21)

AssetRuntime 다음 단계(A1f, [AssetSystem](AssetSystem.ko.md) 1절)가 1–6절을 다음과 같이 정했다. 애매한 곳은 UE 5.8 pak 서명(`FPakSignatureFile` — 서명 파일 + 공개키를 실행 파일에 넣고 불일치면 거부)을 따른다.

1. **위치**: 봉투 형식·검증·쓰기는 AssetRuntime `ContentSignature.h`(Crypto 의존을 더한다 — Core 층 모듈이라 의존 방향이 맞다). 쓰는 쪽은 `content-sign` commandlet이고 읽는 쪽은 EngineAssets다.
   - `ContentScope`: `MakeContentScope(productId, channel)` = SHA-256(productId ‖ 0 ‖ channel). productId·channel은 ASCII 식별자(빈 값·NUL 금지 — InvalidArgument).
   - `ContentTrust`: 신뢰 키 목록(`TrustedContentKey { crypto::KeyId id; crypto::Ed25519PublicKey key; }`, 같은 id 두 번 금지), 최소 contentVersion, scope.
   - `WriteContentEnvelope(allocator, manifestBytes, signingKey, keyId, contentVersion, scope, out)`: 3절 배치 그대로 쓰고 서명 메시지(3절)를 Ed25519ph로 서명한다. 같은 입력이면 같은 byte(Ed25519는 결정적이다). 매니페스트가 `CookedManifestMaxFileBytes`를 넘으면 `CapacityExceeded`(읽기 전), null keyId `InvalidArgument`, 실패하면 out은 그대로다.
   - `VerifyContentEnvelope(allocator, envelope, trust, info, failure) → Result<Span<const uint8>>`: 4절 순서로 검증하고 같은 버퍼 안의 매니페스트 byte span을 돌려준다(복사 없음 — 검증한 byte와 파싱하는 byte가 같다). allocator는 Ed25519ph 검증 상태(SHA-512)를 받는다(할당 실패를 시험할 수 있게 — Crypto 기본값은 비밀 저장소지만 공개 데이터 해시라 일반 allocator로 충분하다).
2. **오류·진단**(4절 "단계는 진단 로그에만"을 진단 칸으로 둔다 — 호출자가 로그에 쓴다): 길이(헤더 136 byte 미만, 봉투 상한 `CookedManifestMaxFileBytes` + 136 초과)·magic·예약 byte·매니페스트 길이 ≠ 남은 byte는 `CorruptPackage` / `InvalidArgument`, 버전·알고리즘을 모르면 `UnsupportedVersion` / `Unsupported`, 신뢰 판정(모르는 keyId·contentVersion < 최소·scope 불일치·서명 불일치)은 모두 `AuthenticationFailed`이고 진단 코드 `ContentNotTrusted`다. 걸린 칸은 `AssetDiagnostic::section`에 `ContentEnvelopeField`(File·Magic·FormatVersion·Algorithm·Reserved·Key·ContentVersion·Scope·ManifestLength·Signature, 200부터 — 매니페스트 칸 100–과 겹치지 않는다)로 둔다(매니페스트 reader의 칸 보고와 같은 방식). 신뢰 정보 자체가 잘못됐으면(키 없음·null id·같은 id) 봉투를 보기 전에 `InvalidArgument`(진단 None)다. 판정 순서는 4절 그대로이고 앞 단계가 이긴다(변조된 봉투가 어느 단계에서 걸리는지 결정적이다).
3. **신뢰 정보의 출처**(CS3): 신뢰 키 목록·최소 버전·scope는 실행 파일 안에 있어야 한다. 빌드 규칙이 프로젝트 설정에서 생성 TU를 만들어 넣는 것은 BuildTargets 쪽 후속(빌드 세션과 조율)이고, 그 생성 코드와 지금의 게임 코드가 같은 진입점 `EngineAssetsSubsystem::SetContentTrust(const ContentTrust*)`(엔진 초기화 전, 게임 스레드, 포인터는 엔진보다 오래 산다)를 부른다. 어느 쪽이든 키는 실행 파일 코드·데이터 안이라 위협 모델(1절 — 실행 파일까지 고치는 공격자는 범위 밖)이 같다.
4. **적용 정책**(CS5, EngineAssets `Initialize` — AssetSystem 11.8절 4의 매니페스트 읽기를 바꾼다):
   - `<root>/CookedManifest.rsig`가 있으면 어느 구성이든 검증하고, 실패하면 엔진 초기화 실패(신뢰 정보가 등록되지 않았으면 `InvalidArgument` — "content trust is not configured").
   - 봉투가 없으면 Release 구성(`IsReleaseBuild`)은 `NotFound`로 실패한다(Release는 서명된 콘텐츠만). Debug·Development는 `CookedManifest.rman`을 읽고 Warning 한 번("cooked content is not signed")이다 — 개발 쿠킹 디렉터리는 서명하지 않는다.
   - 봉투와 rman이 함께 있으면 봉투만 쓴다(rman은 읽지 않는다 — 검증하지 않은 byte를 쓰지 않는다).
   - 실패 기록은 EngineAssets 초기화 실패 한 줄이고 단계 이름이 걸린 곳을 말한다: `content trust`(신뢰 정보 없음), `content signature (<칸>)`(봉투 검증 — 칸은 `ContentEnvelopeFieldName`, 신뢰 정보 자체가 틀렸거나 할당 실패면 `None`), `content signature read`(봉투 파일 읽기 오류 — 없음 밖), `content signature`(Release에서 봉투 없음), 그 뒤 `manifest read`·`manifest parse`. 서명으로 시작했는지와 contentVersion은 `EngineAssetsSubsystem::Stats()`(`contentSigned`·`contentVersion`)로 본다.
   - 봉투 파일은 한 번 읽고(크기 상한은 2의 봉투 상한) 그 버퍼에서 검증·파싱한다.
5. **`content-sign` commandlet**(5절): `content-sign sign <in.rman> <out.rsig> --key <key blob> --password-file <path> --version <u64> --product <id> --channel <name>`(입력은 `ReadCookedManifest`로 검증한 뒤 서명한다 — 매니페스트가 아닌 파일에 서명하지 않는다) — 서명 키는 비밀번호 방식 KeyBlob(`Ed25519SigningKey`, Crypto 10.4절)이고 비밀번호는 파일로만 받는다(명령줄·셸 기록에 남기지 않는다 — 끝의 줄바꿈 하나는 뺀다. PlatformBase에 환경 변수 API가 없어 파일로 정했다 — CI는 비밀을 임시 파일로 넘긴다). `content-sign verify <in.rsig> --trust <public> --product <id> --channel <name> [--minimum-version <u64>]`은 엔진과 같은 순서로 봉투를 검증하고 안의 매니페스트를 확인한다(배포 전 확인·시험 왕복용 — 거부면 걸린 칸을 알린다). `content-sign keygen <out key blob> <out public> --password-file <path>`은(비밀번호 비용 `PasswordCostModerate` — 도구 키 파일 권장값) 새 키(KeyId 무작위)를 만들고, 공개 파일은 텍스트 한 줄 `<keyId hex 32> <public key hex 64>`(신뢰 목록에 넣는 값)다. 기존 파일은 덮어쓰지 않는다. 출력 봉투는 임시 파일에 쓴 뒤 Replace한다(원자적 교체 — CS1). 사용법 오류 2, 파일·키·서명 오류 1, 성공 0(Commandlet 규약).
6. **후속**: 패처의 (keyId → 받은 최고 버전) 기록(CS4)은 패처가 생길 때, 빌드 규칙의 신뢰 정보 생성(3), 부분 읽기 조각 해시(CS6)는 mip 스트리밍 때다.
7. **시험**(6절 검증 항목): golden 봉투(고정 seed 키 — 독립 구현인 RFC 8032 참조 Python 코드로 만든 서명 byte와 같다), 헤더 모든 칸·매니페스트 1 bit 변조, 자르기·늘리기, 다른 scope, 낮은 버전, 모르는 키·버전·알고리즘, 예약 byte, 길이 오류, 판정 순서(겹친 위반), 쓰기 인자 오류·할당 실패, EngineAssets 정책(구성별 봉투 있음·없음 × 신뢰 등록 있음·없음 × 검증 성공·실패), commandlet 왕복(keygen → sign → 엔진 검증)과 사용법 오류.
