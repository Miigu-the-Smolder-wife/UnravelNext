# 요청: 광선 hit 셰이딩을 직접 뷰와 같은 함수로 (R 트랙, 2026-09-25)

## 왜

R의 반사·GI 광선이 맞힌 표면은 반사된 상(像)으로 화면에 나온다. 사용자 방향은 "평면 거울과 잔잔한 물은 반사 카메라 래스터가 직접 뷰와
정확히 같으므로 허용, 래스터와 광선의 선택은 비용식으로"다. 비용식으로 고르려면 두 경로의 품질이 같아야 한다.
그런데 R의 hit 셰이딩은 확산만(`방출 + 알베도/π × (태양 + 캐시 조도)`) 계산했다. 그래서 반사 속 금속이 검고, 태양 하이라이트와 광택 반사가 빠졌다.

R은 2026-09-25에 hit 셰이딩을 모델 v1 전체로 바꿨다(`Native/Render/RayTracing/HitShading.hlsli`):
- 태양: 확산 + M의 태양 원반 스펙큘러 3영역(`shSunSpecular`: 점 평가 / 원반 구적 / 좁은 로브의 원반 안 비율)
- 간접: 확산 = 캐시 조도, 스펙큘러 = `shSpecularAlbedo` × 캐시의 거울 방향 입사 복사

직접 뷰와 똑같이 하려고 **M의 순수 함수(`Passes/Shading/ShadingCommon.hlsli`)를 include해서 쓴다.**
(A, B) 표는 M의 `ShadingSystem.cpp` 안에서만 만들어져 R이 링크할 수 없으므로, 같은 표본으로 만든 **복사본**을 R이 갖고 있다(`RayTracing/SpecularAlbedo.cpp`).
둘 다 소유 경계를 넘는 임시 상태다. 아래를 인터페이스로 고정해 달라.

## 원하는 변경

1. **(A, B) 스펙큘러 방향 알베도 표를 `scene::model`로** (코어): M의 `buildSpecularTable()`을 `scene::model::specularAlbedoTable()`
   (32×32 float2, 모델 E 표와 같은 가시 법선 표본, A + B = E)로 옮긴다. 코어가 한 번 올리고 `FrameConstants`에 SRV
   `g_specularAlbedoLut`를 둔다(`g_materialModelLut`와 같은 방식). M과 R은 각자의 복사본을 지운다.
2. **태양·스펙큘러 순수 함수를 공용 헤더로** (M 소유): `shSpecular`, `shSunSpecularQuadrature`, `shSunLobeFraction`, `shSunSpecular`,
   `shSpecularAB`, `shSpecularAlbedo`를 `Passes/Common/SurfaceLighting.hlsli`(가칭)로 옮긴다. LUT 인자는 `g_specularAlbedoLut`로 대신한다.
   R의 `HitShading.hlsli`가 그 헤더를 include한다. 이후 M이 이 함수들을 바꾸면 광선 hit도 같이 바뀐다(반사 속 표면 = 직접 뷰의 표면).
3. **`giCacheRadiance`에 표면 법선 인자** (R 소유 함수, M이 소비): v1의 `giCacheRadiance(GiSrvs, worldPos, dir, coneHalfAngle)`는 항목을
   **dir의 법선 부류**로 찾는다. 그래서 다른 면의 항목을 읽거나 항목을 못 찾는다(R의 결함). R은 법선을 받는 오버로드를 추가했다(가산 변경, 기존 호출은 그대로 컴파일됨).
   새 시그니처는 `giCacheRadiance(GiSrvs, worldPos, normal, dir, coneHalfAngle)`이고, 8셀 삼선형 + 레벨 오르기다.
   M의 `ShadeOpaque.hlsl`(평면 반사 뷰 분기)이 `nv`를 넘기도록 바꾸면, R은 v1 시그니처를 지운다.
4. **다음 단계(M 텍스처·국소광이 생길 때)**: 표면 한 점을 조명 입력(태양 가시성, 조도, 스펙큘러 입사, 국소광 목록)으로 셰이딩하는 M의 함수를 공용으로 둔다.
   그러면 R의 hit가 그대로 부른다. 그래야 반사 광선과 평면 카메라의 품질이 같아지고, 비용식만으로 고를 수 있다.

## 영향

- 코어: `scene::model`에 표 함수, `FrameConstants`에 SRV 하나, INTERFACES 8.1 표기.
- M: 함수 이동(동작 불변), `ShadeOpaque.hlsl`의 `giCacheRadiance` 호출에 법선 추가.
- R: 복사본 삭제, include 경로 변경.

## 실측 근거 (R, 2026-09-25)

- `unx_test_reflection_reflectionanalytic`(백색로, 벽 f0 0.04): hit 스펙큘러를 넣은 모델 기대값 대비 M 평균 −0.29 %, G −0.15 %, 전 픽셀이 3 % + 4σ 안이다. 하늘 거울은 최악 0.00 %다.
- 비용: 도시 4K 반사 trace 0.724 → 0.857 ms(조도·복사 조회를 한 번의 셀 순회로 합친 뒤).
