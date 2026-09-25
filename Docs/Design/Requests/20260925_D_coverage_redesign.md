# 요청: 설계 개정 1(coverage 층·투과율 층·대역 C·밴드 스케줄링)의 인터페이스 변경 (설계 개정 세션, 2026-09-25)

근거: `Docs/Design/COVERAGE_REDESIGN_KO.md`(절 번호는 그 문서), `ARCHITECTURE_KO.md` [개정 1] 표시. 코어가 INTERFACES에 반영하고 12절에 남긴다. 구현은 각 소유 트랙이 한다. 트랙 사이의 순서 의존은 마지막 절에 있다.

## 1. INTERFACES 7.1 — coverage 층 (V 생산, S·M 소비)

- `coverageFragments`: 레코드 **24 B** `{ uint visId; float depth; uint mask32; uint area16_flags16; uint normalOct32; uint attr32; }`. flags: bit 0 불투명(alpha 1), bit 1 평판(양면). attr32 = 보간 uv(2 × unorm16, 텍스처 재질) 또는 절차 색 RGB8 + 예비(풀·가닥). 옛 16 B 배치와 "픽셀별 깊이 순 정렬"은 폐기한다(정렬은 M 합성 커널이 타일 groupshared에서).
- `coverageChunks`(raw): 8×8 타일마다 64-fragment 청크의 연결 목록(청크 머리 4 B: 다음 청크 | 개수). 풀 용량은 직전 프레임 필요량 × 1.5(최소 4 M fragment), 넘치면 `visibility::Stats::overflow`와 게이트 실패.
- `coverageTileHeader`(raw, 타일당): 첫 청크, 픽셀 64개의 `{ float zMin; float zMax; }`(fragment append 때 원자 min/max), `opaqueCovered` 64비트(front fragment area = 1 & 불투명, 또는 집합체 T < 1/256).
- `bDepth`: R32_FLOAT, 대역 B 앞면 깊이(원자 max, reversed-Z; 면적 1·불투명 fragment만 갱신). 대역 B PS는 `depth`(대역 A)를 읽기 전용 depth로 묶어 early-Z, 그 뒤 bDepth로 다시 거른다.
- `coverageAggregate`: 32 B × P_C `{ half T; float depthRep; half sggx[6]; uint albedoRgb8_material8; half entryDepth[3]; ushort pad; }`. 7.1의 "브릭 fragment는 삼각형 필드 0x7F"는 폐기.
- V 공개 HLSL(5.6): `CoverageFragment coverageLoadFragment(uint index)`, `uint2 coverageTileChunk(uint2 tile)`, `AggregateRecord aggregateRecord(uint2 pixel)`(레코드 없으면 T = 1).
- 5.5.1(V ↔ M): "픽셀별 깊이 순 정렬된 목록" → "타일 청크 목록 + 헤더; 정렬·합성은 M". `Coverage.hlsli` 공유 함수는 그대로.

## 2. INTERFACES 7.3 — fragment·집합체 가시성 (S 생산, M 소비)

- `shadowFragmentVisibility`: 12 B × 픽셀(P_cov만 유효; 헤더의 zMin/zMax가 있는 픽셀) `{ uint sunT4; uint localMin; uint localMax; }` — sunT4 = 구간 위 K_vis ≤ 4점의 태양 투과율 × 불투명 분류(unorm8 × 4, 점 위치는 z_min + (z_max − z_min) × k/(K−1)), localMin/localMax = 슬롯 1~3의 국소광 가시성을 z_min·z_max에서(7.3 부호화).
- `shadowFragmentPairs`(raw): 불투명 캐스터의 penumbra가 구간을 지나는 픽셀의 (fragment 인덱스, unorm8 결과) 목록. M은 fragment 인덱스로 찾는다(드묾; 타일당 정렬된 소목록).
- 집합체: 진입점 ≤ 3의 태양 투과율은 `coverageAggregate`의 entryDepth 위치에서 S가 계산해 `shadowFragmentVisibility.sunT4`의 하위 3바이트에 쓴다(P_C 픽셀).
- 품질 키 `shadow.fragment_exact`(검증·비용 분해 전용, 기본 0): 1이면 fragment마다 직접 조회한 값을 `shadowFragmentPairs`에 전부 쓴다(3.3의 게이트 기준).

## 3. INTERFACES 5.6 — S 투과율 층 조회

- `float shadowSunTransmittanceAt(ShadowSrvs s, float3 worldPos, float footprint, float reach)`: 단 k = footprint에 맞는 단, 밉 = log2(reach / 텍셀)의 블록 프로파일에서 1탭(2×2 gather, 높이 보간). 층이 없는 페이지 = 1. 페이지 없음 = 1(resident 인자는 `shadowSunVisibilityAt`과 같은 규칙).
- `shadowSunVisibilityAt`의 반환값에 투과율 층을 곱한다(V = V_opaque × T). R의 광선 hit·M의 fallback이 자동으로 얇은 캐스터 그림자를 받는다.
- 4.7: 프록셀 공기 그림자는 S 내부(`FroxelIntegrate`)라 인터페이스 변경 없음.
- 품질 키 `shadow.transmittance_knots = 4`(8: 정밀 페이지 변형).

## 4. INTERFACES 5.3 — 깊이 래스터 서비스의 coverage 모드 (V 구현, S 소비)

- `DepthRasterRequest::coverage = true`: 보존 래스터 + 삼각형∩텍셀 정확 면적 + 32-부표본 마스크 + (알파 재질) cutout coverage를 계산해 요청자 픽셀 커널에 `DepthRasterPixel`과 함께 `float area; uint mask;`를 넘긴다(`DepthRasterCoverage` 구조체, V 소유 헤더). 요청자(S)는 페이지별 fragment 목록에 append한다. 대역 판정은 요청 뷰의 텍셀 크기 `RasterView::lodPixelsPerMetre`로 한다(광공간 폭 0.25~1.5텍셀이 coverage, ≥ 1.5는 depth 래스터, < 0.25는 요청자의 브릭 march).
- 직교 브릭 march용으로 V의 브릭 옥트리 순회 함수를 S가 include한다(5.6 V 행에 `brickMarchOrtho(...)`; 시그니처는 V가 정한다).

## 5. INTERFACES 8.1 — 재질 클래스 `Aggregate` (M)

- SGGX 마이크로플레이크 확산 + 스펙큘러(Heitz 2015 §5), 입력 = `coverageAggregate`의 S·알베도·재질(거칠기·f0·transmission). 기준 경로추적기(C)는 대역 C를 쓰지 않으므로(원본 기하) 이 클래스는 실시간 전용이며, 정확성 기준은 개정 1 6.6(대역 C 없는 coverage 층 렌더 + 기준 영상)이다.
- 합성 규칙(5.5.1에 추가): 집합체 층의 coverage = 1 − T, 마스크 없음(픽셀 균일). 앞→뒤 합성에서 집합체 뒤의 층은 T를 곱하고, fragment 뒤의 집합체는 fragment 마스크 여집합 비율을 곱한다.

## 6. INTERFACES 6.5 — 클러스터 메타데이터 (V)

- 밀도(삼각형 투영 면적 / 경계 투영 면적; 픽셀당 fragment 예측 → `band_c_max_fragments_per_pixel` 판정), 둘레/면적 비(평판 클러스터의 대역 B 상한 8 px 적용 여부), 브릭 옥트리 참조(메시 단위), 베이크 잔차 P99(`visibility.brick_residual_max` 검사).
- 품질 키(9절, V): `visibility.band_b_max_width_px_sheet = 8`, `visibility.brick_residual_max`, `visibility.brick_voxel_bytes = 8`.

## 7. INTERFACES 4 — render graph 밴드 패스 그룹 (코어)

- `RenderGraph::addBandedGroup(uint bands, std::span<PassDecl>)`: 그룹 안 패스들을 밴드마다 차례로(패스 A(밴드 0) → B(밴드 0) → … → A(밴드 1) …) 기록한다. 각 패스의 execute는 `PassContext::band`(y0, y1)를 받고 타일 목록·디스패치를 그 범위로 자른다(타일 목록은 밴드별 구간으로 만든다: M `ShadeBegin`·`EdgeArgs`·S `ShadowListArgs`가 밴드 구간 인자를 낸다). 배리어는 밴드마다 같은 자원에 대해 낸다(패스당 0.85 µs × 8밴드 × 5패스 = +0.03 ms).
- 4K 8밴드, 1440p 4밴드(품질 키 `output.band_count`, 코어).
- 조건: 그룹 안 패스는 밴드 밖 픽셀을 쓰지 않는다. 3×3 이웃(가장자리 검출·E 합성)은 경계 1행을 읽기만 한다(이전 밴드는 이미 끝났고 다음 밴드의 값은 아직 없으므로, 아래 경계 1행은 **다음 밴드가 끝난 뒤** 처리하는 지연 목록으로 둔다: M).

## 8. R·S — 광선 hit의 태양 가시성을 컴퓨트로 (개정 1 5.5-1)

- `FrameResources::rayHits`(raw, R 생산): hit 레코드 16 B `{ float3 pos; uint normalOct16_footprintHalf; }` × (GI 0.5 M + 반사 ≤ 1.2 M), 개수 + 디스패치 인자.
- S 진입점 `shadowHitVisibility(fc)`(5.2 순서: `reflections` 뒤, `shadowVisibility` 앞): hit마다 `shadowSunVisibilityAt` × 투과율 층 → `FrameResources::rayHitVisibility`(unorm8 × hit). 비용식: 흩어진 receiver 0.093 ns [실측 `vsm` scattered] + 트래픽.
- R: raygen은 순회만(closest-hit 식별 기록), hit 셰이딩은 컴퓨트 패스(`r.gi.shadehits`, `r.refl.shadehits`)로 옮기고 태양 항은 `rayHitVisibility`를 읽는다. R 실측(raygen 안 VSM 0.87 ns > 그림자 광선 0.51)이 이 분리의 근거다. 채택 판정은 R·S가 통합 게이트에서 hit당 실측으로(개정 1 7.3).

## 9. 코어 `Tools/Microbench` — [예상] 단가를 [실측]으로 (개정 1 7.3)

1. `--only-coverage2`: 24 B fragment 타일 버킷 append(0.25~1 px 조각 + 4 px 카드) → 타일 청크 → groupshared (픽셀, 깊이) 정렬 → 상수 조명 합성. 출력: fragment당 ns(append / 합성), 픽셀당 ns, F 5·10·20 M.
2. `--only-bricks`: 15 MB 희소 브릭(16³, 복셀 1 B와 8 B 두 변형)을 4K 픽셀 광선이 DDA(스텝 16·32·48): 픽셀당 ns, L2 적중률(스텝당 바이트); 직교 뷰 변형(태양 march) + 진입 맵 생성.
3. `--only-bands`: 해석(12 B 쓰기) → 셰이딩(37 B 읽기·4 B 쓰기) 2패스를 전체 화면 대 8밴드로: 4K·1440p, 픽셀당 ns.
4. `--only-shade2`: split 커널에 타일 groupshared 프로브 SH(9 × 70 B) + 공기 3D fetch 3회 + K 아틀라스 탭 + 국소광 8개를 넣은 변형: 점유율(웨이브/SM)별 ms — 4.4의 점유율 조건 수치.

## 10. 순서 의존

1. 코어: 7.1·7.3·5.6·8.1 표기(이 요청), 밴드 그룹(7절) — 마이크로벤치 3 결과 뒤에 확정해도 된다(그 전에는 그룹을 1밴드로 실행).
2. V: 24 B 레코드·타일 청크·헤더·bDepth(1절) → M 합성 커널(4.5) → S fragment 가시성(2절). 브릭 옥트리(6절)·coverage 모드 서비스(4절)는 그 뒤.
3. S: 투과율 층(3절)은 V의 coverage 모드가 있어야 채워진다; 그 전에는 층 없음(= 1)으로 동작하고 조회 함수는 먼저 만든다(R·M이 곧바로 쓸 수 있게).
4. R·S: 8절은 R이 hit 목록 경로를 만든 뒤.

## 11. 개정 1 9~10절 뒤의 추가 (2026-09-25 15:55; 마이크로벤치 실측으로 확정한 품질 불변 변경 a~f)

| | 변경 | 인터페이스 |
|---|---|---|
| a | `coverageFragments` 레코드 **16 B** `{ uint visId; float depth; uint mask32; uint normalOct16_area16; }` (1절의 24 B를 대체). 절차 색은 `GpuInstance` 예비 칸의 인스턴스 색 RGB8(I·코어), 재질 상수는 visId → 클러스터 → 재질 | 7.1, 6.3(GpuInstance 색), 5.5.1 |
| b | `Passes/Visibility/Coverage.hlsli`에 `uint coverageTriangleMaskLut(float2 a, float2 b, float2 c, float2 pixel, StructuredBuffer<uint> lut)` — 변마다 (각 64 × 거리 64) 마스크 표 AND; 표는 V가 만들어 `FrameConstants` 예비 칸의 SRV로 게시. `coverageTriangleMask`(32 판정)는 검증용으로 남긴다 | 5.5.1, 5.5 |
| c | `coverageAggregate` 레코드 **16 B** `{ half T; half pad; float depthRep; half entry[3]; ushort brickMaterial; }` (6절 32 B를 대체); SGGX 모멘트는 브릭 머리(V `aggregateBrickHeader(brick)`)에서 | 7.1, 5.6 V |
| d | 8절 그대로(hit 목록 → S 컴퓨트 가시성 → 컴퓨트 hit 셰이딩). R 실측(raygen 안 VSM 0.87 > 그림자 광선 0.51 ns)이 근거 | 8절 |
| e | 브릭 진입 맵 재생성은 태양 방향 변화 > (텍셀 / 브릭 높이)일 때만; 바람은 맵 좌표 이동. S 내부 | 없음 |
| f | 프록셀 분류 단가·c_texel 분리 실측(S) 뒤 설계서 5.3 갱신 | 없음 |
| 10.3 | **GI 광선 예산의 프레임 배분**: `gi.rays_per_frame`을 tick당 총량 `gi.rays_per_tick`으로 바꾸고, 프레임 몫은 렌더러가 프레임 상수(예비 칸 `giRaysThisFrame`)로 준다: 강체 프레임 −0.04, soft+VFX 프레임 −0.11, 그 밖 +0.21 ms 상당(광선 0.5 → 0.75 M). 총량·항목 갱신 주기는 불변이라 결과가 같다 | 9절 품질 키, 5.5 프레임 상수 |
| 9.4 | `Coverage.hlsli`의 `coverageTriangleArea`를 Green 정리 스트리밍 클립으로(시그니처 불변; DesignBench `triangleAreaGreen`): 삼각형당 −0.13 ns [실측] | 없음(V 구현) |

## 12. 개정 1 11~13절 뒤의 추가 (2026-09-25 19:00; 사용자 결정 "게임 하루 속도 제한 없음", M 밴드 A/B, 물리 8절)

| 항 | 변경 | 소유 |
|---|---|---|
| 11.4 (2) | V 깊이 래스터 서비스: 태양 뷰의 컬링을 **방향 독립 1회 순회**로. 뷰 20~40개(단)가 같은 투영을 쓰면 트리를 한 번 순회하며 노드 bbox를 광공간으로 투영하고, 단별 요청 페이지 마스크(단당 2 KB)를 조회해 (클러스터, 단) 쌍을 낸다(DAG 절단은 단별 임계 비교). `DepthRasterRequest`에 "같은 투영을 공유하는 뷰 묶음"(`sharedProjectionViews`)과 단별 마스크·LOD 임계 배열을 받는 형태. 비용 [예상] 0.10~0.20(현재 clusters 0.47 + nodes 0.39) | V, S(요청 마스크 40개) |
| 11.4 (5) | `shadowSunTransmittanceAt`(5.6)의 뜻을 바꾼다: 대역 B 캐스터 투과율 층 조회 × **receiver 브릭 march**(V의 브릭 옥트리 DDA 공유 함수, 브릭 LOD = footprint, 읽기 전용). 호출 위치: S 가시성 패스(픽셀 대표 receiver), S 공기 mixed 슬라이스 표본, S hit 목록 패스. 태양 뷰 브릭 march·진입 맵(요청 5.6의 태양 뷰 항, 11절 e)은 폐기 | S(호출), V(DDA 함수 노출: `aggregateSunTransmittance(pos, dirSun, footprint)`), 코어(5.6 표기) |
| 11.4 (6) | VSM 아틀라스 = 풀: 인코딩 패스 없이 깊이 아틀라스(D16, 단별로 천정각 > 76°면 D32)를 조회가 직접 읽고, 블록 8²·32² min/max만 컴퓨트로 만든다. `VsmLevel`에 깊이 포맷 플래그 | S, V(04a8f70 아틀라스 모드에 포맷) |
| 11.4 (7) | 클립맵 단 간격 √2, 단 40개(`shadow.vsm.level_scale = 1.414`, 품질 키가 아니라 정보량 상수: 텍셀 ≤ footprint 규칙은 그대로) | S, 코어(품질 키 표) |
| 11.4 (1) | 상위 단 전파(fallback) 요청은 같은 프레임 요청 → 래스터 → 조회 구조에서 조회되지 않으면 제거 | S(조회 측정 뒤) |
| 11.4 (4) | 극좌표 단(수평면 receiver용 두 번째 단 계열)은 10.2와 같은 지위: S가 픽셀별 (footprint 면적/텍셀 면적, 긴/짧은 축 비, 법선 z) 분포를 재어 이방성 몫을 확정한 뒤 설계 개정이 정확 조건·게이트를 확정 | S(측정), 설계 개정 |
| 12.1 | `addBandedGroup`(7절)의 기본 밴드 수 1. 밴드 수는 그룹 안 커널의 실측이 바닥의 1.3배 안일 때만 올린다(M `shading.pass_bands` 규칙과 같음) | 코어 |
| 12.2 | 셰이딩 K 항 비용식: 구별되는 블록 수 × 밉 수 × 탭 + 반사 2단 읽기(M 요청 그대로). 아틀라스 표본은 일괄 발행(M 패치, R 전달) | M, R |
| 13절 | 2.13이 렌더 + 시뮬 정본 표. 강체 GPU 항은 벽시계 × 1.35(시분할 실측), 제출 프레임 최대는 substep 2개를 두 프레임에 + GI 광선 배분(10.3). 물리 tick = 렌더 프레임 정수배(55 Hz)는 사용자 결정 항목 | 물리 설계, 코어(프레임 상수) |

측정 요청: S — 전파 페이지 조회 여부, 픽셀별 footprint/텍셀·축 비·법선 z 분포, 아틀라스 모드 뒤 c_rop(D16)·블록 계층 실측; V — 1회 순회 컬링의 실측(쌍 수·ms); M — 간접/직접 분할 커널 + 중간 버퍼의 합과 밴드 A/B 재측정.

## 13. 개정 1 14절 (2026-09-25 19:50; 사용자 결정: 강체 CPU, 부하 정의 불변, tick 60 Hz, 재설계 계속)

| 항 | 변경 | 소유 |
|---|---|---|
| 14.2 ③ | 대역 B 레코드 **12 B** `{ uint visId24_area8; uint depth16_oct16; uint mask32; }`(7.1 표기 갱신). area 8 bit(1/255), depth 16 bit = 타일 [near, far] 기준(정렬·bDepth 비교 전용, 동률은 visId 순), oct16 유지. 11절 a의 16 B를 대체 | V(레코드·PS), M(합성), 코어(7.1) |
| 14.2 ④ | VSM 한 경로 권고: 페이지 캐시·dirty 추적·단 기저 분할 갱신·바람 메타·`depthRasterCovered`를 지우고 매 프레임 전부 다시 그린다(관리 → 요청 비트 prefix-sum). S가 결정한다 — 예산 행은 어느 쪽이든 전부 다시 그리기(11.5) | S |
| 14.2 ② | 공기 단 선택 3 텍셀/타일: S가 정확 조건(mixed 타일 면적비 추정 오차 1/8 → 1/6이 프록셀 저장 정밀도·필터 뒤 구분되지 않음)과 C 기준 게이트를 적은 뒤 설계 개정이 판정 | S |
| 14.2 ⑤ | 투과율 층의 높이 슬랩 마스크 합집합 변형(게이트 항): V coverage 모드에 "텍셀당 슬랩 마스크 원자 OR" 출력 형태, S 조회는 슬랩 합집합 popcount. 4매듭 층과 A/B 게이트 뒤 채택 | S, V |
| 13절 (c) | 제출 프레임 최대 조건 확정: tick 60 Hz, 강체 CPU(P-C)이므로 GPU 제출 슬롯은 soft+VFX 프레임(+0.21)뿐 → GI 광선 배분(`gi.rays_per_tick`, 11절)으로 ≈ +0.1 | 코어(프레임 상수), R |

## 14. 개정 1 12.3 — 셰이딩 커널 구조 변경 (2026-09-25 20:40; M 요청 회신)

| 항 | 변경 | 소유 |
|---|---|---|
| 12.3 | 셰이딩 그룹(8×8 타일)이 타일 모서리 프로브 2×2의 블록(레코드 70 B + 팔면체 복사 밉 0~2 84 텍셀 RGB fp16 + 팔면체 깊이 64 × 2 B ≈ 640 B/프로브, 합 2.5 KB)을 R 아틀라스에서 그룹당 한 번 groupshared에 올린다. K(로브 밉 이중선형·밉 혼합)·프로브 조도·가장자리 인식 가중치는 LDS + ALU. 픽셀당 종속 텍스처 탭 0. A/B 게이트: 현재 커널 대비 ΔL ≤ 1/256(하드웨어 필터 양자화) | M |
| 12.3 | 화면 프로브 위치 = 타일 모서리(8 px 격자, 타일 4개 공유); R 아틀라스 배치 = 프로브당 640 B 연속 블록(밉 0~2 + 깊이 + 레코드). 반사 2단 읽기는 G/M 픽셀 8 B/px 입력으로만 | R, 코어(INTERFACES 프로브 배치 표기) |
| 12.3 | 순서: LDS 판 → 점유율 실측(regs, o) → 12.1 조건 성립 시 밴드 재측정. 값 [예상]: 도시 4K 셰이딩 1.18 → 0.65~0.75, 그 뒤 밴드 −0.2~−0.35 | M, 코어 |
| 14.2-a | ③ 12 B 레코드: V 첫 실측(대역 B 가시 클러스터 185만 > 131 k)으로 (b) 타일 청크 클러스터 표(≤ 64, 지역 6 bit + 삼각형 7 bit)로 간다 — 확정은 V의 클러스터 채움 수정·대역 C 뒤 재측정 뒤 | V, M |
