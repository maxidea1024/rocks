## 16. 임포트·빌드 의존

### 16.1 재임포트

기존 옵션 유지·명시 값만 변경, 잘못된 옵션 `InvalidImportOption`, 임포터 버전이 기록보다 높으면 `ImporterOutdated` 표시(자동 실행 없음), 사라진 출력은 `OrphanedOutput` 표시·`asset-orphans-*`로 처리.

### 16.2 Build 의존 선언

`DependencyDeclarer`(AssetRuntime 인터페이스, 타입별 등록)가 에디터 객체에서 Build 의존(예: Texture2DArray의 원본 Texture2D)을 선언한다. AssetDatabase는 저장 때 이 인터페이스로 목록을 얻어 Dependencies에 기록하고(쿠커를 부르지 않는다 — 의존 방향 유지), AssetCooker는 같은 선언을 너겟 키·선행 조건에 쓴다.
