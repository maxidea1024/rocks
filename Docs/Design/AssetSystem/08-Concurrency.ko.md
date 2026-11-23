## 8. 동시성

- 쓰기 잠금: `Intermediate/AssetDatabase.lock`을 `FileLock`(Windows `LockFileEx` + 삭제 공유 없는 열기, POSIX `flock` + 잠근 뒤 dev/ino 재확인)으로 잡은 프로세스만 에셋·별칭 표·인덱스 캐시·쿠킹 출력을 바꾼다. 잠금을 잡는 명령은 13절 목록이며 서비스 API도 같은 규칙이다. 쿠킹도 이 잠금을 쥐므로(에디터 패키지가 쿠킹 중 바뀌지 않게) 같은 프로젝트의 쿠킹은 프로파일과 무관하게 직렬화되고 쿠킹 출력에 따로 잠금을 두지 않는다. 못 잡으면 `Busy`(`--wait=<초>`).
- 에셋 너겟(`NuggetFileSystemBackend`)은 키 주소 저장소라 잠금 없이 동시 기록한다(9절 — 같은 키의 동시 게시는 no-replace 이동이 하나만 남기고 나머지는 `Exists`·`Conflict`다). Rockpile backend는 서버가 저장소 잠금을 쥔다(Rockpile 6.8절).
