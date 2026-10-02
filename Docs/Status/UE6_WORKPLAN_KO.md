# UE6 구조 렌더러 — 남은 작업과 조사 결과 (2026-10-02)

사용자 지시(2026-10-02): 안개를 잘 만들 것. 얇은 지오메트리 비용, 숲 규모, high 티어, 캐릭터 음영, 원거리 GI, 구름. 배치가 도는 동안에도 코드를 쓸 것. 최적화할 때 표현·함수·알고리즘·수치해석·시스템 구조·하드웨어 매핑·수학/물리 모델을 점검할 것.
측정 결과는 `UE6_PORT_STATUS_KO.md` 7절. 이 문서는 "무엇이 왜 남았고 어디를 고치는가"만 적는다.

## 0. 작업 방식

- 배치는 **고정된 스냅샷**(커밋을 체크아웃한 별도 워크트리 `C:\Users\USER\UnravelNext-ue6-run`, 자체 빌드)에서 돌린다. 게이트는 설정을 `UNX_SOURCE_DIR/Config/quality`에서, 셰이더를 실행 파일 옆에서 읽으므로 개발 워크트리의 편집·빌드가 도는 배치를 바꾸지 못한다.
- 개발 워크트리(`UnravelNext-ue6`)에서는 배치 중에도 코드를 쓰고 낮은 우선순위로 빌드한다.
- 조사(읽기 전용)는 에이전트에 맡기고 결론만 받는다.

## 1. 안개 (진행 중)

첫 구현(공기 볼륨의 매질)의 한계 [실측/그림]: 하늘·먼 땅이 회색으로 덮임, 국소광 많은 실내에서 깊이 슬라이스 띠(타일 24 px × 64 슬라이스가 65 km까지: 30 m 안에 슬라이스 약 22개).
새 구조(원본의 VolumetricFog + ExponentialHeightFog):

- **근거리 볼륨**(0 ~ `volumetric_distance_m`, 기본 80 m): 자체 격자(셀 16 px, 96 슬라이스, 슬라이스 = log2(z·k + 1)·b, k = 32 / far), 셀마다 밀도와 산란광(태양 × VSM 그림자 구간, 국소광 = MegaLights 볼륨의 fluence·moment를 삼선형으로, 간접광 = 이전 프레임 반투명 볼륨), 프레임 지터 + 히스토리 0.9, 열마다 앞에서 뒤로 적분.
- **원거리**: 닫힌 식 지수 높이 안개 × 열별 원거리 소스(그림자 없는 태양 + 간접광).
- **하늘**: 기본 1 — 하늘 픽셀도 안개를 통과한다(원본의 안개 패스는 하늘도 덮는다: `bFogOnlyOnRenderedOpaque`는 장면 캡처 전용). 0이던 때 안개 낀 능선이 맑은 지평선 앞에 서 있었다 [그림: FogV2 ridge_sunset]. `sky_amount`로 조절.
- 적용: 불투명 음영 뒤, 화면 추적용 장면색 보관 뒤, 반투명 합성 앞의 전체 화면 패스.
- 그림자 페이지: 안개 격자의 셀 중심 광선 구간을 표시하는 패스(`s.vsm.markfog`).
- 남는 것: 반투명·입자에 안개 적용, 보조 뷰, 씬 데이터로 파라미터 전달, 구름 그림자.

## 2. 얇은 지오메트리 (조사 완료)

- 빌더가 그룹마다 단순화 오차를 `lod_max_relative_width_error(0.25) × 그룹의 가장 좁은 폭`으로 묶는다(`Tools/ClusterBuilder/src/ClusterBuilder.cpp:183-195`). 넘으면 그 그룹은 종단(:219). 나무 클러스터 3,820개 중 LOD가 있는 것은 3개, 풀은 0개(`Results/V/BandC/README_KO.md:18`).
- 먼 얇은 지오메트리를 맡기로 한 대역 C 브릭은 꺼져 있다(`brick_max_feature_width = 0`).
- preserve-area 없음. 테스트 `thin_geometry_is_not_thinned`(`Tests/ClusterBuilderTests.cpp:531-553`)가 "면적 2 % 이내 유지"를 요구.
- 쿡 캐시 키에 이 값이 들어 있어(`ClusterBuilder.cpp:624`) 바꾸면 모든 메시가 다시 빌드된다.
- coverage 래스터: 프래그먼트당 약 0.3 ns + 삼각형당 1.13 ns, 호숫가 4K에서 1,200만 프래그먼트(`COVERAGE_REDESIGN_KO.md:209`). 합성은 기록당 21~24 ns였던 것을 타일×깊이 구간 조명으로 줄이는 설계가 있다(`RENDERER_REDESIGN_V2_KO.md` 14.1c, 스위치 `shading.coverage_tile_lights`).
- 대역별 통계는 `unx::visibility::Stats`에 있으나 RendererGate가 찍지 않는다(`RendererGate.cpp:1386`).
- **할 일**: (a) 빌더: 얇은 그룹도 단순화하되 섬(연결 성분) 단위로 남은 삼각형을 키워 면적을 보존(Nanite의 Preserve Area). 단순화된 클러스터의 폭을 다시 계산. (b) 게이트에 대역별 통계 출력. (c) 그 뒤에도 남는 대역 B 비용은 설계 문서의 깊이 묶음 래스터·프리미티브 HiZ.
- 숲 씬의 그림자 상한 폭발(레벨당 4e8)도 나무에 LOD가 없어서다: (a)가 되면 굵은 레벨의 절단이 수천 → 수 개로 준다.

## 3. 숲 규모

- (2)의 LOD가 먼저. 그 뒤 남는 것: 그림자 레벨의 텍셀보다 작은 인스턴스를 V의 인스턴스 컬에서 제외(뷰 레코드에 최소 반지름), 요청 수 줄이기(빈 요청 1개가 약 0.2 ms).

## 4. 캐릭터 음영 (조사 완료)

| | 현황 | 근거 |
|---|---|---|
| 피부 | 모델 없음: Subsurface 클래스가 일반 불투명 커널로 그려짐 | `ShadeOpaque.hlsl:23`, `ShadingSystem.cpp:781` |
| 머리카락 | 가닥 시뮬레이션·래스터·섬유 BSDF(R/TT/TRT+꼬리)는 있으나 음영에 연결 안 됨: 기록의 복사휘도 0 | `HairBsdf.hlsli:109`(포함처는 테스트뿐), `CoverageSpecial.hlsl:39`, `visibility.coverage_hair = false` |
| 눈 | 없음 | — |
| 천 | Charlie sheen 층 있음(클리어코트와 배타) | `MaterialModel.hlsli:183-246` |
| 클리어코트 | 있음 | `MaterialModel.hlsli:264-342` |

- 재질 레코드에 subsurface·눈·이중 스펙큘러 필드가 없다(`Scene.hlsli:71-102`, `SceneData.h:63-128`). MegaLights와 광선 hit은 Subsurface를 일반으로 취급.
- 원본: `ShadingModels.ush` `SubsurfaceProfileBxDF`:592(이중 GGX + Burley 확산 + 투과), `PostProcessSubsurface.usf`(분리형/버얼리 화면 공간 SSS), `TransmissionCommon.ush`, `HairBsdf.ush`(+ `EvaluateHairMultipleScattering`:45, 이중 산란), `EyeBxDF`:959, `ClothBxDF`:674.
- **할 일 순서**: 재질 필드(산란 색·평균 자유 경로·이중 로브) → 피부 BRDF(이중 스펙큘러 + 투과) → 화면 공간 SSS 패스 → 머리카락 기록 음영 연결(+ 다중 산란 근사, 그림자) → 눈 → 천의 fuzz 혼합.

## 5. 구름 (조사 완료)

- 있음: 구형 껍질 한 층의 볼륨 레이마치(Perlin-Worley), 이중 HG, 2옥타브 다중 산란 근사, 1/4 해상도, 태양 방향 깊은 불투명도 맵(±16 km, 불투명 그림자와 GI hit이 읽음).
- 없음: 시간 재구성·지터·양방향 업샘플(원본 `VolumetricRenderTarget.usf:262`), 불투명 지오메트리 앞의 구름(하늘 픽셀에만 합성), 공기·안개·반투명에 드리우는 구름 그림자, 콘텐츠 연결(설정 키 없음, 게이트 `--clouds`뿐, scenegen 미사용).
- 비용 [문서의 실측]: 720p에서 march 3.27 ms(320×180) — 시간 재구성 없이는 비싸다.
- **할 일**: 프레임 분할 추적 + 재투영 재구성, 설정 키와 scenegen 씬, 안개·공기에 구름 그림자.

## 6. 원거리 GI (조사 완료)

- radiance cache 프로브 광선은 200 m에서 끝나고 그 너머는 하늘: 200 m 밖 지형이 가리지도 반사하지도 않는다. 카드는 300 m까지. TLAS는 전체 인스턴스(컬링·HLOD·원거리 TLAS 없음).
- 원본: 원거리 TLAS 층(HLOD), `r.LumenScene.FarField.MaxTraceDistance` 1e6 cm, 근/원 디더(`LumenHardwareRayTracingCommon.ush:1200-1221`).
- **할 일**: 캐시 프로브 광선을 원거리까지(TLAS가 이미 전체를 담고 있으므로 거리만 늘리고, 원거리 hit은 카드 없이 직접광 + 하늘 근사), 비용 측정 후 원거리 전용 마스크.

## 7. high 티어

- 첫 정의(반사 반해상도 + TSR flicker 끔): 로비 4K 12.11 → 11.59 ms. 원본 High 묶음(프로브 32 px, radiance cache 프로브 16², MegaLights 표본 2)을 더함 — 측정 대기.

## 8. 4K 목표와 최적화 점검 목록

4K(내부 1080p) 게임 씬 11~12 ms. 큰 항목: GI 2.3, MegaLights 1.5, TSR 1.2, 반사 0.9~1.6, 프록셀 0.7, coverage 0.7, 그림자 0.65, 음영 0.6, resolve 0.5.
점검 축(사용자 지시): 표현(버퍼 형식·패킹), 함수(근사식), 알고리즘(표본 수·재사용), 수치해석(적분·필터), 시스템 구조(패스 수·그래프·비동기 큐), 하드웨어 매핑(웨이브·캐시·대역폭), 수학/물리 모델.
