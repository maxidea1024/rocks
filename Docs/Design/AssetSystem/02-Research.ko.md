## 2. 사전 조사(2026-11-12)

| 영역 | 있는 것 | 부족한 것 → 보강 |
|---|---|---|
| 객체 | `ObjectRegistry`(ContextId 문맥)·`RootRef`·`ObjectRef`(같은 Registry 안에서만)·`WeakObjectRef`·`Borrow`(다른 문맥 Registry는 WrongContext), `ConstructionContext`(스테이징한 객체를 한 번에 공개, RootRef는 첫 객체에만), `TypeId = Guid` | Outer·Name, 다중 root Commit(root마다 RootRef) → A0-O([ObjectFoundations](../ObjectFoundations.ko.md)) |
| 리플렉션 | MetaCompiler: 스칼라·Guid·String·`ObjectRef`, 모든 비정적 필드 리플렉션 필수, 상속 base·참조 대상은 같은 생성 단위, 스키마 버전 1 고정, 타깃 소유 메타데이터·명세 | → A0-B([ReflectionFoundations](../ReflectionFoundations.ko.md)) |
| 직렬화 | Archive·Record(TLV, 반복 필드 없음), `SaveObjectGraph/LoadObjectGraph`(단일 root, 내부 Commit, 마이그레이션 콜백, maxObjects 1024) | → A0-O graph v2(호출자 저장 집합, 단계형 복원·호출자 Commit, 외부 참조 — ObjectFoundations 5절), `RecordList` → A0-C |
| 비동기 | `WorkerScheduler`(DAG·취소), `AsyncReadFile/AsyncWriteFile`(IOCP, `Flush`) | 로더가 사용(11절) |
| 파일 | `FileSystem`, `CreateNew`, no-replace Move, `Path`, Win64는 `FILE_SHARE_DELETE` 열기 | → A0-P, VFS(APK) → 후속 |
| 해시·텍스트 | CRC32, `NameKey`, UTF-8 검증·`CodePointRange` | SHA-256·Unicode·JSON → A0-C |
| 프로세스 | 없음 | 임포트 worker process는 17절. PlatformBase `Process`·`LocalChannel`은 Rockpile R0(별도 작업 — A3는 쓰지 않는다) |
| 그래픽 | `PixelFormat` 전체(PF1–PF3), 차원 5종·view swizzle·샘플러 W축·필터 5종·비교 샘플러(A0-G 구현) | 바인딩 샘플러 → GX6 |
| 도구 | `CommandletRegistry`, `Programs/Commandlet` | `asset-*` → A6 |
| 기존 에셋 코드(미커밋) | AssetRuntime(단일 `.rasset`), AssetTools(BMP·TGA·PSD·PNG 디코더 — PNG는 zlib 대기, 설정·mip 빌드) | 대체. 디코더 → ImageCodec, mip → TextureTools |
| 외부 라이브러리 | Core/ThirdParty(zlib·LZ4·Zstd), Core `Compression` 모듈 | PNG zlib·bulk Zstd에 사용. 이미지 디코더는 자체 구현 |
