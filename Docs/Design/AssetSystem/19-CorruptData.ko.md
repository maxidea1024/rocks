## 19. 손상 데이터 오류 `CorruptData` 이전(2026-11-26)

Core에 범용 오류 `ErrorCode::CorruptData`가 생겼다(12342451 — 사용자 지시, Error.h 주석: 읽은 데이터가 손상됐거나 형식 규칙을 어긴다, 호출자 인자 오용은 InvalidArgument, 인증·무결성 검증 실패는 AuthenticationFailed, 데이터를 다시 만들거나 받으면 성공할 수 있다). 에셋 계열은 그전까지 손상 입력을 `InvalidArgument`로 냈다(직렬화 reader 주석 "truncation·잘못된 bool·UTF-8은 InvalidArgument", ImageDecodeFailure "InvalidSource/InvalidArgument" 등). 이 장은 그 판정을 새 값으로 옮기는 규칙과 단계다(사용자 지시 2026-11-26 — 압축 단계보다 먼저).

### 19.1 분류 규칙

| 원인 | 오류 | 예 |
|---|---|---|
| 읽은 byte의 구조·길이·필드·체크섬·형식 규칙 위반(잘림, 범위 밖 tag·varint, 잘못된 bool·UTF-8, CRC·Adler·SHA-256 불일치, 머리 불일치, 서로 맞지 않는 크기 필드, 비유한 표본) | `CorruptData` | Archive·Record reader, 패키지·매니페스트·너겟·bulk 검증, 이미지 원본 디코더, 색인 캐시 |
| 호출자가 넘긴 인자·옵션·API 순서 위반(빈 span, null, 잘못된 형식 enum, 겹치는 범위) | `InvalidArgument`(그대로) | writer API, 디코더 옵션 |
| 읽은 데이터가 한도를 넘음(크기·작업량·깊이) | `CapacityExceeded`(그대로) | 디코딩 한도, archive 작업 한도 |
| 지원하지 않는 변형(압축 방식·형식 판·기능) | `Unsupported`(그대로) | TIFF 압축, 미래 판 |
| 암호 인증 실패(tag·MAC·서명) | `AuthenticationFailed`(그대로) | 서명 봉투 |
| 읽은 데이터가 가리키는 대상이 없음·종류가 다름 | `NotFound`·`TypeMismatch`(그대로) | 모르는 타입 id, 다른 스키마 |

- 체크섬(CRC32·Adler-32·내용 SHA-256)은 손상 검출이지 인증이 아니므로 `CorruptData`다(암호 MAC·서명만 AuthenticationFailed).
- 사람이 쓰는 텍스트 입력(설정 JSON·`*.ini`·명령 인자·대입 텍스트)의 문법·필드 위반은 지금처럼 진단 + `InvalidArgument`다 — "다시 만들거나 받으면 성공"이 아니라 사람이 고쳐야 하는 입력이다(진단 코드가 원인을 알린다). 도구가 쓰고 도구가 읽는 파일(색인 캐시·별칭 표·매니페스트·쿠킹 보고서·너겟·import record)은 `CorruptData`다.
- 진단 코드(`CorruptPackage`·`InvalidSource`·`CorruptManifest` 등)는 그대로이고 오류 값만 바뀐다. 진단 None + 오류만 있는 경로도 같다.
- 하부의 `CorruptData`는 위층이 그대로 전달한다(원본 오류 코드 보존 — 로깅 규칙과 같다). 위층이 같은 뜻을 다른 오류로 바꾸던 곳(예: 읽기 실패를 `InvalidArgument`로 덮음)은 전달로 고친다.

### 19.2 단계

| 단계 | 범위 | 모듈 |
|---|---|---|
| CD-A | 직렬화 reader: Archive·Record·RecordList·ValueArray·Map·Set·Optional·Text·InstancedStruct wire 읽기, 헤더 주석, 시험 | Core Serialization |
| CD-B | 객체 그래프 읽기: GraphLoad·GraphEnvelope·StructRecord·FieldValue 해석(ObjectRuntime), 클래스 기본값 레코드 읽기 | ObjectSerialization·ObjectRuntime |
| CD-C | 패키지·매니페스트·bulk·너겟: PackageReader·PackageLoad·CookedManifest·MemoryBulkSource 검증, NuggetRecord·NuggetFile | AssetRuntime·AssetNugget |
| CD-D | 이미지 원본: ImageDecodeFailure 분류(InvalidSource/CorruptData), 디코더 아홉, TextureTools 원본 해석, fuzz `AcceptableFailure` | ImageCodec·TextureTools |
| CD-E | 도구 파일: 색인 캐시·별칭 표·import record·쿠킹 매니페스트 재사용·보고서 읽기 | AssetDatabase·AssetCooker |

단계마다 그 층의 `InvalidArgument` 반환을 하나씩 19.1 규칙으로 가르고, 손상 입력 시험의 기대값을 `CorruptData`로 바꾼다(인자 오용 시험은 그대로). 공개 헤더의 오류 주석을 같이 고친다. 다른 세션이 쓰는 경로(ShaderCompilerCommon의 직렬화 읽기)는 CD-A 뒤 전 세션에 알린다.
