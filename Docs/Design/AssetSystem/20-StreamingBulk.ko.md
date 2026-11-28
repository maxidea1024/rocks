## 20. 스트리밍 bulk(sidecar)와 범위 읽기 — PR12(2026-11-28, 설계 교차 검증 반영 개정 1, PR12a 구현 리뷰 반영 개정 2)

오디오 AU6b(AudioSystem 3.12·3.40절)는 3분 곡의 첫 음을 내기 전에 플랫폼 데이터 전체를 읽지 않아야 한다. 그러려면 쿠킹된 압축 청크가 패키지 밖에 있고, 재생 중에 필요한 범위만 비동기로 읽을 수 있어야 한다. 지금 런타임은 쿠킹 패키지를 통째로 읽고 `Prepare`가 모든 bulk를 풀어 `Package`가 갖는다(`PackageBulkMode::Decode` — 5절 10번·11절). 이 절이 그 기반(PR12 — AudioSystem 06-Phases 표의 선행)을 정한다.

기준은 UE 5.8이다:
- 별도 파일 payload `BULKDATA_PayloadInSeparateFile`(5.7부터 옛 철자 `…Seperate…`는 deprecated 별칭 — Runtime/CoreUObject/Public/Serialization/BulkData.h 122–123)와 쿠킹 `.ubulk`(IoStore에서는 package 청크와 따로인 `EIoChunkType::BulkData`).
- 범위 읽기 `FBulkData::CreateStreamingRequest(offset, bytes, priority, callback, userBuffer)` → `IBulkDataIORequest`(BulkData.h 84–95·905–923 — callback은 아무 스레드에서 늘 불리고, 요청은 늘 취소·대기한 뒤 지운다. 구현 Private/Serialization/BulkDataStreaming.cpp 83–86은 소멸에서 `DoneEvent.Wait()`). UE 5.8 오디오 캐시는 `FBulkDataBatchRequest::NewBatch(1)`·`Read`·`Issue`를 쓰고(AudioStreamingCache.cpp 2053–2056), 대기·삭제를 막지 않으려고 백그라운드 task로 넘긴다(`FClearAudioChunkCacheReadRequestTask` — 1825–1853), 굶으면 우선순위를 올린다(`UpdatePriority(AIOP_MAX)` — 2132–2139).
- 오디오 청크: 청크 0은 늘 인라인(`ForceInlinePayload`), 나머지는 `Force_NOT_InlinePayload`(SoundWave.cpp 882·893–900). 청크마다 bulk 하나다(`FStreamedAudioChunk` — SoundWave.h).
- 무결성: UE는 `.ubulk`를 로드 때 해시하지 않지만, 서명된 IoStore 컨테이너는 읽을 때 블록마다 해시를 대조한다(PakFile/Private/IoDispatcherFileBackend.cpp 1723–1740·2230–2235).

개정 1(설계 교차 검증 높음 6·중간 11·낮음 4 반영): 너겟 서술 정정(H1), `bulkRawBytes`·Prepare·예산에서 sidecar 제외(H2), sidecar 블록 해시 표로 서명 사슬 유지(H3), callback 스레드 규칙과 막지 않는 해제(H4), 손잡이는 lease가 아니라 계수된 sidecar 파일을 쥔다·IO 경계·종료 순서(H5), 오디오 배치를 3.30절 위치 불변 규칙에 맞춤(H6), 형식·경로·머리·로더 IO·우선순위·취소·API·모듈·명령 정정(M1–M11), UE 인용·서버·준비 티켓 이관·주석(L1–L4), 단계 재배치.

### 20.1 결정

1. **배치**(UE 별도 파일 payload): bulk 하나는 패키지 안(Bulk 섹션 — 지금) 또는 sidecar에 있다. sidecar는 **쿠킹 패키지에만** 있다(에디터 패키지는 지금처럼 Defer 저장소로 지연 읽기 — 7.1절 15). 에디터 저장에 sidecar 배치가 있으면 InvalidArgument, 에디터 패키지에 `Sidecar` flag·섹션이 있으면 `CorruptPackage`다. 쿠킹 처리기가 bulk마다 배치를 고른다(기본 패키지 안). 서버 프로필은 sidecar를 만들지 않는다(오디오 서버 payload 제외는 BS7 — AudioSystem 3.39.5), sidecar 없는 쿠킹 패키지는 정상이다.
2. **오디오 배치**(AudioSystem 3.30.1 위치 불변 규칙): sidecar entry = payload 영역 byte 그대로(청크 0 포함 — 위치 기준이 나누기 전과 같다), 패키지 bulk = 메타 + 선택으로 payload 앞부분 [0, 청크 0 끝)(청크 0이 두 곳에 있다 — UE 청크 0 인라인과 같은 효과, byte를 다시 쓰지 않는다). 그래서 `SoundClip`은 bulk ref가 둘이 된다(메타·인라인 청크 + 스트리밍 payload — 오디오 세션 몫, 알림). UE처럼 청크마다 bulk를 두지 않고 payload 하나를 범위로 읽는 까닭: 청크 표·SHA가 이미 오디오 플랫폼 데이터 안에 있고(3.30절), 청크마다 BulkIndex 항목을 두면 패키지 표가 수천 항목이 된다.
3. **sidecar 파일**: 패키지 하나에 sidecar 하나다. 머리 64 byte(magic `RBSC`·formatVersion 1·bulkAlignment·entryCount·fileBytes·패키지 대표 AssetId·crc32 — 머리 무결성) 뒤에 entry 본문을 bulkAlignment(패키지와 같은 값) 정렬로 BulkIndex 순서대로 잇는다. entry 사이 정렬 여백은 0이고 머리 읽기와 블록 해시가 덮는다(아래 5). 범위 읽기를 하므로 sidecar entry의 codec은 None만이다(Zstd 블록 컨테이너는 entry 전체를 풀어야 한다 — 5.7절 6). 경로는 내용 주소다: `Packages/<a>/<A>-<sidecar SHA-256 앞 16 hex>.rbsc`(`AppendCookedSidecarPath` — 패키지 경로 4절 17과 같은 디렉터리 규칙, 셰이더 artifact와 같은 내용 주소). 다시 쿠킹하면 새 경로다. 쿠킹 정리(10.2절 — 매니페스트가 가리키는 파일만 남긴다)와 패키징 복사(10.3절)는 `.rbsc`를 함께 다룬다. 실행 중 프로세스가 이미 연 이전 sidecar는 지워져도 계속 읽힌다(Windows `AsyncReadFile`은 `FILE_SHARE_DELETE`로 연다 — PlatformBase Win64 AsyncFile.cpp 108, POSIX unlink도 같다). 그래서 AudioSystem 3.11.5의 "기존 revision은 참조가 끝날 때까지 둔다"는 열린 파일 의미로 충족되고, 열지 않은 이전 revision은 정리된다(그 문장을 고치도록 알린다).
4. **패키지 포맷**(5절 — `formatVersion` 올림, 옛 판은 `UnsupportedVersion`): 
   - BulkIndex 항목 flag `Sidecar`(bit 1). Sidecar 항목의 offset은 sidecar 파일 안 위치(머리 뒤 bulkAlignment 정렬), storedBytes = rawBytes(codec None), crc32·sha256은 지금 뜻 그대로다. offset 증가·정렬 규칙은 두 공간(패키지 Bulk 섹션·sidecar)마다 따로 적용한다.
   - 새 섹션 `Sidecar(10)`(Required — Sidecar 항목이 있으면 필수, 없으면 생략): `{ fileBytes u64, sha256 (32), hashBlockBytes u32, hashCount u32, blockSha256 × hashCount }`. 블록 해시는 sidecar 파일 전체를 hashBlockBytes(64 KiB — UE IoStore 압축 블록과 같은 크기) 단위로 나눈 SHA-256이다(마지막 블록은 짧다). 패키지 SHA(매니페스트)가 이 섹션을 덮으므로 서명 사슬(ContentSigning — 매니페스트 서명이 패키지 byte를 모두 묶는다)이 sidecar byte까지 이어진다.
   - 섹션 존재: BulkIndex는 Bulk 또는 Sidecar 섹션이 있을 때만 있다(둘 다 없는 BulkIndex는 `CorruptPackage` — 부분 읽기도 표 단계에서 거부), 에디터 패키지는 Sidecar 섹션이 없고 Bulk ⇔ BulkIndex다. rawBytes 0인 bulk는 Sidecar를 골라도 패키지 안에 둔다(읽을 byte가 없다 — sidecar fileBytes > 머리 규칙을 정렬과 무관하게 지킨다, 구현 리뷰 반영).
   - 모든 bulk가 Sidecar면 Bulk 섹션은 없고 BulkIndex만 있다(5절 53행 "Bulk와 함께 생략" 규칙을 고친다). `PackageKnownSectionKinds`는 10이 된다.
   - `bulkRawBytes`(10.1절 28)는 **패키지 안 entry의 rawBytes 합**으로 뜻을 바꾼다. sidecar 크기는 매니페스트 `sidecarBytes`로 따로 센다.
5. **매니페스트 v3**(10.3절 4의 v2를 대체 — v2 reader는 두지 않는다): 항목에 `sidecarBytes u64 | sidecarSha256 (32)`를 더한다. 0 byte면 SHA도 0이어야 한다(아니면 `CorruptPackage`). 진단 칸 `SidecarBytes`·`SidecarSha256`. 패키지 `Sidecar` 섹션의 fileBytes·sha256이 매니페스트와 다르면 `CorruptPackage`다(`CheckCookedPackageContents`).
6. **쿠킹**: bulk 배치는 `PackageBulkSource::Placement(ref)`(새 가상 함수 — 기본 Package, writer가 저장 전 id로 묻는다)로 정한다. writer는 패키지와 sidecar를 같은 staging에 쓰고(sidecar를 먼저, 블록 해시를 계산해 패키지 `Sidecar` 섹션에 넣는다), 실행기가 staging의 sidecar 파일에서 크기·SHA-256을 직접 재 매니페스트에 넣는다(셰이더 artifact와 같은 방식 — 10.3절). 게시는 sidecar → 패키지 → 매니페스트 순이다. 부분 쿠킹 재사용(10.2절 8)은 패키지 대조에 sidecar 파일 크기·SHA-256 대조를 더한다(sidecar만 지워져도 다시 쿠킹). **배치는 너겟 키·값에 들어가지 않는다**: 너겟 값은 처리기의 가공 결과(오디오는 플랫폼 데이터 byte — AudioSystem 3.37절)이고 패키지가 아니며, 배치는 패키지를 쓸 때 정한다(위치·SHA가 나누기 전후로 같다).
7. **런타임 로드**: `Prepare`는 sidecar를 읽지 않고, `DecodePackageBulk`·Prepare SHA 확인·bulk 할당·로더 예산(11절 52·63 — 파일 크기 + bulkRawBytes)·`EstimatePrimaryAsset`(11절 107)에서 Sidecar entry를 뺀다(`EstimatePrimaryAsset`은 sidecarBytes 합을 따로 낸다). sidecar 파일 크기 대조는 로더 IO 경계 안에서 한다: `CookedPackageRead`에 sidecar 경로·기대 크기를 더하고 IO 구현이 IO 스레드에서 Stat한다(다르거나 없으면 `CorruptPackage` — Game 스레드 동기 IO 없음, 시험은 지금처럼 gate 구현을 끼운다). `Package`의 bulk 조회는 Sidecar 항목에 `Streamed`를 알린다(`BulkBytes` → `Unsupported` — 메모리에 없다).
8. **범위 읽기**: 20.2 3. 읽은 byte는 덮는 블록 해시로 늘 검증한다(블록 단위로 읽어 대조한 뒤 요청 범위를 복사 — 서명 사슬). 그래서 소비자가 자기 해시를 따로 볼 필요가 없다(오디오 `VerifyAudioChunk`는 남겨도 되고 빼도 된다 — 오디오 세션 결정).
9. **준비 티켓은 오디오 몫**: "frame N부터 낼 수 있다"(AudioSystem 3.12.4 `PreparePlayback`)는 청크 캐시·디코드를 아는 오디오가 범위 읽기 위에 만든다. 에셋 쪽은 읽기 요청 하나의 상태만 준다. AudioSystem 06-Phases의 PR12 행("준비 티켓·sidecar 범위 읽기")과 3.12.4를 고치도록 알린다(준비 티켓은 AU6b).
10. **텍스처는 지금 그대로**: 스트리밍 mip을 sidecar로 옮기는 것은 렌더 스트리밍 단계가 같은 API로 할 수 있게 열어 두고, 지금 텍스처 쿠킹은 패키지 안이다(12.14절 A5j3).

### 20.2 API

1. **AssetRuntime 형식**(`PackageFormat.h`·`PackageReader.h`·`PackageWriter.h`·`CookedManifest.h`):
   - `BulkEntryFlags::Sidecar`, `PackageSection::Sidecar`, `PackageSidecarInfo{ fileBytes, sha256, hashBlockBytes, blockHashes }`.
   - writer: `PackageBulkSource::Placement(ref)`로 Sidecar 항목을 고르고, sidecar 출력(staging 파일)을 쓰기 옵션으로 받는다(쿠킹만).
   - reader: Sidecar 항목은 Bulk 섹션 범위 대신 `Sidecar` 섹션 fileBytes 안인지·정렬·증가를 본다. `Sidecar` 섹션 크기 = 48 + 32 × hashCount, hashCount = ceil(fileBytes / hashBlockBytes), hashBlockBytes는 2의 거듭제곱(4 KiB–1 MiB).
   - sidecar 머리: `ReadSidecarHeader`(magic·판·정렬·entryCount·fileBytes·대표 AssetId·crc32 — 다르면 `CorruptData`). 런타임은 첫 열기 때 머리 64 byte를 비동기로 읽어 본다(첫 Read 앞에).
   - 매니페스트 v3 reader·검증·쓰기(판 상수 3 — `CookedManifest.h`).
2. **AssetCooker**: `CookContext`에 sidecar sink(staging 파일 — 실행기가 패키지 sink와 함께 준다), 처리기는 `PackageBulkSource::Placement`를 구현하는 쿠킹 bulk 원본(`CookChainedBulkSource` — CookBulkSource.h 36)에 배치를 표시한다(`Add(raw, CookBulkPlacement::Sidecar)`). 실행기가 staging sidecar의 크기·SHA를 매니페스트에 넣는다.
3. **런타임 손잡이**(AssetRuntime — `AssetManagerConfig`에 범위 IO 인터페이스를 더한다, 기본 구현은 EngineAssets의 `AsyncIoContext`(EngineAssetsSubsystem `io_`)를 쓴다):
   ```cpp
   // lease가 가리키는 에셋 패키지의 스트리밍 bulk를 연다(Game 스레드 — lease는 그대로 호출자에게 있다).
   // 손잡이는 패키지가 아니라 계수된 sidecar 파일(경로·크기·블록 해시 표·열린 AsyncReadFile)을 쥔다.
   Result<StreamedBulk> AssetManager::OpenStreamedBulk(const AssetLease& lease, const BulkDataRef& ref) noexcept;
   class StreamedBulk {  // move-only. Read·소멸은 아무 스레드(AudioRT 제외 — 할당이 있다)
       uint64 Bytes() const noexcept;
       Result<BulkReadHandle> Read(uint64 offset, Span<uint8> destination, BulkReadPriority priority,
                                   BulkReadCallback callback, void* context) noexcept;
   };
   using BulkReadCallback = void (*)(void* context, const BulkReadResult& result) noexcept;  // result: error(None·
                                                                                            // CorruptData·IoError·Cancelled)
   class BulkReadHandle {  // move-only
       BulkReadState State() const noexcept;  // Pending | Done | Failed | Cancelled
       void Cancel() noexcept;                // 요청만 한다 — callback은 Cancelled 또는 원래 결과로 정확히 한 번
       void SetPriority(BulkReadPriority) noexcept;  // 아직 제출하지 않았으면 큐 순서를 바꾼다(UE 굶음 승격)
       // Pending인 손잡이의 소멸은 Fatal이다(막는 소멸을 두지 않는다 — 막지 않는 해제는 Cancel 뒤 callback을
       // 받고 놓는다, UE WaitForAsyncLoadCompletion(bCancel)·백그라운드 정리와 같은 모양).
   };
   ```
   - **callback 스레드**: 블록 해시 대조와 사용자 callback은 AssetManager의 worker(로더 Prepare와 같은 WorkerScheduler)에서 돈다(IO 완료 스레드에서 돌리지 않는다 — Native backend는 완료 스레드 하나를 로더와 같이 쓴다, AsyncFile.h 97–98). callback은 짧게 돌고 다음 일은 넘긴다. callback 안의 Wait·Shutdown은 금지, 자기 손잡이 파괴는 허용한다(그때는 이미 Done이다). destination은 **callback이 돌아올 때까지** 살아 있어야 한다. Cancelled면 destination 내용은 정하지 않는다.
   - **제출 큐**: 손잡이 공용 큐가 우선순위 순서로 제출하고 동시 진행 상한(`StreamedBulkMaxInFlight`)을 둔다. 그래서 `Cancel`·`SetPriority`가 아직 제출하지 않은 요청에 실제 효과가 있다. 이미 제출한 요청의 취소는 최선 노력이다(AsyncIO 취소 — 경합하면 원래 결과).
   - **검사**: 범위가 rawBytes를 넘거나 한 요청이 `AsyncReadFile` 상한(4 GiB − 1) 이상이면 InvalidArgument, 패키지 안 bulk면 `Unsupported`, Shutdown 뒤면 `ShuttingDown`, 할당 실패 OutOfMemory다 — 즉시 실패는 callback을 부르지 않는다. 짧은 읽기·블록 해시 불일치는 `CorruptData`, IO 오류는 그 오류다.
   - **파일 수명**: sidecar 경로마다 계수된 `SidecarFile`을 연다(첫 손잡이가 비동기로 열고 머리·크기(`AsyncReadFile::Size`)를 다시 본다 — Stat과 열기 사이 교체 검출). 마지막 손잡이와 마지막 진행 요청이 끝나면 닫는다(잠금 + 계수). `BulkReadHandle`은 `StreamedBulk`보다 오래 살 수 있다(요청이 파일을 계수한다).
   - **lease와의 관계**: 손잡이는 열 때만 lease를 본다. scope Revoke·lease 반납·패키지 내림 뒤에도 손잡이는 계속 읽는다(필요한 값을 이미 복사했다).
   - **종료**: AssetManager Shutdown은 새 Open·Read를 `ShuttingDown`으로 거부하고 진행 요청을 취소한다. AssetManager 파괴 때 손잡이·진행 요청이 남으면 `platform::Fatal`이다(lease 규칙과 같다 — 11.7절 6). 그래서 손잡이를 쥐는 Subsystem(`AudioEngineSubsystem`)은 `EngineConfig::subsystems`에서 `EngineAssetsSubsystem` 뒤에 등록해(역순 정리) 자기 정리에서 손잡이를 놓아야 한다(오디오 세션 알림 — 3.08절 70의 정리 순서와 같다).
4. **오디오 쪽 사용**(AU6b — 오디오 세션 구현): 청크 캐시가 clip마다 `StreamedBulk`를 쥐고 청크 하나를 `Read` 하나로 읽는다. seek는 새 재생이라 그 청크 offset을 읽는다(AudioSystem 3.40절). 굶으면 `SetPriority`로 올린다. stop·회수는 `Cancel` 뒤 callback에서 놓는다. AudioRT는 손잡이를 쥐지 않는다.
5. **검증 명령**: `asset-load-cooked`에 `--verify-sidecars`를 더한다(모든 sidecar 전체 SHA-256·블록 해시·머리 대조 — 13절 문법에 더한다). 따로 `asset-verify` 명령은 두지 않는다.

### 20.3 단계

| 단계 | 내용 | 모듈 |
|---|---|---|
| PR12a | 패키지 포맷 판 올림(BulkIndex `Sidecar` flag·`Sidecar` 섹션·블록 해시), sidecar 파일 형식·writer·reader·머리, `bulkRawBytes` 뜻 변경·Prepare·`DecodePackageBulk`·예산에서 Sidecar 제외, 매니페스트 v3 reader·검증·`CheckCookedPackageContents`, `CookedPackageRead`의 sidecar Stat, `CookedPackageIo.h` 옛 주석 정정(짧은 읽기 CorruptData), 시험 | AssetRuntime |
| PR12b | 쿠킹: `Placement`·쿠킹 bulk 원본 배치, `CookContext` sidecar sink, staging·게시 순서, 매니페스트 sidecar 칸 쓰기·병합, 부분 쿠킹 재사용, 정리·패키징 `.rbsc`, `asset-load-cooked --verify-sidecars`, 시험 | AssetCooker·AssetCommandlets |
| PR12c | 런타임 손잡이: 범위 IO 인터페이스·기본 구현, `OpenStreamedBulk`·`StreamedBulk`·`BulkReadHandle`(제출 큐·우선순위·취소·블록 해시·worker callback·파일 계수), 종료 계약, 시험 | AssetRuntime·EngineAssets |

**진행**: PR12a 구현 완료(2026-11-28 — 대조표 14.1.121). 구현 리뷰에서 블록 수 올림 넘침, 섹션 존재 규칙(BulkIndex ⇔ Bulk 또는 Sidecar), 0 byte Sidecar 배치(패키지 안), 쿠커 bulkRawBytes 합의 Sidecar 제외, `ExpectedSidecarHeader` 입력 계약, Sandbox TestContent 에디터 패키지 20개의 판 올림을 반영했다. 쿠커는 아직 Sidecar를 고르지 않는다(PR12b). 다음은 PR12b다.

단계마다 리뷰어 하나다. PR12a 판 올림 뒤 Sandbox TestContent 재쿠킹·14.1 golden 재생성을 같이 한다. PR12c가 끝나면 오디오 세션에 알린다(AU6b 착수 가능 — 배치·두 번째 bulk ref·Subsystem 순서·3.11.5·3.12.4·06-Phases 수정). 렌더 세션에는 텍스처 스트리밍이 같은 API를 쓸 수 있음을 알린다.

### 20.4 시험(완료 조건 — 과하게)

- 형식: Sidecar 항목만(Bulk 섹션 없음)·섞임·없음, 두 공간의 정렬·증가(4096·64), 머리 손상(magic·판·정렬·crc·entryCount·fileBytes·AssetId), `Sidecar` 섹션 크기·hashBlockBytes·hashCount 규칙, Sidecar 항목이 fileBytes 밖, codec Zstd Sidecar 거부, 에디터 저장 Sidecar 요청 거부·에디터 패키지의 Sidecar 섹션 `CorruptPackage`, 옛 판 `UnsupportedVersion`, golden byte(패키지 + sidecar)·두 번 쓰면 같은 byte, 고정 seed 변이 fuzz의 Sidecar flag·섹션 확장.
- 로드: 로더가 sidecar byte를 하나도 읽지 않음(IO byte 계수), Prepare 할당 최댓값 < sidecar 크기, bulkRawBytes가 sidecar를 빼고 셈, `EstimatePrimaryAsset` sidecarBytes, sidecar 크기 불일치·없음 `CorruptPackage`(gate IO).
- 매니페스트 v3: 판정 순서 표, sidecarBytes 0인데 SHA 0 아님, 패키지 `Sidecar` 섹션과 매니페스트 불일치 `CorruptPackage`, v2 `UnsupportedVersion`.
- 쿠킹: 게시 순서(중간 실패에서 매니페스트가 새 sidecar를 가리키지 않음), 재사용(sidecar만 바뀌거나 지워지면 다시 쿠킹), 배치가 너겟 키·값을 바꾸지 않음, 내용 주소 경로, 전체 쿠킹이 낡은 `.rbsc`만 지움·부분 쿠킹은 보존, `--verify-sidecars`, 서버 프로필 sidecar 없음.
- 런타임: 범위 경계(0·끝·넘침·4 GiB), 짧은 읽기 `CorruptData`, 같은 크기로 byte만 바뀐 sidecar의 블록 해시 검출, IO 실패(AsyncIoFaultPoint), 취소 제출 전·뒤, 우선순위 순서, 완료 재정렬(`HoldCompletions` gate — 시간 대기 금지), callback 정확히 한 번·worker 스레드, callback 안 자기 손잡이 파괴, Pending 손잡이 소멸 Fatal(death test), 마지막 손잡이 뒤 파일 닫힘, scope Revoke·lease 반납 뒤에도 읽기, Shutdown 뒤 Open·Read `ShuttingDown`, 파괴 때 남은 손잡이 Fatal, 할당 실패 주입(전 지점), 여러 스레드 동시 Read.
