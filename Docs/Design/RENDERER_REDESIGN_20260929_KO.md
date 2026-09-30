# UnravelNext 렌더러 재설계안 — 같은 표본을 정보의 소유 단위에서 처리하기

작성: 2026-09-29. 기준 소스: `47c965f`의 설계 지시서 및 현재 작업 복사본. 코드를 변경하지 않은 설계 제안이다.

**판정:** 아래는 브리프 6절의 다섯 결과물을 갖춘 구현·검증 가능한 재설계안이다. **1440p 렌더러 2~3 ms와 4K 전체 6.06 ms는 목표 [예상]이며, 달성이 증명된 수치가 아니다.** 영역별 할당은 제시하지만, 이를 실측 예측값으로 위장하지 않는다. 특히 제공된 자료에는 동적 최악 부하의 광선 수·페이지 수·적분 구간 수가 없어 최악 예산은 아직 닫히지 않는다. 이 문서는 그 부족분을 숨기지 않고, 어떤 구조를 구현하고 어떤 값에서 채택 또는 기각하는지를 정한다.

기준 문서: [재설계 브리프](REDESIGN_BRIEF_20260929_KO.md), [현재 아키텍처](ARCHITECTURE_KO.md), [최근 작업 기록](../Status/CLOUD_BRIEF_KO.md), [측정 원자료](../../Results/Local/Timing20260929/README_KO.md), [하드웨어 실측 하한](../../Tools/Microbench/Results/FLOORS.md). 이전 아키텍처의 예상 예산·캐시 적중률을 현재 실측으로 승계하지 않는다.

## 0. 근거, 불변 조건, 비용을 읽는 방법

### 0.1 실측 자료의 정정과 한계

다음 표는 이 문서를 작성하면서 원본 JSON의 `gpu_frame_ms.median`을 읽은 값이다. 전부 [실측: 기존 결과 재집계], 단위 ms. 이번 세션에서 GPU를 재실행한 값은 아니다.

| 빌드·장면 | 출력 / 내부 해상도 | 중앙값 | P95 | 품질 판정 |
|---|---|---:|---:|---|
| 887b5d8 기차 | 1440p / 1440p | 9.582520 | 10.056641 | 현재 기준 경로, 알려진 품질 결함 포함 |
| 887b5d8 욕탕 | 1440p / 1440p | 11.054565 | 11.342773 | 동일 |
| 887b5d8 기차 | 4K / 1440p | 10.037231 | 10.469238 | 네이티브 4K 자료가 아님 |
| 887b5d8 욕탕 | 4K / 1440p | 11.640381 | 11.915283 | 동일 |
| 974c6bb 기차 | 1080p / 약 720p | 3.965942 | 4.276123 | 업스케일 흐림으로 불합격 |
| 974c6bb 욕탕 | 1080p / 약 720p | 4.725830 | 4.967529 | 동일 |
| 974c6bb 기차 | 1440p / 약 960p | 5.498535 | 5.798828 | 동일 |
| 974c6bb 욕탕 | 1440p / 약 960p | 6.406128 | 6.635498 | 동일 |
| 974c6bb 기차 | 4K / 약 1440p | 9.692261 | 10.161133 | 동일 |
| 974c6bb 욕탕 | 4K / 약 1440p | 11.432373 | 11.728027 | 동일 |

출처: `Results/Local/Timing20260929/<빌드>_<train_v0 또는 bath_bt0>_ev6_<출력>.json`. 내부 크기는 같은 폴더 README의 실행 설정에 따른다. JSON의 `resolution`만으로 내부 크기를 알 수는 없으므로 다음 측정에서는 실제 정수 내부 크기도 기록한다. 브리프 일부 요약값과 달리 여기서는 평균을 중앙값으로 옮겨 적지 않았다.

- `887b5d8` 자료의 실제 식별자는 `887b5d855e99530400dd09095995b9052a4f0197`, **dirty=true**, diff `636ef77259e87a89`이다. 깨끗한 해당 커밋의 실행으로 부르지 않는다. 품질 해시는 `19a6a535adca7fdb97ebaf93c3bf31e2617ff29e889902e947513e780afc3e8e`이다.
- `974c6bb` 자료는 `974c6bb5c7f9d7413c31b9d993e6f4a56e7f74ed`, dirty=false, 품질 해시 `a613a5844fabeea0dd9b839914cf578a55a69cb2b358fe5f986d960e21e94f50`이다.
- 각 실행은 600프레임 [실측], GpuLock, 경합 0 [실측], 정지 카메라다. 동적 최악, 새 콘텐츠 상주, 전체 게임 Player, 네이티브 1080p·4K에 대한 증거가 아니다.
- **중앙값은 더해지지 않는다.** 욕탕 1440p의 `median(frame)-Σmedian(pass)`는 0.748039 ms [실측 재집계]지만 `mean(frame)-Σmean(pass)`는 약 0.0001 ms [실측 재집계]다. 기차는 각각 0.588133 / 0.0595 ms [실측 재집계]다. 따라서 0.6~0.7 ms [브리프 실측 요약]를 제거 가능한 장벽 고정비로 공제하지 않는다. 패스별 표본 수·포함 범위가 같아야 평균 차도 의미가 있으므로, 다음 실행에서는 동일 frame ID의 구간을 직접 비교한다.
- 원자료에는 반사 광선 수, 가림 광선 수, 요청 페이지 수, 전량 재래스터 삼각형 수, froxel의 실제 적분 구간 수가 없다. 패스 시간에 임의 단가를 나눠 이 수들을 실측값으로 역산하지 않는다.

### 0.2 유지할 것과 바꿀 것

장면 표현, 가시성 버퍼·coverage 경로, 클러스터 LOD의 기존 품질 규칙, 재질과 BRDF, VSM 가상 주소·페이지 형식, 렌더 그래프, Unity ABI, 기준 추적기·시험 도구를 유지한다. 재질을 단순화하거나 시야 밖 광원·캐스터를 제거하지 않는다.

변경 범위는 **조명 질의의 중간 표현과 스케줄링**이다. 주 뷰·반사 hit·GI hit·공기 적분이 만든 질의를, 소비자 호출 순서 대신 표면 자료·캐시 셀·물리 그림자 페이지의 소유 순서로 처리한다. 최종 BRDF와 합성은 원래 표본 소유자에게 돌아간다. 이것은 새 엔진이나 새 렌더 그래프를 만드는 작업이 아니다.

불변 항목 [예상: 설계 계약, 현재 설정 승계]: GI 주 광선 500,000/프레임, 캐시 200,000항목, 화면 프로브 간격 8 px, 근접 가림 16탭, 화면 GI 필터 12탭, 태양 차단물 탐색 5탭·필터 16탭, G 반사 표본당 4광선, M/G/K 분류 규칙, 반사 이력 최대 32프레임, GI 이력 32/256회. GI 광선 250,000을 쓰는 옛 아키텍처 1440p 예상표를 적용하지 않는다. 동적 광원의 이력을 늘리거나 새 표본을 다음 프레임으로 미루지 않는다. 설정·실행 중 실제 값도 결과에 넣는다.

RPP-1은 256캐릭터·헤어 8체·동적 강체 1,024·soft 32·광원 512/그림자 128·수목 100,000·풀 1,000,000·입자 524,288·리본 256·볼륨 16 [예상: 보존할 명세 부하]를 그대로 유지한다. 게임 유체 128k [예상: 브리프 부하]와 과거 RPP draft의 Matter 250k [예상: 미봉인 할당]를 혼동하지 않는다. 실내 두 장면의 성공으로 RPP-1 성공을 대체하지 않는다.

### 0.3 공통 표현과 수명

기존 패스 모듈 안에 다음 논리 레코드를 도입한다. 크기는 정렬을 포함한 초기 배치 예산 [예상]이며 확정 ABI가 아니다.

| 자료 | 형식·소유 | 초기 크기 [예상] | 정확성·수명 |
|---|---|---:|---|
| SurfaceQuery | surface ID, generation, triangle/barycentric, footprint, material revision, consumer ID | 32 B/질의 | 비균일 변환·스킨·양면·재질 세대 보존; 위치를 양자화해 같은 질의로 취급하지 않음 |
| LightQuery | receiver/consumer ID, light ID+generation, sample ID, 종류 | 16 B/질의 | 광원 NEE의 원래 seed/PDF와 표본 순서 유지 |
| CellStencil | 기존 GI 레벨·모서리 handle·유효 mask·epoch | 최대 64 B/스텐실 | 모든 corner를 원래 규칙으로 touch/request; 읽기 최적화 때문에 갱신 요구 누락 금지 |
| PageWork | light/page key, receiver range, 원래 결과 offset | 16 B/receiver | physical page를 재할당하면 generation 변경; cube seam·halo 포함 |
| AirInterval | froxel, light, 원래 구적 index, page/span, range | 32 B/구간 | 광학 깊이와 광원 합을 원래 순서로 복원 |

CPU 되읽기로 작업량을 정하지 않는다. GPU count/scan과 indirect dispatch를 사용한다. 임시 목록은 밴드 링으로 처리하고 완료 fence 전에 재사용하지 않는다. 큐가 차면 같은 프레임 안에서 다음 chunk로 이어 간다. 초과 표본 폐기, 광원 병합, 지연 프레임 도입은 금지다. 총시간은 chunk 수만큼 늘어난다. 큐 용량과 성능 부하 상한은 다른 개념이다.

기차 좌표계는 **주소 안정화**에만 사용한다. 차량 내부 표면끼리 고정 관계여도 창밖 occluder, 태양, 움직이는 승객, 광원 변화는 해당 프레임 자료를 갱신한다. 좌표계가 같다는 이유로 조명을 재사용하지 않는다. frame epoch, world generation, material/light revision, producer fence를 명시적으로 전달한다.

### 0.4 비용식과 하드웨어 근거

기호: `P=W×H`, `x=P/3,686,400`, `Q_r`=반사 경로의 모든 순회 광선(주 광선+추가 가림), `Q_g`=GI 경로의 모든 순회 광선, `J`=pixel/fragment/hit-light 쌍, `D`=이번 프레임 그리는 VSM 페이지, `T_s`=그 페이지들에 제출되는 삼각형 중복 합, `F=ceil(W/24)ceil(H/24)×64`, `E_air`=실제 (구적 구간, 광원) 쌍. coverage·보조 뷰는 `P`에 숨기지 않고 별도 `P_aux`, `F_cov`로 더한다.

| 출력=내부 [예상: 기본 경로] | P | x | 화면 프로브 | F |
|---|---:|---:|---:|---:|
| 1080p | 2,073,600 | 0.5625 | 32,400 | 230,400 |
| 1440p | 3,686,400 | 1 | 57,600 | 410,880 |
| 4K | 8,294,400 | 2.25 | 129,600 | 921,600 |

기차 인스턴스 1,052·삼각형 약 195k, 욕탕 인스턴스 2,291·삼각형 약 11k [실측: 브리프]는 유지한다. 이 값은 `T_s`와 같지 않다. 그림자 페이지마다 다시 제출되기 때문이다.

실측 하한은 동일 기계의 `FLOORS.md`에서 가져온다. 이 표의 처리율은 해당 벤치의 달성값이며, 다른 접근 패턴의 보장 처리율은 아니다.

| 근거 | 값 [실측] | 적용 제한 |
|---|---:|---|
| 비압축 순차 읽기 / 쓰기 / read+write 복사 | 697.7 / 675.2 / 621.9 GB/s | 트래픽은 읽기+쓰기를 모두 셈 |
| 무작위 4 B DRAM / 32 MiB 상주 | 5.14 / 64.69 Gload/s | hash lookup을 순차 697.7 GB/s로 계산하지 않음 |
| 무작위 64 B, 32 MiB 상주 | 55.26 Gload/s | 캐시 메타뿐 아니라 동시에 읽는 전체 집합으로 상주 판정 |
| 32 MiB 작업 집합의 4 KB chunk 읽기 | 3,987 GB/s | 캐시 전체 수백 MB에 적용 금지 |
| 종속 load: L1 / L2 / DRAM | 35.53 / 116.1 / 238.3 ns | 단일 포인터 체인의 지연; 모든 질의에 직렬로 곱하지 않음 |
| coherent trilinear / incoherent bilinear | 361.6 / 26.4 Gsample/s | VSM·GI 복합 커널의 실효율은 새로 측정 |
| Dispatch+UAV barrier | 0.8858 μs | 실제 graph 비용이나 모든 barrier 1개의 비용이 아님 |
| 짧은 alpha DispatchRays / 긴 얇은 식생 alpha | 2.951 / 1.186 Gray/s | 표준 DXR; NVAPI OMM/SER 수치 제외 |
| 긴 얇은 식생 inline alpha RayQuery | 0.6405 Gray/s | inline이 항상 유리하다는 가정 금지 |
| depth-only, 삼각형 변 0.5 / 2 / 16 px | 32.92 / 12.89 / 3.417 Gtri/s | 큰 삼각형을 32.92 Gtri/s로 세지 않음 |

각 순차 phase `k`의 비용 하한 모델 [예상]은 다음과 같다.

`C_k = d_k δ + max(Bseq_k / βseq_k, Bl2_k / βl2_k, Nrandom_k / λrandom_k, Ntex_k / λtex_k, Q_k / ρray_k, T_k / ρtri_k, chains_k × depth_k × latency_k / Aeff_k, Ccompute_k)`

`T_domain = Σ_k C_k + Tsort + Tscan + Tscatter + Tboundary`.

단위는 B/ms, load/ms, sample/ms, ray/ms로 통일한다. `Aeff`는 숨길 수 있는 동시 종속 체인 수로, SM 수와 wave occupancy만 곱해 추정하지 않고 MB-1~MB-7에서 측정한다. 실제 trace→shade→shadow처럼 순차 phase 사이에는 `max`를 쓰지 않는다. `Ccompute`는 LTC·SFU·레지스터 압력 때문에 필요한 복합 커널 실측 항이다. FLOP 수로 대체하지 않는다.

회계 규칙: 한 dispatch와 그 barrier의 시간은 한 소유 영역에만 넣는다. 각 `C_k`의 `d_kδ`에 넣은 작업을 1.7의 fixed 행에 다시 넣지 않는다. fixed 행은 영역 밖의 실제 control/args/query·잔여 동기화 작업만 소유한다. 여러 consumer가 함께 쓰는 커널은 전체 시간을 한 번 측정하고, 영역별 비용 귀속은 해당 프레임의 실제 작업 수로 나눈 회계값임을 표시한다. 이 배분값을 다시 독립 실행 단가처럼 쓰지 않는다.

대표 비용식을 간단히 표기할 때 `βseq=600 GB/s` [예상], `βl2=3 TB/s` [예상]를 쓰지만, 이는 위 실측보다 낮게 둔 후보 단가일 뿐이다. 모든 영역의 **할당값 `A(x)`는 실측으로 도달해야 하는 한도**이며 이 비용식으로 이미 증명됐다는 뜻이 아니다.

## 1. 영역별 재설계

### 1.1 반사 — 표본 수를 유지하는 hit 자료 분리와 질의 묶음

**정보량.** 광선 하나는 hit identity, footprint, 이동, 입사 방향과 outgoing 방향을 가진다. 같은 삼각형을 맞혔다고 같은 복사휘도가 아니다. 노멀맵·BRDF·가려짐·면광원 방향이 변하므로 결과 색을 삼각형당 하나로 공유하지 않는다. 공유할 수 있는 것은 같은 material/texture page, 같은 GI corner 주소, 같은 그림자 페이지, 같은 광원 구조다.

**새 알고리즘.** 기존 M/G/K·평면 뷰 분류와 주 광선을 그대로 생성한다. `ReflectionTrace`의 hit를 consumer 순서에서 material page/캐시 셀 순서로 제한된 radix binning한다. 표면 해석, GI 질의, 국소광 sample 선택, 태양·국소광 visibility 요청을 각각 좁은 커널에서 처리한다. visibility는 1.3의 공통 receiver 서비스로 전달한다. 마지막 BRDF는 원래 hit의 방향·footprint·PDF로 계산하고 consumer ID로 scatter한다. M/G의 면광원 이중 계산 방지 규칙과 emitter mask를 유지한다. 이미 존재하는 trace/compute 분리는 재구현하지 않고, **compute 내부의 랜덤 gather를 실제 자료별 큐로 분리**한다.

국소광 sample ID는 재정렬 전 생성한다. 키에 surface/light generation을 넣는다. RNG를 dispatch 위치로 다시 seed하지 않는다. 동일 질의의 부분 결과를 공유하려면 모든 입력의 동등성이 성립해야 한다. 근접 위치·유사 roughness를 같은 것으로 취급하는 보간은 3절 결정 항목이다. 평면 반사는 현재 마스크 뷰와 같은 기하·셰이딩을 유지하며 `P_aux`를 별도 청구한다.

**자료·메모리 [예상].** ray hit와 surface/light 질의, 값 및 permutation을 포함한 처리 중 상한을 192 B/활성 hit로 설계한다. 262,144 hit 밴드는 약 48 MiB이며 입력·출력과 현재 캐시 hot set이 함께 L2에 들어간다고 가정하지 않는다. 실제 밴드는 65,536 hit부터 [예상: 약 12 MiB] 시험한다. 모든 ring copy가 이 크기를 동시에 점유하는 경우도 예산에 포함한다.

**비용식 [예상].**

`T_R = Ctrace(Q_r) + Csurface(H, Bmaterial) + Cbin(H) + Cgi-hit(H) + Cshadow-hit(J_r) + Cbrdf(H) + Cscatter(H) + Tplanar(P_aux)`.

교차 서비스로 보낸 hit의 GI·그림자 비용도 이 행에 귀속한다. 주 뷰의 GI/그림자 행에 다시 더하지 않는다. 입력 byte ledger의 192 B는 상주 크기다. sort가 여러 번 읽고 쓰는 트래픽은 그 배수를 따로 센다.

수치 대입을 위한 민감도 사례 [예상, **장면 실측이나 허용 부하 상한 아님**]: 기차 `Q_r=0.75Mx`, 욕탕 `0.36Mx`. 짧은 alpha 벤치의 2.951 Gray/s [실측]를 낙관적으로 적용해도 순회만 기차 0.143/0.254/0.572 ms, 욕탕 0.069/0.122/0.274 ms [예상, 1080p/1440p/4K]다. 긴 식생 1.186 Gray/s [실측]라면 각각 약 2.49배 [예상]가 된다. MB-1이 실제 `Q_r`와 접근 분포를 먼저 기록한다.

대표 영역 할당 [예상]: 기차 `A_R=.10+.50x`, 욕탕 `.07+.24x` ms, 즉 **0.381/0.600/1.225**, **0.205/0.310/0.610** ms. 이 한도와 위 순회만의 비용 사이에 모든 binning·hit 셰이딩·추가 가림 비용이 들어가야 한다. 안 들어가면 광선을 줄이지 않고 이 구조를 기각한다.

**품질.** 동일 seed에서 재정렬 전후 sample ID, hit identity·barycentric, PDF, visibility와 소비 결과를 비교한다. fp32 합 순서 변화는 오차 분석 대상이며 비트 동일을 무조건 주장하지 않는다. `reflectionanalytic`, furnace의 평균·개별 outlier, `hostmotion`, 평면 거울·거친 금속·욕탕 수면·기차 창문을 정지와 이동 모두 검사한다. 기존 furnace 실패를 허용치 확대나 새 baseline으로 흡수하지 않는다.

**최악과 실패.** 캐시 재사용 0, 매 프레임 전부 새로운 hit·광원, 자료 bin이 모두 분산되는 입력도 처리한다. 전 화면 G 간격 1이면 주 광선만 `4P`; 1440p 14.746M [예상]이다. 긴 얇은 식생 벤치율에 대입한 순회 비용은 12.43 ms [예상]로 목표 밖이다. 이 사례는 임의 콘텐츠 전체에 대한 3 ms 보장을 현재 근거로 할 수 없음을 보여준다. 이를 부하 제한으로 해결하지 않는다. RPP-1 실제 `Q_r`의 상한과 MB-1이 맞지 않으면 추가 표현 재설계가 필요하다. 표준 DXR 파이프라인과 inline 질의를 둘 다 측정하되 벤더 전용 가속을 예산에 넣지 않는다.

### 1.2 GI — 셀 주소와 법선 응답을 분리한 정확 스텐실 평가

**정보량.** 넓은 면에서는 이웃 픽셀이 같은 셀·레벨·모서리를 찾는데 hash probing을 반복한다. 그러나 위치의 삼선형 가중치와 법선의 octahedral 보간 가중치는 서로 다르다. 픽셀 하나의 조도 값을 이웃에 복사하면 노멀맵과 경계가 바뀐다.

**새 알고리즘.** 기존 200k world cache와 500k 주 광선 [예상: 보존 계약]을 유지하고 조회를 두 단계로 바꾼다.

1. 질의를 `(frame epoch, level, cell, normal-map texel patch, 유효 corner mask)`로 묶어 CellStencil을 만든다. 화면뿐 아니라 GI hit·반사 hit도 같은 주소 사전을 이용하되 비용은 소비자별로 분리한다.
2. 같은 stencil의 모서리 자료를 한 번 적재하고 각 픽셀의 실제 위치·법선 가중치로 평가한다. 텍셀 patch 안에서는 공간 삼선형×법선 이중선형의 multilinear 계수를 쓸 수 있다. 레벨 blend, normal fold, 미수렴·빈 corner, footprint 경계가 다르면 다른 stencil이다. fp32 결과는 기준 함수와 ULP 및 상대오차로 대조한다.
3. 12탭 화면 필터는 동일 좌표·가중치·near occlusion을 보존한다. 출력 조도를 texture로 한 번 모은 다음 별도 필터로 소비한다. 생산과 소비가 같은 workgroup 범위임을 증명하지 못하면 한 커널로 합치지 않는다.
4. cache 갱신은 별도 current/next epoch로 처리한다. 조회 묶음이 원래의 touch/request를 생략하지 않도록 요청 OR와 갱신 우선순위를 보존한다. GI 누적·Jacobi·광원 변경 반응을 바꾸지 않는다.

과거 quad 공유·일괄 corner 적재는 실제로 느려져 제거됐다(`GiScreenIrradiance.hlsl` 주석). 이번 차이는 wave 내 중복 명령이 아니라 **질의 목록에서 stencil당 작업을 한 번 소유**하는 것이다. 정렬·중간 버퍼를 포함한 비교가 실패하면 채택하지 않는다.

**자료·메모리 [예상].** stencil 64 B와 8 corners×4 normal texels×RGB fp32=384 B를 하나의 hot block으로 본다. 16k stencil 창은 약 7 MiB다. 원래 캐시 본체의 수백 MB가 L2에 상주한다고 계산하지 않는다. 32계수 전체를 픽셀별 저장하지 않으며, fp32 coefficient 변환 비용도 MB-2에 포함한다.

**비용식 [예상].**

`T_G = Ctrace(Q_g) + Cintegrate(500k, updated entries) + Crequest(U_g, hash probes) + Cevaluate(P, stencils, level branches) + Cfilter(12P) + Cprobe(Nprobe,16 taps)`.

`Nrandom ≈ U_g×h + unresolved_queries×h`; `U_g`는 unique stencil 수, `h`는 충돌을 포함한 실제 탐사 길이다. `U_g=P`도 처리하며 이때 얻는 공유 이득은 0이다. hash table 상주/비상주별 load율을 적용한다. `Q_g`에는 500k 주 광선 외의 태양·국소광 NEE 광선을 모두 더한다.

두 장면 모두 주 광선 0.5M [예상: 보존값]의 순회만 약 0.169 ms [예상, 2.951 Gray/s 적용], 긴 식생이면 약 0.422 ms [예상]다. 추가 가림이 주 광선당 2개이면 총 1.5M [예상], 순회만 약 0.508~1.265 ms [예상]가 된다. 따라서 GI 전체 0.55 ms [예상 할당]는 추가 광선이 많은 입력에서 성립하지 않을 수 있다.

12탭 필터의 texture 요청 수는 두 장면 각각 24.883/44.237/99.533M [예상, 1080p/1440p/4K]. coherent trilinear 361.6 Gsample/s [실측]만 적용한 접근 하한은 0.069/0.122/0.275 ms [예상]이고 depth/normal fetch·가중치·write는 별도다. 단순히 필터를 0으로 둔 비용을 채택하지 않는다.

대표 할당은 두 장면 모두 `A_G=.37+.18x` = **0.471/0.550/0.775 ms [예상]**. 해가 매 프레임 바뀌어도 주 광선 총량과 갱신 정책을 유지하며, 캐시 수렴을 완료한 정지 화면만으로 이 예산을 검증하지 않는다.

**누설 해결 후보 — 기본 정확 경로와 분리.** 기존 위치·법선 hash는 벽 안팎이 한 셀을 공유한다. 단순 corner 제거·재정규화는 얼룩을 만들었다. 대안은 `(surface chart, side, topology generation)`를 주소에 추가하고 서로 연결되지 않은 벽·바닥을 분리하는 **표면 소유 GI 계층**이다. 방 ID만으로 광수송을 차단하지 않는다. 문을 통과하는 빛은 원래 광선이 운반하며, chart는 저장 지원 영역을 나누는 것뿐이다. 스킨·파괴 시 topology generation을 갱신하고 이전 표면 history를 새 조각에 잘못 옮기지 않는다. 면적당 ray budget이나 총 entry 수를 줄이지 않는다. 필요한 entry가 늘면 메모리와 갱신 시간 증가를 함께 보고한다. 이것은 추정 지원 영역을 바꾸므로 **D2 결정·품질 게이트 전까지 기본 예산의 이득으로 계산하지 않는다.**

**품질·최악.** `gianalytic`, albedo 0.5/0.9 furnace, 수렴 편향·분산, 벽 뒤 강한 태양, 문 열기, 어두운 방의 폭발 섬광, 360°/s 회전 [예상 시험]을 검사한다. 새 화면의 첫 프레임과 기존 relight 한도도 검사한다. 모든 stencil이 고유하거나 모든 캐시 entry가 젊은 경우 `U_g` 전체를 처리한다. 조회 최적화는 GI의 3% 누설 [실측: 기존 보고]을 자동으로 해결하지 않는다.

### 1.3 그림자 필터 — 광원·페이지별 receiver 서비스

**정보량.** 한 receiver의 그림자는 `(light, receiver plane, blocker distribution, filter footprint)`의 함수다. 같은 페이지를 읽는다고 visibility가 같은 것은 아니다. 여러 픽셀·hit가 읽는 깊이 자료는 공유하고 비교값·탭 좌표·가중치는 각 receiver가 가진다.

**새 알고리즘.** 주 뷰의 고정 슬롯과 별도 overflow 셰이딩을 하나의 가변 receiver-light stream으로 바꾼다. 기존 froxel 광원 목록의 모든 원소를 열거하며 셋째 이후 광원의 재분류·재해석을 없앤다. 큐를 `(light, mip, physical page)`로 묶고 depth/투과율 블록을 coalesced하게 읽는다. 태양과 국소광, 주 뷰와 reflection hit는 같은 서비스의 다른 consumer다. 결과는 소비 순서대로 저장한다.

차단물 5탭과 필터 16탭 [예상: 보존값]을 유지한다. 보수적인 block min/max가 **receiver plane의 전체 비교 범위와 필터 footprint를 포함할 때만** 완전 lit/umbra를 판정한다. 투과율 층이 있으면 해당 층도 범위 판정에 들어가야 한다. mixed block은 원래 모든 탭을 평가한다. 차단물 평균 거리까지 알아야 하는 단계에서 단순 min/max만으로 평균을 대체하지 않는다. cube seam과 fine/coarse fallback 규칙도 동일하게 유지한다.

**자료·메모리 [예상].** receiver-light 16 B, 결과 4 B 및 index ring. 1M pair 창은 약 24 MiB(index 포함)다. 모든 pair를 전 화면에 동시에 저장하지 않고 light/page 밴드로 나눈다. 요청과 결과의 fence는 shadow atlas producer 뒤에 놓는다. 더 많은 광원이 닿는다고 결과를 상수 조도로 합치지 않는다.

**비용식 [예상].**

`J=Σ_receiver shadowLightCount(receiver)`

`T_S = Cbuild(J) + Cbin(J) + Cblocks(U_page) + Cmixed(5J +16J_mixed, extra cube/PCF texels) + Cscatter(J)`.

`J_mixed=J`를 최악으로 쓴다. 탭이 2×2 PCF이면 logical sample과 실제 texel/load를 분리해 센다. 대표 민감도 입력은 기차 `J=1.5P`, 욕탕 `4P` [예상, 원자료에 없는 값]이다. 즉 1440p에서 5.530M / 14.746M pair [예상]. 21 logical tap 단순 기준이면 coherent trilinear 벤치율의 접근만 0.321 / 0.856 ms [예상]이며, incoherent bilinear라면 4.399 / 11.729 ms [예상]다. **이 차이가 주소 재배열의 연구 대상이지만 coherent 벤치율에 도달해도 아래 할당이 자동으로 성립하지 않는다.** local의 실제 estimator가 태양과 다르면 각 tap 수로 대체한다.

영역 할당 [예상]: 기차 `.02+.22x` = **0.144/0.240/0.515 ms**, 욕탕 `.02+.30x` = **0.189/0.320/0.695 ms**. 이를 맞추려면 동일 depth texel의 실제 fetch를 여러 receiver에서 재사용하는 효과가 필요하다. 전부 mixed이고 footprint가 서로 떨어지는 MB-3에서 불가능하면 해당 할당을 미달로 기록한다. 캐시 lit 비율을 올린 정지 장면만 제출하지 않는다.

**품질·최악.** `vsmtests`, `localshadowtests`, `shadingtests`, 투명·얇은 캐스터·물·머리카락, 매우 가까운 면광원, 움직이는 태양·섬광·문·파괴를 검사한다. 접촉 경계와 반그림자 에너지, 누설, 모든 overflow pixel을 별도 mask로 비교한다. 페이지 순서가 바뀌어도 visibility는 같은 논리 texel에서 읽어야 한다. 새 캐스터 등장 프레임에 stale 값은 허용하지 않는다.

### 1.4 공기 적분·국소광 페이지 표시와 전량 VSM 갱신

**정보량.** localmark는 같은 광원·mip·페이지 요청을 픽셀마다 반복하며, 공기는 유사한 shadow page를 서로 다른 froxel loop가 읽는다. 표면의 실제 요청 집합과 공기 광학 구간의 합집합을 각각 한 번 만들고, 의미가 다른 두 집합은 합치되 소비 이유를 보존한다.

**페이지 표시 재설계.** 현재 `VsmLocalMark.hlsl`의 각 pixel/light가 만드는 최대 3 mip×5방향 [예상: 현재 알고리즘]의 요청을 유지한다. tile/light마다 같은 cube face·mip·page에 들어간다는 **보수 범위 증명**이 가능하면 같은 key 요청을 한 번만 발행한다. 그렇지 않으면 원래 pixel별 projection과 요청을 수행한다. 파괴된 기하·얇은 표면·cube seam은 exact enumeration으로 간다. wave/local hash에서 key를 deduplicate한 뒤 전역 bitset으로 OR한다. 범위의 사각형을 무조건 모두 요청하는 방법은 과잉 페이지와 fine/coarse 선택을 바꾸므로 기본안에서 제외한다.

**공기 재설계.** 이미 있는 froxel 64 slice prefix와 local-light batch를 다시 제안하지 않는다. 현재 tile별 긴 loop를 **프레임 전체의 `(shadow page, froxel interval, light)` 작업 목록**으로 바꿔 긴/짧은 구간을 따로 처리한다. 구적 위치·수는 보존하고 page/medium 경계를 가로지르는 작업만 분할한다. 일정 계수 구간에서 `(T,L)`의 결합은 `(T_aT_b, L_a+T_aL_b)`지만 부동소수 합 순서를 바꾸는 오차는 검증한다. 고도·매질이 변하는 구간은 기존 구적을 그대로 평가한다. constant-medium 정확 적분을 임의 대기 전체에 적용하지 않는다.

**VSM 전량 재그리기.** 페이지 캐시는 여유만 만든다. 예산에는 `D = requested pages`, `T_s = all caster/page triangles`를 사용한다. light/page별 caster bin을 한 번 만들고 기존 depth raster를 소비시킨다. 태양이 매 프레임 임의 방향으로 바뀌어도 기저·높이 범위·페이지 데이터를 그 프레임에 갱신한다. 전량 dirty일 때 cache 판정·비교 비용을 더하지 않도록 생성 단계에서 전체 갱신 경로를 선택한다. 캐시 on/off가 결과와 시간 모두 검증 가능해야 한다.

**자료·메모리 [예상].** PageWork와 AirInterval은 공유 scratch ring에서 최대 64 MiB를 각각 배정하고 서로 수명이 안 겹치는 범위는 alias한다. VSM 페이지의 D32 128²는 64 KiB/페이지다. 4,096페이지에 깊이만 256 MiB, 부가 블록 약 44 MiB [예상]이며 page table·투과율 층은 추가다. D16 전환을 승인된 성능 이득으로 넣지 않는다.

**비용식 [예상].**

`T_mark = Cproject(P×Lshadow) + Cdedup(K_request) + Cemit(U_request)`

`T_air = Cclassify(F) + Cqueue(E_air) + Cwalk(Nblock,Ntexel) + Cquadrature(E_air) + Cprefix(F) + Cvolume(Nvolume)`

`T_VSM = Ccull(Ncaster_page_pairs) + Cclear(65536D bytes) + Craster(T_s, pixel coverage, alpha fragments) + CpageMax(D) + Ctransmittance(F_sun)`.

공간 재사용이 전혀 없는 경우도 처리하는 비용을 센다. 아래 가정 입력은 실측이 아니며, 성능을 맞추기 위해 허용 페이지 수를 제한한 값도 아니다.

수치 민감도 입력 [예상]: 기차 `D=2,048x, T_s=6Mx`, 욕탕 `D=4,096x, T_s=9Mx`로 전량 갱신을 계산한다. D는 실제 실행에서 정수 올림한다. depth clear만 기차 **0.126/0.224/0.503 ms**, 욕탕 **0.252/0.447/1.007 ms** [예상, 600 GB/s, 1080p/1440p/4K]다. 변 2 px depth 삼각형 12.89 Gtri/s [실측] 기준 래스터만 기차 **0.262/0.465/1.047 ms**, 욕탕 **0.393/0.698/1.571 ms** [예상]다. 둘은 순차라 더한다. 작은 삼각형 단가로 바꾸거나 dirty 비율을 낮춰 숨기지 않는다.

**이 입력이면 아래 전량 갱신 할당을 초과한다.** 실제 `D,T_s,alpha fragments`가 없으므로 이를 해결됐다고 선언할 수 없다. 재설계의 핵심 확인은 page 표시의 정확 중복 제거가 실제 요청량을 유지하면서 불필요한 반복 raster 제출을 없애는지다. 논리 요청 집합 자체가 이만큼 크면 더 깊은 페이지 표현 재설계가 필요하며, 원래 품질을 유지하는지 D3에서 판정한다.

대표 할당 [예상]: VSM 기차 `.08+.18x` = **0.181/0.260/0.485 ms**, 욕탕 `.10+.20x` = **0.213/0.300/0.550 ms**. 공기+mark는 기차 `.03+.16x` = **0.120/0.190/0.390 ms**, 욕탕 `.04+.20x` = **0.153/0.240/0.490 ms**. `F`는 0.4 표의 실제 해상도별 식을 쓰고, 광원 교차 수를 상수로 숨기지 않는다. `E_air`가 커지면 해당 항이 선형 증가한다.

**품질·최악.** `volumetests`, atmosphere/froxel 분석 시험, localshadow, god ray의 가려짐 변화, 창밖이 빠르게 지나가는 기차, 폭발 섬광, 물속·연기·역광을 검사한다. shadow page 사이 구간이 길게 교대하는 입력도 목록에 전부 담는다. cache hit 0, 모든 국소광 이동, 동적 캐스터 전체 이동, 모든 태양 페이지 dirty를 동시에 시험한다. 단일 dispatch 분할은 TDR 방어이지 총시간 해결이 아니다.

### 1.5 셰이딩·해석 — receiver 자료를 한 번 만들고 재질별로 소비

**정보량.** vis ID로부터 복원한 위치·법선·UV gradient·재질 값은 태양·국소광·GI·반사 분류가 반복해서 읽는다. 다만 view-dependent BRDF와 필터 footprint는 최종 consumer의 것이다. 면광원 14개 [실측: 욕탕 브리프]를 대표광 하나로 합치지 않는다.

**새 알고리즘.** `m.resolve`를 얕은 geometry/identity 해석과 material 해석으로 분리한다. 깊이·identity만 필요한 cull/mark는 얕은 자료를 쓴다. 최종 shading은 material variant별 tile 목록으로 모으고 compact resolved input을 재사용한다. normals/roughness/clearcoat/film/anisotropy와 texture derivatives의 정밀도를 유지한다. LTC 행렬·광원 정점과 material 상수는 tile에서 공유하지만 LTC 적분의 pixel별 방향은 그대로 계산한다.

무거운 재질에 경량 경로를 억지 적용하지 않는다. 재질 경계·coverage fragment·hair·water·transmission은 각각 원래 자료를 가진 별도 stream이다. 면광원 diffuse를 모서리 값으로 보간하거나 specular를 낮은 차수 방향 기저로 대체하는 방법은 D1에만 둔다. 모든 픽셀 full layered일 때도 같은 합성 순서로 실행한다.

**자료·메모리 [예상].** compact input은 우선 32 B/pixel의 임시 예산으로 잡고, 실제 계약에 필요한 값이 넘으면 늘린다. 1440p 112.5 MiB / 4K 253.1 MiB [예상]를 전 화면에 영구 저장하지 않고 64k pixel 창 약 2 MiB [예상]로 소비한다. 이 창의 boundary halo·모션·이력은 별도다. 레지스터 압력이나 halo 때문에 기존 banding처럼 손해가 나면 unfused 경로가 남는다.

**비용식 [예상].**

`T_M = Cgeometry(P+F_cov) + Cmaterial(P,Ntexture,variant) + Cdirect(J_direct,LTC type) + Ccombine(P) + Cedge(F_cov) + Caux(P_aux)`.

decode 후 write/read 합이 64 B/pixel [예상 사례]이면 순차 트래픽만 **0.221/0.393/0.885 ms [예상]**다. 32 B read+write 총량까지 줄이면 **0.111/0.197/0.442 ms [예상]**다. 밴드가 producer/consumer 사이 실제 cache hit를 만들지 못하면 아래 할당은 매우 빡빡하다. 바이트를 전역에서 LDS로 옮겼다고 전체 트래픽이 사라졌다고 세지 않는다.

대표 할당 [예상]: 기차 `.02+.30x` = **0.189/0.320/0.695 ms**, 욕탕 `.02+.40x` = **0.245/0.420/0.920 ms**. 수식의 `J_direct=Σ_receiver L_direct`를 기록해야 한다. light·material coherence가 깨진 최악에서도 같은 광원을 모두 계산한다.

**품질·최악.** `shadingtests`, furnace, material layers·LTC 기준 구적, 윤기 있는 타일·얇은 막·clearcoat·젖은 기차 벽·비등방 물체를 검사한다. 노멀맵 high-frequency와 negative scale, coverage edge의 동일 BRDF를 포함한다. 레지스터 수·점유율·실제 load throughput도 MB-6에 기록한다. 기능 조합 때문에 DXIL이 한도를 넘으면 재질별 커널 경계를 나누며 기능을 삭제하지 않는다.

### 1.6 업스케일 재구성 — 기본 예산은 원래 해상도, 선택 경로는 검증 후

**기본안.** 전체 예산표의 1080p·1440p·4K는 모두 내부=출력 [예상: 후보 계약]이다. 현재 흐린 업스케일의 시간을 품질 합격 수치로 사용하지 않는다. 업스케일은 승인된 레버지만, 그 구현의 합격까지 승인된 것은 아니다.

**정보량과 새 선택 경로.** 새로 드러난 출력 픽셀의 high-frequency texture·specular는 과거 history에 없을 수 있다. 시간 누적만으로 그 프레임의 원래 신호를 항상 복원할 수 없다. 새 경로는 output-grid의 jitter sample 누적, 정확한 surface motion/ID, specular hit motion, 노출 보정과 신뢰도, reactive/translucency mask를 분리한다. disocclusion/급변 조명/얇은 기하에서는 출력 해상도 **현재 프레임 재셰이딩 목록**으로 부족한 자료를 채운다. sharpen으로 신호를 만들어 통과시키지 않는다.

출력-grid coverage·depth/ID는 얇은 기하를 놓치지 않아야 한다. 내부 저해상도 depth만으로 이미 놓친 표면을 신뢰도 검사에서 복원할 수 없으므로, output-grid visibility 보충 비용을 포함한다. motion blur·DoF·UI 적용 순서와 sharp 영역을 명시하고 기준과 동일하게 둔다.

**자료·메모리 [예상].** history color 2벌 RGBA16F=16 B/output px, depth/ID/moments/reactivity 포함 총 32 B/output px 초기 예산: 63.3/112.5/253.1 MiB [예상]. correction mask/list는 추가 8 B/output px 상한 [예상]. 내부/출력 두 surface buffer의 동시 수명도 계산한다.

**비용식 [예상].**

`T_U(P,s,d)=Tnative(s²P)+Cvisibility_output(P)+Creconstruct(P)+Ccorrect(dP)+Chistory(P)`.

`s`는 한 축 내부 비율, `d`는 출력 해상도 보정 비율이다. 순이득은 `Tnative(P)-T_U`이며 항상 양수가 아니다. 후보 `s=2/3` [예상]의 내부 pixel은 0.922/1.638/3.686M [예상], 출력은 0.4 표와 같다. reconstruction의 최소 48 B/pixel [예상 사례] read/write만 600 GB/s에서 **0.166/0.295/0.664 ms [예상]**다. 실제 보간 탭·모션·보충 visibility는 추가한다. 기본 표의 업스케일 비용은 0 ms [예상: 사용 안 함]이고, 이 선택안을 쓰면 이 항을 반드시 새로 더한다.

**최악.** `d=1`인 첫 프레임·컷·폭발·완전 가려짐 해제에서는 네이티브 렌더 한 번보다 비쌀 수 있다. 잘못된 결과를 낸 뒤 다음 프레임 native로 바꾸는 방식은 해당 첫 프레임을 해결하지 못한다. frame 시작의 복원/컷은 미리 native로 보내고, 예측 불가능한 변화는 same-frame 보정 전체 비용을 부담한다. 이 최악이 4K 예산을 넘으면 업스케일로 4K 합격을 선언하지 않는다.

**판정 지표 [예상: 기존 기준에 추가할 시험 기준].** 같은 출력 해상도에서 native `--capture` 대 `--capture-output`, 같은 tick·노출·셔터·카메라로 비교한다. 정지와 움직임에서 각각 고주파 대역 0.25~0.5 cycles/pixel의 energy ratio 0.95~1.05, SSIM ≥0.99, edge spread 증가 ≤0.25 output px를 후보 경계로 둔다. 이 숫자는 사용자의 눈 판정을 대체하지 않는다. high-frequency energy만 맞추는 ringing을 막기 위해 overshoot, FLIP P99, temporal error와 disocclusion 첫 프레임을 함께 본다. 기준의 잡음 바닥을 넘는 편향·잔상은 실패다. 최소 1/8/32/128프레임 정지 수렴 [예상 시험]과 보행·회전·기차 창밖·물·머리카락에서 검증한다. 기존 반사/hostmotion 시험도 통과해야 한다.

### 1.7 프레임 고정비 — 실재하는 의존성 경계만 줄이기

**정보량.** 패스 수가 같아도 비용은 커널 시간, pipeline flush, cache 경합, query 배치에 따라 달라진다. 0.1의 중앙값 차이를 고정비로 취급하지 않는다. 기존 graph 계획 캐시 8개·용량 bucket은 유지한다.

**새 실행 순서.** surface decode → 기존 표본 생성 → GI/그림자 질의 정리 → 전량 또는 dirty VSM raster → receiver visibility·hit lighting → pixel BRDF·합성 → 후처리. GI update와 읽기 epoch, dynamic AS의 준비, FX tick producer는 기존 의존성을 명시적으로 유지한다. 동일 thread가 생성하고 즉시 소비하는 classify/args/짧은 resolve만 합친다. global scan·indirect args·UAV producer/consumer 사이에는 필요한 barrier를 남긴다.

패스 합체로 늘어나는 register/LDS·DXIL과 줄어드는 메모리 트래픽을 같이 비교한다. banding은 자료 hot set이 맞을 때만 선택한다. buffer alias는 plan 전체의 마지막 GPU reader fence 뒤에서만 한다. async compute 이득은 기본 예산에서 **0 ms [예상]**다. 기존 overlap 2~6% [실측: 아키텍처]로 4배 개선을 설명하지 않는다.

**비용식 [예상].** `T_fixed = Ddependent×δ + Cbarrier_actual + Cquery + Cqueue_wait + Cresidency_sync`. 패스 timestamp가 이미 포함한 dispatch 비용을 다시 더하지 않는다. 예상 dependent chain 수 60/80/110 [예상: 1080p/1440p/4K 후보]에 0.8858 μs [실측]를 적용하면 0.053/0.071/0.097 ms [예상]다. 실제 패스 개수 목표가 아니라 줄어드는 의존 chain의 민감도 계산이다.

두 장면 모두 고정 작업 할당 `.10+.02x` = **0.111/0.120/0.145 ms [예상]**. 출력·기본 후처리는 별도로 `.02+.06x` = **0.054/0.080/0.155 ms [예상]**. bloom/DoF/motion blur·투명 합성의 활성 비용을 이 작은 행에 숨기지 않는다. 해당 효과가 켜진 게임 경로는 2절의 전체 합에 실제 비용을 더한다.

**검증·최악.** per-frame timestamp로 frame span, 겹치지 않는 phase span, barrier 전후를 비교하고 timestamp on/off 오버헤드도 기록한다. D3D debug validation·GPU validation·alias barrier·resize·cut·restore·device removed·고정 스텝 여러 번의 hostmotion을 실행한다. 그래프 재사용은 CPU 개선으로 따로 보고 GPU 이득과 더하지 않는다. 모든 작업이 같은 자원을 요구해 직렬화되는 경우에도 기본 합계로 판정한다.

## 2. 전체 예산, 메모리, 최악 조건

### 2.1 영역별 할당표

**전부 [예상: 설계 할당], 단위 ms, 각 셀은 기차 / 욕탕. 원래 해상도다.** 아래 숫자는 1절의 `A(x)`를 대입한 값이다. 실측 하한이 이 할당의 달성을 보장하지 않는다. 기존 시간의 일정 비율을 곱한 성능 예측표도 아니다. 단계별 개발을 계속할 수 있는 비용 한도를 명확히 하는 표다.

| 영역 | 1080p | 1440p | 4K |
|---|---:|---:|---:|
| 반사 전체(그 hit의 조명·가림 포함) | 0.381 / 0.205 | 0.600 / 0.310 | 1.225 / 0.610 |
| GI update·화면 조회·필터·프로브 | 0.471 / 0.471 | 0.550 / 0.550 | 0.775 / 0.775 |
| 주 뷰 그림자 필터 | 0.144 / 0.189 | 0.240 / 0.320 | 0.515 / 0.695 |
| VSM 전량 갱신 | 0.181 / 0.213 | 0.260 / 0.300 | 0.485 / 0.550 |
| 공기·페이지 표시 | 0.120 / 0.153 | 0.190 / 0.240 | 0.390 / 0.490 |
| 셰이딩·해석 | 0.189 / 0.245 | 0.320 / 0.420 | 0.695 / 0.920 |
| 가시성·AS·특수 표면 기본 몫 | 0.165 / 0.145 | 0.200 / 0.180 | 0.300 / 0.280 |
| 출력·기본 후처리 | 0.054 / 0.054 | 0.080 / 0.080 | 0.155 / 0.155 |
| GPU 고정 작업 | 0.111 / 0.111 | 0.120 / 0.120 | 0.145 / 0.145 |
| 업스케일 | 0 / 0 | 0 / 0 | 0 / 0 |
| **렌더러 소계(반올림 전 합)** | **1.816 / 1.785** | **2.560 / 2.520** | **4.685 / 4.620** |
| 렌더러 여유 할당 | 0.240 / 0.240 | 0.240 / 0.240 | 0.240 / 0.240 |
| **렌더러 합계** | **2.056 / 2.025** | **2.800 / 2.760** | **4.925 / 4.860** |

특수 표면 기본 몫은 두 실내 장면에 대한 초기 할당이다. RPP 전체의 hair/coverage/water/volume 비용이 이 금액이라는 뜻이 아니다. 별도 수식:

`T_render = ΣT_domain + T_visibility(I,C,Tvis,Fcov) + T_AS(Ndyn,Ndeformed) + T_special(Nhair,Npart,Nvolume,Pwater) + T_post(active optics) + T_fixed`.

표의 가시성·AS·특수 표면 합을 계산할 때 기차 `.12+.08x`, 욕탕 `.10+.08x` [예상 할당]을 썼다. RPP에서는 실제 위 변수로 **행을 교체**해야 하며 두 번 더하지 않는다. `N_dyn`과 변형 BLAS가 커지면 해상도에 비례하지 않는 비용이 늘어난다.

### 2.2 전체 GPU 프레임

GPU 유체 128k의 0.63 ms/프레임 [실측: 브리프 인용, 현재 모든 조건 재측정 아님]를 별도 더한다. particle+cloth 0.20 ms와 Unity 추가 GPU 작업 0.10 ms는 **[예상 할당]**이며 측정되지 않았다. 동시 실행 할인은 없다.

| 항목 | 1080p 기차/욕탕 | 1440p 기차/욕탕 | 4K 기차/욕탕 |
|---|---:|---:|---:|
| 렌더러(여유 포함) [예상] | 2.056 / 2.025 | 2.800 / 2.760 | 4.925 / 4.860 |
| 유체 + particle/cloth + host GPU [실측 인용 0.63 + 예상 0.20+0.10] | 0.930 / 0.930 | 0.930 / 0.930 | 0.930 / 0.930 |
| **전체 할당 [예상]** | **2.986 / 2.955** | **3.730 / 3.690** | **5.855 / 5.790** |
| 전체 목표 6.06까지 남은 몫 [예상] | 3.074 / 3.105 | 2.330 / 2.370 | 0.205 / 0.270 |

여유 0.24를 P95 보정 계수처럼 사용하지 않는다. 실제 P95는 전체 프레임 표본으로 판정한다. GPU 틱이 한 렌더 프레임에 여러 번 제출되면 `n_fluid×Tfluid + n_fx×Tfx + n_cloth×Tcloth`를 더한다. 시간 스텝이나 부하를 줄이지 않는다. 추가 틱 때문에 선을 넘으면 그 경우의 전체 프레임 목표는 미달이다. CPU tick·Present 대기 역시 사용자 프레임 주기에 따로 기록한다.

### 2.3 현재 근거로 예산이 닫히지 않는 조건

| 조건 | 비용식으로 확인되는 문제 | 설계 판정 |
|---|---|---|
| 전 화면 G, 작은 blur, 캐시 hit 0 | 1.1의 예에서 순회만 12.43 ms [예상, 1440p] | 광선 수 유지 경로의 보편적 3 ms 보장 근거 없음 |
| 모든 태양/국소광 페이지 dirty | 1.4 욕탕 가정에서 clear+raster만 1.145 ms [예상, 1440p], 할당 0.300 초과 | 실제 page/caster 중복량과 재표현을 확인하기 전 합격 불가 |
| 그림자 footprint가 전부 mixed·분산 | 1.3의 fetch 하한조차 할당 초과 가능 | 자료별 scheduling만으로 부족한지 MB-3으로 판정 |
| GI hit마다 추가 가림 광선 다수 | 500k만으로 순회 비용을 셀 수 없음 | Q_g 전체와 trace/shade 단가 필요 |
| 처음 보는 영역·전량 disocclusion | 시간 재사용과 업스케일 보정 이득 소멸 | native 또는 same-frame 보정 전체 비용 적용 |
| RPP hair·얇은 식생·물·volume 동시 부하 | 실내 패스 시간은 이 부하를 포함하지 않음 | 원래 RPP 부하로 별도 검증, 실내 통과로 대체 금지 |

따라서 **현재 정량 결론은 “2.800/2.760 ms [예상 할당]을 만족시키는 구현 후보와 기각 조건을 정했다”이다. “품질 불변 최악 3 ms를 증명했다”가 아니다.** 이 간극을 cache hit, 빛 속도 제한, 부하 감소, 미승인 광선 감소로 닫지 않는다. 4절 P0에서 부하 counters를 얻으면 각 영역의 식으로 표를 갱신하고, 선을 넘으면 해당 표현을 다시 설계한다.

### 2.4 8 GB VRAM·16 GB RAM에서의 상주 계약

다음은 8 GiB급 장치에 대한 **[예상: 엔진 내부 최대 상주 할당]**이다. OS가 실제로 허용하는 budget은 `QueryVideoMemoryInfo`로 확인한다. 모든 풀 최대값의 합, 프레임 링, alias로 줄인 물리 할당, resize 시 겹치는 할당을 각각 보고한다.

| 소유 | 최대 동시 상주 [예상, MiB] | 조건 |
|---|---:|---|
| 원래 품질 texture mip·material data | 1,280 | 현재와 예측 영역의 필요한 mip를 residency 완료 후 사용 |
| geometry·cluster | 896 | 현재/이전 pose 소비자 포함 |
| BLAS/TLAS·변형 scratch | 640 | RT/Skin fence 완료 전 퇴역 금지 |
| VSM depth·blocks·투과율·page tables | 768 | 페이지가 이 예산을 넘으면 low-res page로 대체하지 않음 |
| GI cache·screen probe·이력 | 448 | D2 chart 추가량은 이 안에 맞추거나 재산정 |
| atmosphere LUT·air·보조 뷰 | 256 | J_ms 64 MiB [실측: 현 구조] 포함 |
| render targets·motion·history·후처리 | 512 | 선택 업스케일 포함 시 실제 동시 수명 확인 |
| 공통 query rings·sort/scan·graph transients | 512 | 모든 cache plan이 붙잡은 heap을 합산; 장면 전체 queue 동시 저장 금지 |
| simulation·유체·FX·cloth | 512 | 렌더 자원과 별도 회계 |
| upload/readback·PSO·기타 엔진 GPU | 256 | staging ring의 중복 포함 |
| **엔진 합** | **6,080 MiB** | 약 5.94 GiB [예상] |
| 8 GiB에서 나머지 | 2,112 MiB | OS·Unity·다른 앱·fragmentation의 실제 여유를 보장하는 수치는 아님 |

현재 JSON의 alias transient heap은 기차 1440p 약 1,479.5 MiB, 욕탕 약 903.5 MiB [실측 재집계]다. 위 512 MiB query/transient 행을 달성하려면 전 화면 큰 ray/중간 버퍼를 실제로 밴드화하고, 별도 render target·history 행과 겹치는지 분류해야 한다. 작은 ring을 선언한 것만으로 메모리 목표를 통과하지 않는다.

RAM 할당 [예상]: 엔진/Unity CPU working set 6 GiB, RAM asset cache 3 GiB, decode+upload staging 1 GiB, OS·다른 앱 여유 6 GiB. 로컬 개발 PC의 더 큰 메모리를 전제로 하지 않는다. cook/streaming 입력의 최악 decode expansion도 기록한다.

NVMe → RAM → VRAM은 **원래 요구 해상도 자료를 전부 준비한 뒤 publish**한다. 복사 완료 fence와 리소스 상태 전이, generation을 통과하기 전 shader·writer에 주소를 노출하지 않는다. 쓰기 대상도 allocation/MakeResident 완료 후 사용한다. RAM/VRAM ring의 retired generation은 모든 큐 reader가 끝난 뒤 회수한다.

필요한 프리페치 창 [예상 식]: `distance >= vmax×(Tio+Tdecode+Tupload+Tqueue) + camera_guard`. 광원·반사·그림자가 필요로 하는 뷰 바깥 범위도 포함한다. 강제 teleport로 미상주 asset을 즉시 요구하면 저장장치 지연을 3 ms로 보장할 수 없다. 정확한 자료를 기다리는 stall을 보고하고 performance gate는 실패로 처리한다. 이미지를 낮은 mip로 바꾸거나 누락시켜 통과하지 않는다. 8 GB에 실제 최악 working set이 안 들어가면 패키징·working set 표현을 다시 설계해야 한다.

### 2.5 이식성과 구조적 상한

표준 D3D12, DXR 1.1, 현재 bindless kernel에 필요한 SM 6.6, 기존 mesh shader 경로를 유지하는 경우 Mesh Shader tier 1을 명시 요구한다 [예상: 지원 계약]. **FL 12_0이라는 이름만으로 이 기능들을 보장하지 않는다.** 시작 시 각각 feature query한다. 기능이 없는 GPU의 지원을 주장하지 않으며, 필요하면 별도 표준 indexed raster 경로의 품질·비용 설계가 필요하다. NVIDIA 전용 SER/OMM/NVAPI는 핵심 경로에서 사용하지 않는다. wave32 고정 가정도 하지 않는다.

커널별 DXIL ≤200 KiB [예상: 기존 빌드 한도 보존]. record 생성·sort·scan·가림·BRDF를 기능 단위로 분리해 한 mega-kernel에 넣지 않는다. dispatch당 record 창 ≤65,536, 한 loop chunk의 광원 ≤32·구간 ≤64 [예상: 초기 실행 분할]로 시작한다. 전체 데이터는 여러 chunk로 모두 처리한다. 각 shader 루프는 명시적 현재 chunk 끝을 가지며, 초과분은 다음 같은-frame dispatch로 전달한다. hash probing은 table size 이내, BVH 소프트웨어 보조 순회는 유한 노드 수와 continuation을 사용한다.

최악 분포에서 dispatch 시간 ≤0.25 ms는 **[예상: 측정할 목표]**이지 위 record 수만으로 증명된 시간이 아니다. DXR의 하드웨어 BVH·alpha traversal은 삼각형 겹침과 candidate 수에 좌우되므로 총 scene/candidate 입력 범위를 함께 측정한다. ray count만 잘라 무한히 큰 장면의 시간을 보장했다고 하지 않는다. 오류·기기 제거는 기존 host 계약으로 전달한다.

## 3. 결정 필요 항목

아래는 **미승인**이다. 문서 작성 요청을 표본·품질 정책 변경 승인으로 해석하지 않는다. 기본 예산에 아래 이득을 더하지 않았다. 표본 수 감소와 부하 감소는 실행안으로 넣지 않는다.

| ID | 결정 대상 | 이득 [예상] | “보이는 것이 같다”의 근거와 부족분 | 기본안과의 관계 |
|---|---|---|---|---|
| D1 | view-dependent hit 조명을 방향 기저/표면 radiance atlas로 근사 | 기차 hit shade 1.69 ms [실측] 중 정확 평가가 남는 비율 r에 대해 최대 `(1-r)×1.69 - build/gather` ms; r=0.25이면 gross 1.27 ms [예상], 순이득 미측정 | 저주파 diffuse에는 가능성이 있으나 specular·film·면광원 경계에는 근거 부족. 잔차 및 적응 분할의 최악 비용 필요 | MB-1 정확 경로가 실패할 때만 검토. RGB 한 값 공유는 금지 |
| D2 | 표면 소유 GI chart로 보간 지원 영역 변경 | 화면 조회+필터 현재 기차 1.28/욕탕 1.10 ms [실측 반올림] 중 `1-candidate cost` 비율; 후보 절감 0~0.8 ms [예상], 추가 storage/update로 음수 가능 | 분리된 벽의 공유를 막는 구조적 이유는 있음. chart 경계 seam, 파괴·문·곡면에서 품질 검증 없음 | ray/entry 총량을 줄이지 않는 prototype. 누설 해결을 승인 전 성능 이득으로 계산하지 않음 |
| D3 | 같은 VSM 논리 texel을 lazy clear·압축 tile로 저장하는 depth backend | 1.4 가정에서 clear의 최대 0.224/0.447 ms [예상, 1440p]에서 metadata/read 비용을 뺀 값; raster 비용은 그대로 | untouched texel이 정확한 clear depth를 반환하면 수학적 값 보존. 하드웨어 depth raster와의 연결·비압축 최악·투과율은 미검증 | VSM 페이징은 유지하지만 depth storage backend 변경이므로 별도 승인·실험 |
| D4 | 광선 budget·M/G 경계·G sample 수 또는 시간 누적 길이 변경 | 순회·hit 비용은 줄인 표본 몫에 비례 [예상]; 현재 비용 상한은 반사 3.43/1.10 ms [실측] | 동적 first-frame variance가 커질 수 있음. 동일 외형 근거 없음 | **채택 권고 안 함.** 숫자를 맞추기 위한 기본 선택지 아님 |

이미 승인된 내부 해상도 조절과 화면 GI 프레임 분할은 새 승인 항목이 아니다. 다만 전자는 1.6의 quality gate 전까지 꺼 두며, 후자는 현재 이득 없음 [실측: 브리프] 때문에 기본안에서 사용하지 않는다. GI를 4분할하면 새 조명/가려짐에서 재사용 불가 비율 `d`만큼 즉시 전체 갱신해야 한다. 순이득은 `saved queries - motion/validity/recovery cost` [예상 식]이고 `d=1`에서 절감 0 또는 음수다. 느린 변화만으로 승인된 레버의 성공을 주장하지 않는다.

이번 문서 작성에서 Unreal Engine 소스를 열람하거나 복사하지 않았다. 현재 저장소의 Lumen/TSR 관련 주석은 과거 참고 기록으로만 읽었다. 새 구현에 Unreal 코드를 가져오지 않는다. 이후 실제 개념 조사를 하면 실제로 열람한 파일 경로와 개념을 해당 단계 문서에 기록한다.

## 4. 구현 순서와 단계별 수용 조건

모든 단계에서 기존 경로를 A/B 진단 경로로 남기고 같은 씬·seed·tick을 실행한다. 새 경로가 기존 시험을 깨면 다음 단계와 합쳐 원인을 가리지 않는다. **이 절은 향후 구현 지시이며 이번 작업에서 구현한 목록이 아니다.** 코드 파일명은 현재 소스에서 확인했다. 추가 파일명은 제안이다.

| 단계 | 교체/추가 모듈·파일 | 기대 이득 [예상] | 로컬 확인·채택 기준 |
|---|---|---|---|
| P0 계측 기준 고정 | `Shadow/Gates/RendererGate.cpp`, `Render/src/GpuProfiler.cpp`, `Frame/FrameRenderer.cpp`; 결과 schema 확장 | 직접 속도 이득 0; 잘못된 0.6~0.7 ms 고정비 공제 방지 | 실제 render/output 크기, scene SHA, 모든 quality 값, Q_r/Q_g/J/D/T_s/E_air, per-frame timestamp, dirty diff 확보. current-source baseline을 두 장면에서 native 1080/1440/4K로 다시 생성 |
| P1 exact page request 소유 | `Shadow/VsmLocalMark.hlsl`, `VsmLocalMarkAir.hlsl`, `VsmSystem.cpp`; 제안 `PageRequestWork.hlsl` | 욕탕 localmark 1.18 ms [실측]에서 0~0.8 ms [예상]; sort 비용에 따라 음수 가능 | 요청 key 집합·mip/cube seam 동일, full-dirty 페이지·raster 수, `localshadowtests`, `vsmtests`, 공기·물. 실패하면 기존 mark 유지 |
| P2 receiver-light stream | `ShadowVisibility.hlsl`, `ShadowOverflow.hlsl`, `ShadowPenumbra.hlsl`, `ShadowFragments.hlsl`, `ShadowTrack.cpp`; 제안 `ShadowReceiverWork.hlsl` | 기존 필터 1.37/1.99에서 할당 0.24/0.32까지 gross 1.13/1.67 ms [예상 요구량] | MB-3에서 mixed/random 포함; 모든 탭·광원·coverage 동일, overflow loss 0. 매 프레임 광원/해 이동 시험 |
| P3 GI stencil | `GI/GiCache.hlsli`, `GiCacheTile.hlsli`, `GiScreenIrradiance.hlsl`, `GiScreenFilter.hlsl`, `GiSystem.cpp`; 제안 `GiStencilBuild.hlsl` | 화면 조회 0.89/0.73에서 후보 0.10~0.20으로 gross 0.53~0.79 ms [예상]; 필터는 별도 보존 | unique=P·차가운 cache·hash 충돌 포함 MB-2. touch/request counts, GiAnalytic, furnace, 노멀맵·이동. 과거 quad-batch 회귀 재현 방지 |
| P4 reflection 자료 스케줄링 | `Reflection/ReflectionShadeRays.hlsl`, `ReflectionLocalShadow.hlsl`, `ReflectionSystem.cpp`, `RayTracing/HitShading.hlsli`; 제안 `ReflectionSurfaceWork.hlsl` | 반사 전체 3.43/1.10 → 할당 0.60/0.31의 요구 절감 2.83/0.79 ms [예상]; P2 공유 이득과 중복 합산 금지 | ray count/seed/PDF 유지, `reflectionanalytic`·`hostmotion`·planar/water. 통계적 outlier 근본 원인 해결 없이 통과 판정 금지 |
| P5 air interval 소유 | `Atmosphere/FroxelIntegrate.hlsl`, `FroxelLists.hlsl`, `Shadow/FroxelSystem.cpp`, `VsmAir.hlsli`, `VsmLocalAirWalk.hlsli`; 제안 `AirIntervalWork.hlsl` | 욕탕 integrate 1.10 ms [실측]에서 0~0.8 ms [예상] | 같은 구적 지점·매질·광원 합, god ray, volume/atmosphere 분석, 계단형 그림자 최악. 총 queue/scan 비용 포함 |
| P6 material 소비·graph 경계 | `Shading/ShadeOpaque.hlsl`, `ShadingSystem.cpp`, `Frame/FrameRenderer.cpp`, `Render/src/RenderGraph.cpp` | 기차/욕탕 M+resolve 1.10/1.77 [실측 반올림]에서 할당 0.32/0.42까지 요구 절감 0.78/1.35 ms [예상] | material variants·coverage·thin film·LTC·motion, MB-6/7, 실제 DXIL≤200 KiB, transient peak≤2.4절 할당. CPU graph cache와 GPU 절감 분리 |
| P7 선택 추정기·업스케일 | D1~D3 승인된 항만 해당 모듈; `Shading/Upscale.cpp`, `Upscale.hlsl`, `UpscaleMotion.hlsl` | 순이득 미확정 [예상], 1.6식으로만 계산 | 품질 게이트와 same-frame correction 최악을 먼저 통과. 실패하면 native와 exact 경로 유지 |
| P8 전체 수용 | 기존 Host·Unity bridge·RPP gate, 코드 의미 변경 없이 통합 | 단계 이득의 단순 합산 금지 | 아래 Q0~Q5, 8 GB·16 GB 상주, 모든 dirty·동시 시뮬·RPP 원부하. 사용자 화면 판정 |

단계 이득은 대부분 **목표까지 필요한 절감량**이며 검증된 예측이 아니다. 측정 이득이 작으면 실제 값으로 전체 예산을 고친다. P2/P4는 같은 shadow hit 비용, P3/P4는 같은 GI gather를 공유하므로 단계별 이득을 더해서 총속도를 만들지 않는다.

공통 수용 절차 [예상: 실행 계획]:

- **Q0 출처:** source SHA+dirty diff, shader hash, quality hash, scene/content SHA, loaded DLL 경로·hash, GPU·driver·D3D12Core, 실제 출력/내부 크기. 이번 JSON만으로 미추적 scene의 동일성을 확인했다고 하지 않는다.
- **Q1 정확 변환:** 고정 seed와 같은 sample ID로 old/new 결과, intermediate IDs, visibility·PDF, NaN/overflow를 검사. 산술 순서 변경은 fp32 허용오차를 근거와 함께 정한다. 기존 임계값을 완화하지 않는다.
- **Q2 광수송:** 같은 BRDF·광원·노출의 독립 경로추적 기준. 4,096 spp부터 [예상 시험] 두 seed 기준 간 잡음 바닥을 측정하고 더 필요하면 늘린다. global mean뿐 아니라 벽·바닥·수면·광원 근처 ROI의 signed bias, relMSE, FLIP 평균/P99, 분산을 기록한다. GI 누설과 면광원 중복은 개별 시험한다.
- **Q3 움직임:** 정지 수렴, 보행, 회전 90/360/720°/s, 기차 60 m/s와 제동, 모든 동적 물체 이동, 파괴, 빠른 노출 변화·폭발광, 매 프레임 불연속 태양 방향, 복원·컷 [예상 시험 시나리오]. 숫자는 속도 제한이 아니라 시험점이며 태양은 rate-limit 없이 갱신한다. 첫 프레임·가려짐 해제·steady state를 각각 보고한다.
- **Q4 비용:** GpuLock, 동일 카메라·tick replay, warm-up 뒤 A/B/A/B. 정지 600프레임 [예상 기존 방식 재사용]과 RPP 120초 경로 3회 [예상: 기존 명세]를 함께 사용한다. median/P95/P99/max, renderer와 sim·host를 분리하고 전체 frame도 측정한다. 캐시 전량 무효와 contention 0을 별도 기록한다. 성능과 품질은 같은 설정·부하의 실행이어야 한다.
- **Q5 사용자 시각 판정:** 목욕탕 줄눈·욕조 기저·벽 밝은 점, 기차 창밖·광택 벽·무기/뷰모델, 물·머리카락·연기에서 native 기준과 무표식 A/B를 정지 및 동작 중 비교한다. 자동 점수가 좋아도 흐림·잔상·깜빡임이 보이면 불합격이다.

필수 기존 시험은 `reflectionanalytic`, `gianalytic`, `hostmotion`, `vsmtests`, `localshadowtests`, `shadingtests`, `volumetests`다. 앞선 보고에 알려진 실패가 있으므로 green baseline이 없으면 실패를 먼저 분류한다. 새 설계가 실패를 가렸다는 이유로 기준을 바꾸지 않는다.

기존 시험보다 느슨해지지 않는 추가 품질 판정안 [예상: 구현 전 고정할 후보 기준]은 다음과 같다. 더 엄격한 기존 기준이 있으면 그 기준을 적용한다. reference 자체의 잡음은 독립 seed로 추정하며, 새 알고리즘의 오차와 함께 숨겨 합산하지 않는다.

| 대상 | 측정 가능한 판정안 [예상] |
|---|---|
| exact 자료 재배열 | sample/receiver/광원 ID 집합·개수·세대는 완전 동일. 자료 복사는 비트 동일. fp32 재결합은 해당 연산의 forward-error bound와 기준 경로 차이를 함께 보고하며 blanket epsilon을 주지 않음 |
| GI·반사 furnace | 기존 평균 ±1% 조건 및 기존 outlier 조건 유지. albedo 0.9에서 수렴 전 편향과 수렴 후 편향을 분리하고, 불합격 픽셀의 위치·기대값·sigma를 보존 |
| 실제 씬 광수송 | 기준이 충분히 수렴한 ROI에서 signed luminance bias 목표 ≤1%. 새 relMSE·FLIP 평균/P99가 기존 경로보다 나빠지지 않아야 하며 reference noise floor를 보고. 기존 결함을 고치는 경로는 해당 누설·밝은 점 ROI도 개선해야 함 |
| 시간 반응 | 조명 변경 재관측 최대 8프레임이라는 현재 계약 보존. 프레임별 참조 오차의 P95/P99와 disocclusion 첫 프레임 오차가 기준 경로보다 증가하지 않음. 같은 픽셀 위치의 밝기 차이만으로 움직임 품질을 판정하지 않음 |
| 그림자·공기 | 같은 receiver와 구적 node에서 기존 visibility·투과율을 비교. 정확 재배열은 기존 수치 오차 범위, 표현 변경은 경계 위치와 구간 에너지의 reference 오차가 악화되지 않음 |
| 업스케일 | 1.6의 고주파·SSIM·edge spread 조건과 출력 해상도의 temporal/ROI 비교를 모두 적용. sharpening으로 고주파 지표만 맞춘 경우와 사용자에게 구분되는 경우는 실패 |

## 5. 새로 측정할 microbench 목록

하한 표는 존재하므로 이번 문서 작업에서 새 GPU 벤치를 실행하거나 벤치 코드를 추가하지 않았다. 아래는 **새 접근 패턴의 구현 채택을 결정하는 측정 명세**다. 일반 memcpy/FLOP 벤치 재실행으로 대체하지 않는다. 각 벤치는 real scene에서 기록한 질의·자료와 합성 최악 입력 둘 다 받는다.

| ID | 입력·독립 변수 | 측정할 것 | 정확성·기각 기준 |
|---|---|---|---|
| MB-0 frame ledger | 두 장면 native 3해상도, full dirty·동작, timestamp on/off | 같은 frame의 phase 시간, 모든 Q/J/D/T_s/E_air, stream bytes, scratch peak | 평균/중앙값 혼용 없이 재합산; GPU frame과 겹치지 않는 구간 합의 차를 직접 설명 |
| MB-1 hit packet | 원래 모든 반사/GI hit, unique ratio 0~1, cold/hot material, alpha·full layered | binning+sort+scatter 포함 ns/hit, 전체 ray/s, texture·GI load, occupancy | sample/seed/PDF 동일; N=1/direct보다 느린 분포도 보고. 원래 표본 수에서 1.1 할당에 들어가는지 판정 |
| MB-2 stencil | unique=P부터 고중복, 8 corner·법선 patch·24 level 경계, 충돌 table, nonresident cache | 실제 hash probes/query, L2 miss, coefficient build·평가·filter 합, ns/pixel | 기존 GiCache 함수와 같은 입력 비교; 젊은 corner·touch/request 손실 0; 기존 quad-batch 변형도 대조 |
| MB-3 shadow receiver | 모든 mixed, 1~128광원, cube seam, 모든 tap 분산, 다양한 filter radius | page binning 비용, actual depth loads/receiver, fetch 공유율, texel 처리율, shadow 전체 | 5/16탭 및 local estimator 동일; 초과 광원/fragment 누락 0. 낙관적 coherent율을 실제 커널에 적용할 수 있는지 판정 |
| MB-4 request·raster | 전체 scene의 실제 requested set, D/T_s/alpha 면적, 모든 페이지 dirty | project/dedup/emit, depth clear, page-caster bin, 크기별 raster, pagemax·투과율 | requested key/mip 집합 동일. 모든 방향 태양과 움직이는 기차에서 1.4식 대입; 평균 dirty율 사용 금지 |
| MB-5 air interval | 실제 F/E_air, 광원 512 중 교차 목록, 매질·page 경계 교대, 긴/짧은 구간 혼합 | interval 생성+정렬+shadow walk+구적+prefix, max dispatch | 같은 구적·같은 매질에 대한 reference; volume 에너지·색·transmittance 비교. iteration 생략 금지 |
| MB-6 material band | 같은 재질·모두 다른 재질, 14면광원 욕탕, specular/film/hair/coverage | decode bytes·texture locality, LDS/레지스터, producer/consumer 전체 시간 | band 없는 기준보다 빠른 범위와 손해 범위 모두 보고; 희귀 재질 off로 측정 금지 |
| MB-7 graph·alias | 실제 DAG 재생, 1~여러 band, timestamp on/off, 직렬/async | barrier·cache flush·command boundary의 실제 구간, 동시에 살아 있는 heap, CPU plan 시간 | alias/fence validation 통과. async 이득은 같은 전체 frame에서 재실측될 때만 반영 |
| MB-8 upscale | 정지·회전·새 표면·섬광, d=0~1, 같은 출력 native | reconstruction·output visibility·same-frame correction의 전체 비용, HFE/SSIM/FLIP·잔상 | native와 사용자 시각 판정. d=1 최악 시간과 full-resolution 첫 프레임 오류 공개 |
| MB-9 residency·8 GB | 원래 texture/mesh working set, cold NVMe·RAM hit·VRAM hit, teleport, resize | I/O/decode/upload/resident fence, peak RAM/VRAM, page-fault·stall, 단일 dispatch 최대 | 원래 자료가 준비되기 전 사용 0; low-res 대체 0; full-quality 부하를 유지한 6.06 ms 전체 판정 |

**문서 완료와 구현 완료의 구분:** 이번 결과물은 이 설계 문서뿐이다. 비용 재집계와 산술·참조 점검을 수행했으며 GPU microbench, 코드 변경, shader compile, runtime quality acceptance를 수행한 것으로 기록하지 않는다. 구현 착수의 첫 판단은 MB-0~MB-4로 `2.1 할당표`와 실제 비용식의 차이를 닫는 것이다. 모든 조건이 검증되기 전에는 renderer 3 ms·RPP 4K 6.06 ms·8 GB 제품 수용을 완료로 표시하지 않는다.
