# 요청: INTERFACES 10.1에 `scenegen::terrainHeight` 한 줄 추가 (RPP 트랙, 2026-09-26)

## 무엇
C가 SceneGen 공개 API에 지형 높이 질의를 넣었다(4a322b2):

```cpp
float scenegen::terrainHeight(SceneId id, float x, float z);  // 그 장면 지형 메시가 표본으로 쓰는 해석 함수; 범위 밖은 NaN
```

장면별: city_block·city_night = cityTerrain(도시 정사각형 안 0), forest_* = rollingTerrain, waterside = lakeFloor, interior = 바깥 지면 −0.2, ridge_sunset = ridgeTerrain. seed·scale과 무관. 시험 `unx_test_scenegen`이 모든 장면에서 지형 메시 정점이 1 mm 안임을 확인한다(C).

## 왜
RPP-1 세계 조립(`Tools/RppBuild`)이 구간 지형을 이을 때 쓴다. 전에는 RPP가 C 내부 수식을 복사해 두고 정점 대조로 검사했다. 이제 복사를 지웠다(RPP a42dff6). 세계 장면은 가장자리 법선만 바뀌었다(C 질의가 영역 밖에서 NaN이라 영역 안으로 자름); 경로·강체 파일은 비트 동일 [실측].

## 원하는 변경
INTERFACES 10.1 문단 끝에 위 시그니처와 "범위 밖 NaN" 한 줄. 다른 트랙 영향 없음.
