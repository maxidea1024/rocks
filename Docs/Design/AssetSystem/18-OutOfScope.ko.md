## 18. 범위 밖과 후속

메시·스켈레톤·스킨 메시·애니메이션 클립 에셋 타입은 [MeshAnimationAssets](../MeshAnimationAssets/README.ko.md)가, UE 콘텐츠를 가져오는 교환 경로는 [Unreal 교환 형식](../UnrealExchange/README.ko.md)이 설계했다(2026-11-20). 두 스펙의 단계(MA·UX)는 이 문서의 A1–A6 뒤이며 착수는 사용자가 정한다.

후속(이번 범위에 API도 없음): 에디터 UI, GPU 업로드·텍스처 mip 스트리밍(계획 §9.5), equirect Cube, Cube 면 경계 연속 mip, 임포트 worker process, Virtual Asset, VFS, 예산 LRU 해제, 실제 다중 출력 임포터(FBX·glTF), UE의 다른 텍스처 계열(렌더 타깃 에셋·IES `TextureLightProfile`·`RuntimeVirtualTexture`·미디어·동적 텍스처), 일반 에셋 사용자 데이터(`AssetUserData`), 쿠킹 결과 패키징(배포 위치 배치).

계약(API·스키마는 있고 기본값이 아니거나 호출하면 명시 오류)으로 이번에 두는 것은 3·11·12절 표에 있다: `NuggetRockpileBackend`(설정 `backend: "rockpile"` → Unsupported, Rockpile R1a), Record `Codec` encoding(늘 미적중), 너겟 byte 범위 부분 읽기(Rockpile 8.6절), `NuggetPolicy`의 원격 비트(원격 동작 없음), 블록 압축, 12.4절 계약 설정(가상 텍스처·스트리밍·보정·색 관리·압축 설정 포함), `ReadyMask::Render`, `TaskLane::Worker`·`Render`.
