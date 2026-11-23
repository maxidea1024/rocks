# 엔진 런타임 — 11. 에셋

[목차](README.ko.md)

## 11. 에셋

- `AssetId`는 `Guid`다. 에셋 파일(`.rasset`)은 `magic "RAST" · 형식 버전 · 에셋 종류 · AssetId · 페이로드`를 Serialization `ArchiveWriter`로 기록한다.
- `TextureAsset`: 폭·높이·형식(RGBA8 sRGB)·픽셀. 행 순서는 위→아래.
- `AssetManager`: 루트 디렉터리를 마운트해 에셋 파일 헤더를 읽고 `AssetId ↔ 경로` 목록을 만든다. `Load(AssetId)`는 참조 계수 핸들을 반환한다. 마지막 핸들이 사라지면 언로드한다. 이번 단계의 로드는 동기이며 비동기 로딩(`AsyncReadFile`, 로드 상태 Pending/Ready/Failed)은 후속이다.
- `BitmapImporter`(AssetTools): 비압축 BMP(24·32비트, `BI_RGB`·`BI_BITFIELDS`, 상향·하향 행)를 RGBA8로 바꾼다. 압축 BMP·팔레트는 `Unsupported`다. Commandlet `import-texture <src.bmp> <dst.rasset>`로 실행한다.
- GPU 텍스처는 `WorldIntegration`의 텍스처 캐시가 AssetId별로 만든다. 에셋 데이터와 GPU 자원의 수명은 분리한다.
