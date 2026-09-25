# 요청: RPP(RPP-1 장면 제작) 트랙과 `Tools/RppBuild`·`Content/RPP1` 등록 (RPP 트랙, 2026-09-26)

## 왜 필요한가

REBUILD_PLAN 14.3 ⑬(사용자 결정 2026-09-26)으로 RPP-1 장면 제작 트랙이 시작됐다. P7 판정 장면(RPP-1.0)을 지금부터 병렬로 만든다.
소유(쓰기): `Content/RPP1/`, `Tools/RppBuild/`, `Docs/Status/RPP_STATUS_KO.md`, `Results/RPP/`, 이전 저장소 `Assets/RPP1/`.

`Tools/RppBuild`는 manifest(`Content/RPP1/rpp1_manifest.json`)에서 RPP-1 세계(`.unxscene`), 구간별 강체 파일(C 생성기 출력),
카메라 트랙·이벤트 스크립트(json), 기준 대기열용 스냅샷을 만드는 CPU 도구다. GPU를 쓰지 않는다.

**지금 막혀 있는 것**: `cmake/Modules.cmake`의 `unx_add_tool_folders`는 `Tools/<이름>/CMakeLists.txt`가 있는 폴더마다
`unx_folder_enabled`를 부르고, `cmake/Tracks.cmake`에 `UNX_TRACK_OF_RppBuild`가 없으면 `FATAL_ERROR`다. 그래서 RPP가
`Tools/RppBuild/CMakeLists.txt`를 먼저 만들면 **모든 세션의 구성이 실패한다.** 등록 전에는 CMakeLists 없이 소스만 쓴다.

## 원하는 변경

1. **`cmake/Tracks.cmake`**: `set(UNX_ALL_TRACKS V M S R C I FX RPP)`, `set(UNX_TRACK_OF_RppBuild RPP)`.
2. **`Tools/CI/Build.ps1`**: `-Track RPP` → `build/RPP`, 기본 `UNX_TRACKS` = `C;RPP`.
   RppBuild는 `unx_scenegen`(C: 구간 장면·강체 배치 `dynamicContent`)과 `unx_scene`만 링크한다. 렌더러 모듈은 필요 없다.
3. **INTERFACES 1절 표**: `RPP RPP-1 장면 | RPP 세션 (v1.40) | Content/RPP1/, Tools/RppBuild/ (이전 저장소 쪽 Assets/RPP1/)` 행과
   2.5절 트랙 목록에 RPP.
4. `Content/`는 빌드 대상이 아니다(데이터: manifest, 생성 규칙, 작은 json). 큰 생성물(`.unxscene`)은 `Cache/RPP1/`에 쓰고 커밋하지 않는다
   (`Cache/`는 이미 무시 목록에 있다). 커밋하는 것은 manifest, 생성 파일의 sha256, 카메라·이벤트 json(수백 KB 이하)이다.

## 영향

- 다른 트랙의 빌드·파일은 바뀌지 않는다. `-Track all`에는 RPP가 들어가지만 CPU 도구 하나다.
- C의 공개 API(`SceneGen.h`)는 읽기만 한다. 변경이 필요하면 따로 요청한다(지형 높이 질의, 아래 참고).

## 관련(별도 요청 예정, C와 직접 조율 중)

RPP-1 세계는 한 2×2 km 구역에 네 구간(C의 forest_combat 틀에 city_night·waterside·interior를 앵커 변환으로 배치)을 합친다.
구간 지형을 이으려면 C의 `rollingTerrain`·`cityTerrain`·`lakeFloor` 값이 필요하다. 지금은 `src/Common.h`(C 내부)에 있어서
RPP가 수식을 복사하고 C 지형 메시 정점과 1 mm 안에서 일치하는지 매번 검사한다(어긋나면 빌드 실패). C가 공개 질의
(`float terrainHeight(SceneId, float x, float z)`)를 내주면 복사를 지운다.
