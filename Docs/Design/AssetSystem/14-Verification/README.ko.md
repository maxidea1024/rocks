## 14. 검증 시나리오

1. PNG 임포트·저장 → 새 프로세스에서 같은 AssetId·픽셀
2. 원본 PNG 삭제 후 에디터 패키지만으로 쿠킹
3. 원본 변경 재임포트: ID·참조 유지, 해시 갱신, 에셋 너겟 무효화
4. 잘못된 PNG·원본 누락·잘못된 옵션 재임포트 실패 시 기존 패키지 바이트 불변
5. 이동·이름 변경(디렉터리 포함) 후 ID 참조 유지, 별칭 해석·경고·만료
6. 복제본 새 ID·내부 참조 재매핑
7. 중복 ID·깨진 참조·TypeMismatch·UnsupportedCycle 진단, 하드 순환 SCC 로드
8. 인덱스 캐시 삭제 후 재구축, 에디터 밖 이동·삭제 후 재검색, 감시 Overflow·RootLost 복구
9. 에셋 너겟 적중·설정 변경 무효화·툴체인 지문 변경 무효화·세대 변경 무효화·손상 복구·두 프로세스 동시 기록·GC(A3는 `NuggetFileSystemBackend` 경로 — Rockpile 경로는 Rockpile R1a가 같은 시나리오를 `NuggetRockpileBackend`로 다시 통과한다)
10. mip 크기·대표 픽셀(12.5절 표: sRGB·알파·홀수·Unfiltered·PreserveBorder·FlipGreen·NoAlpha(비압축 결과 불변)·Normalize·Volume·Cube)
11. 쿠킹 패키지만으로 로드, EditorOnly 데이터·필드 부재
12. 잘못된 패키지(magic·CRC·섹션 순서·겹침·범위·크기)·UnsupportedVersion·NewerVersion
13. 쿠킹 재현성(에셋 너겟 빈/찬, 두 번 쿠킹 동일 바이트, Debug·Release 도구 사이 동일 바이트 — Rockpile 경로는 R1b), 부분 쿠킹 병합·전체 쿠킹 정리, 실패 시 게시본 불변
14. 저장 실패 주입 시 기존 보존, 열린 대상 교체, 잠금 Busy, ExternalModification
15. 경로 정책: NFC/NFD 같은 이름, 대소문자 중복, 예약 이름, 길이, CaseCollision
16. 소프트 참조 비자동 로드, 요청 합류·개별 취소, OnReady 중복 AlreadyRegistered, scope Revoke·ScopeClosed, 의존 retention, lease 해제 후 회수
17. PlatformBase A0-P: DirectoryWatcher 8건·파일 내구성 8건 Windows 통과(깊은 작업 디렉터리 포함), Android 빌드, macOS 미검증. SyncDirectory 시험(디렉터리·끝 구분자·현재 디렉터리·디렉터리 link 성공, 파일·끝 구분자 붙인 파일·파일 link WrongType, 없는 경로·파일을 지나는 경로 NotFound, 잘못된 경로 InvalidArgument, wrapper 기본 Unsupported) Windows 통과
18. PNG 적합성: PngSuite 대조
19. UnsupportedSetting(계약 설정 값), NeverCookReferenced, LossySource 거부
20. 다중 출력: 시험 전용 임포터(원본 하나 → 출력 둘)로 출력 대응 유지·사라진 출력 OrphanedOutput 표시·명시 삭제
21. Cube·2DArray·Volume 임포트·쿠킹·로드(원본 구성별)
22. 매니페스트 게시: 교체 전후 reader가 일관된 집합만 보는지, 쿠킹 실패 주입 시 이전 매니페스트 유지, 부분 쿠킹 `IncompatibleCook`
23. 수명: Closing 직후 같은 ID 재요청이 GC 전에 새 로드로 성공(Discard), Closing 대기 중 재요청 revive, 하드 순환 SCC 단위의 해제, 로드 중 의존 단위 보유, 동적 하드 간선의 계수 반영, lease 해제 지연
24. 12.4절 표: 모든 계약 설정에 기본값이 아닌 값 → `UnsupportedSetting`, 숨김 CompressionSettings → `InvalidSetting`, 선형 설정·float 출력 + SRGB on → `SrgbIgnored`, CompressionSettings 15종 × 원본 형식의 쿠킹 형식·swizzle, sRGB 출력 규칙, LOD 그룹 합성·필터 조합
25. 결정적 수학 기준값·CRT 초월 함수 미사용 링크 검사, 서로 다른 호스트·빌드 구성의 쿠킹 바이트 비교
26. 원본 규칙: BMP 32비트 BI_RGB 알파 무시·V5 알파 마스크, TGA 알파 비트·확장 영역(premultiplied), PSD 색 모드·hasRealMergedData·추가 채널, PNG 회색 16비트 + tRNS
27. 매니페스트 대조: 프로파일 불일치 `IncompatibleCook`, 크기·SHA·의존 불일치 `CorruptPackage`, 손상된 게시 파일의 재게시
28. 순환 키: 하드 순환 A↔B·3-node 순환의 cookKey와 Build 순환의 너겟 키가 정의되고, 구성원 하나를 바꾸면 SCC 전체의 키가 바뀐다. 동적 간선으로 생긴 순환의 단위 병합과 해제, 간선 다중도(필드 둘 중 하나 제거)
29. 저장: 이전 파일의 손상 bulk entry 복사 거부, 저장소 id 표 다시 잇기 후 지연 읽기, 저장 중 편집 시 재연결 생략, 대체 경로(FAT 등)에서 자기 핸들 때문에 실패하지 않음
