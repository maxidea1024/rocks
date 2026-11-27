## 14. 단계

ID는 이 문서가 부여한다(UX). 고정 구현 순서를 바꾸지 않으며 착수는 사용자가 정한다. 사용자 지시(2026-11-21 "진행 가능한거부터")에 따라 UX1의 Rocks 리더 부분을 UX1a로 떼어 먼저 한다. UE 플러그인은 소스 빌드 없이 Launcher 설치판을 C:에 받아 빌드한다(사용자 결정 2026-11-20). 사용자가 UE 5.8.4 설치판을 받았으므로(2026-11-22) UX1의 UE 쪽을 UX1b로, Rocks 임포트 연결을 UX1c로 나눈다. UX2 이후 단계도 UE 추출 쪽은 Rocks 리더 왕복으로 검증해 Rocks 임포트(에셋 시스템 A2f–A5·MA b 단계)보다 먼저 할 수 있다. 그래서 텍스처·정적 메시는 추출(UX2a·UX3a, UE)과 임포트(UX2b·UX3b, Rocks)로 나눈다(개정 10, 사용자 지시 "진행 가능한거부터"). 스켈레톤·스킨 메시도 같이 UX7a·UX7b로, 애니메이션 클립도 UX8a·UX8b로 나눈다(개정 12·13).

| 단계 | 쪽 | 내용 | 선행 |
|---|---|---|---|
| UX1a | Rocks | 묶음 형식 v1 리더·검증(JSON 스키마·blob SHA-256·크기), 경로 대응·이름 보정, 위상 정렬(SCC), 손으로 만든 묶음 fixture와 변조 시험 — 에셋 DB에 쓰지 않는 부분. **구현**(2026-11-21, 10.4절) | MA1(원본 메시 형태 확인용), 엔진 플러그인 빌드 지원(BuildTargets 3.6절) |
| UX1b | UE | 플러그인 골격(11절 — Editor 모듈, commandlet `-run=RocksExport`, 메뉴, 묶음 writer, 자체 SHA-256, 숫자 서식, UPROPERTY 덤프, 닫힘, redirector)과 에셋 JSON 공통부. 설치판 엔진에 대고 `RunUAT BuildPlugin`으로 빌드하고, 엔진 콘텐츠를 내보낸 묶음을 UX1a 리더로 읽어 왕복 검증한다(에셋 종류별 본문은 UX2 이후). **구현**(2026-11-22, 11.7절) | UX1a, UE 5.8 Launcher 설치판(C:) |
| UX1c | Rocks | 임포트 계획과 명령(10.5절): `--only` 닫힘, 기존 에셋 조회 `ExchangeIndexLookup`(태그 `UnrealExchange.ObjectPath`·`FormerPaths`), 재임포트 대상 경로, 종류별 임포터 표와 실행 순서·실패 전파, Commandlet 명령 `asset-import-unreal`(`--dry-run` 포함) 등록. 실제 에셋 쓰기(종류별 임포터)는 UX2b 이후이고 그 전제 프레임워크 보강은 UXR8–UXR11 | UX1b, A2f |
| UX2a | UE | 텍스처 추출(4절 — 원본 mip 사슬 blob, 형식·감마·손실 원본 표시, 설정 덤프). Rocks 리더 왕복(묶음·blob 검사)과 UE 자기 시험(blob과 `FTextureSource` 원본 대조)으로 검증한다. **구현**(2026-11-22, 4.5절) | UX1b |
| UX2b1 | Rocks | 종류별 임포트 공통 틀(10.6절 — 에셋 문서 byte 읽기·blob 검사·계획의 묶음 밖 대상 id·옵션 struct·설정 번역 틀)과 텍스처 순수 변환(4.6절 2·3번 — 원본 형식 대응, 설정 표), 새 진단 `UnsupportedSourceLayout`·`GammaMismatch`. **구현(2026-11-26)**: 묶음 리더 공용화(동작 불변), `ExternalTarget`, 플러그인 첫 리플렉션 타입(옵션 struct·`UnrealExchange.rocksids.json`), 설정 번역 틀·텍스처 표 73행, 원본 변환, 엔진 콘텐츠 설정 fixture(`Tests/Settings/EngineTextureSettings.json` — `AssetImportData` 본문을 뺀 사본), 시험 21개, 리뷰 반영(실패 때 view 비우기, 감마 번호를 UE `EGammaSpace` 순서로, Guid는 Core 서식, PerPlatform 형식 판정) | UX2a, UX1c, A5, UXR8·UXR9 |
| UX2b2 | Rocks | 텍스처 임포트(4.6절 — A2f 임포터 다섯, 종류별 임포터, 감마 대조, 제품 Commandlet 연결, 서비스·명령·C# 시험). **구현(2026-11-26)**: 선행 A2f5 태그 접두(AssetSystem 7.1절 21 — 교환 임포터들이 `UnrealExchange` 접두), `UnrealTextureImporter`(+Set)·`ExchangeTextureKind`, 명령의 상대 묶음 경로를 절대 경로로(공용 `AbsoluteCommandPath`), 제품 Commandlet 연결(호스트가 UnrealExchange 모듈을 시작해 옵션 타입 등록), Basic fixture 텍스처 둘을 4.5절 스키마로, 시험(명령 흐름·이름 바꾸기 세 번·차원 클래스·비ASCII·묶음 밖 참조·blob 교체·감마, A2f 임포터 거부·경고, C#), 리뷰 반영 — UX2b 완료 | UX2b1 |
| UX3a | UE | 정적 메시 추출(5절 — `source`·`prepared`·생성 LOD·충돌, 5.2절 `MeshSource` 기록). Rocks 리더 왕복과 MA1 `MeshSource` 적재·검증으로 확인한다. **구현**(2026-11-22, 5.4절) | UX1b |
| UX3b1 | Rocks | 정적 메시 순수 변환(5.5절 1–9번 — 기록 적재 `LoadExchangeMeshSource`·모드·LOD와 에셋 설정·슬롯·섹션·소켓·충돌 → `StaticMeshImportSpec`), 설정 번역 행의 `rocksKey`, 경고 `UnrealNameNormalized` | UX3a, UX2b, MeshAnimationAssets MA2b–MA3 |
| UX3b2 | Rocks | 정적 메시 임포트(5.5절 10–12번 — A2f 임포터 `unreal_static_mesh`, 종류별 임포터 일반화 `ExchangeServiceKind`, 옵션 `meshNormals`, 제품 연결, 명령·C# 시험) | UX3b1, UXR11(complex collision 메시 순환은 계획이 `HardCycle`로 막는다) |
| UX4a | UE | 머티리얼·인스턴스·함수·컬렉션 추출(8.1·8.2절 — 그래프·출력 핀·인스턴스 `resolved`·설정 덤프, Substrate 그래프도 그대로 덤프). Rocks 리더 왕복과 그래프 구조 불변식으로 확인한다. **구현**(2026-11-22, 8.6절) | UX1b |
| UX4b1 | Rocks | 머티리얼 그래프 번역(8.7절 1–10번 — 카탈로그 기준 노드·핀·속성, 출력 `SurfaceFromShadingModel`, 설정, 파라미터 병합, 함수·컬렉션, 대체 규칙 3번). **명세**(2026-11-27, 8.7절) | UX4a, UX2b, MaterialSystem MS3·MS6, UXR8–UXR10, UXR12–UXR14·UXR16 |
| UX4b2 | Rocks | 머티리얼 인스턴스(8.7절 11·12번 — override 키, trait override, 결과 따름, `resolved` 대조는 MS4 뒤) | UX4b1, MaterialSystem MS6(대조는 MS4) |
| UX5 | 둘 다 | Substrate 그래프 번역(8.3절) | UX4b1 |
| UX6 | 둘 다 | 굽기 대체(8.4절)와 "구운 표면" 내장 머티리얼, 대체 규칙 2번 | UX4b2, UXR15 |
| UX7a | UE | 스켈레톤·스킨 메시 추출(6절 — 본 계층·커브, 스킨 가중치·morph, 생성 LOD 복원). Rocks 리더 왕복과 MA4a 스켈레톤 검증·병합 판정, MA5a 스킨 빌드로 확인한다. **구현**(2026-11-22, 6.3절) | UX3a |
| UX7a2 | UE | 스킨 메시 머티리얼 슬롯 `data.materials`(6.3절 13번 — transient `Materials`), 자기 시험, golden 다시 내보내기 | UX7a |
| UX7b1 | Rocks | 스켈레톤·스킨 메시 순수 변환(6.4절 1–7번 — `ExchangeSkeleton`·`ExchangeSkeletalMesh`, 설정 행 `EnumMap`, 공용 `ExchangeMeshCommon`) | UX7a2, UX3b, MA4b–MA5 |
| UX7b2 | Rocks | 스켈레톤·스킨 메시 임포트(6.4절 8–10번 — A2f `unreal_skeleton`·`unreal_skeletal_mesh`, 종류별 임포터·제품 연결, 명령·C# 시험) | UX7b1, UXR8–UXR10 |
| UX8a | UE | 애니메이션 클립 추출(7절 — 본 트랙·커브·이벤트·동기 마커). Rocks 리더 왕복과 MA6a 원본 검증·샘플링·쿠킹 v1 변환으로 확인한다. **구현**(2026-11-22, 7.1절) | UX7a |
| UX8b | Rocks | 애니메이션 클립 임포트(7.2절 — `ExchangeAnimationClip`, 커브 열거·플래그 대응, 이벤트 속성 버림 경고, A2f `unreal_anim_sequence`) | UX8a, UX7b, MA6, UXR8–UXR10 |
