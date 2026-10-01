# Fix-11: 프록셀 광원 목록 가변 길이 (결함 대기열 11, RENDERER_REDESIGN_V2 14.1 L1)

작업트리 `UnravelNext-fix`(가지 `redesign-v2-fix`, 기준 origin/redesign-v2 338cadd). 기준 빌드 = 338cadd 게이트 빌드(`baseline/bin`).

## 바뀐 것
- 목록 배치: 프록셀당 고정 32항목(헤더 `first<<6|count`) → 개수 패스(FroxelLists MODE0) → 짝수 올림 배타 접두합(FroxelScan, 2048×2단) → 채움 패스(MODE1). 헤더는 프록셀당 uint2(first, count), 런은 짝수 항목에서 시작.
- `lights_max`(32, 짝수 ≤ 96)는 "정렬된 머리"의 길이. 머리를 넘는 광원은 전부 꼬리로 저장(결정적 순서). 병합·버림 없음. `shading.analytic_lights_max` 제거(더미 읽기였음).
- 용량: CPU가 같은 프레임에 계산하는 장면 광원 항목 수의 상한(`froxelListBound`) + FX 입자 광원 몫(초기 슬롯당 2048, 초과 실측 ×1.5로 성장), 2의 거듭제곱(1/4 아래로만 축소), 최소 2^16. `needed` > `capacity`인 프레임(FX 몫 초과)만 장면 광원만의 접두합(FroxelScan 둘째 합, 상한 안)으로 런을 잡아 장면 광원은 그대로(머리+꼬리) 싣고 FX 입자 광원만 그 프레임에 뺀다: 통계(잘린 목록 = FX 항목이 있던 프록셀, 잃은 항목 = FX 항목)·게이트 FAIL·로그·몫 성장. 시험 키 `list_capacity_forced`, `list_fallback_forced`.
- FX 입자 광원의 구조 상한은 없다: 수는 슬롯 용량(`GpuScene::fxLightRange().capacity`)으로 정해지지만 범위를 GPU가 밝기와 노출에서 계산하므로(FxLights.hlsl `range = sqrt(Y·exposure/(π/1024))`, 상한 없음) 닿는 프록셀 수를 CPU가 같은 프레임에 셀 수 없다. 그래서 몫 + 위의 오류 경로(장면 광원은 정확, FX만 그 프레임에 빠짐, 게이트 FAIL)로 처리한다. FX에 범위 상한이 생기면 몫도 구조 상한으로 바꿀 수 있다.
- 소비자: 전부 `Froxel.hlsli` 헬퍼 경유. 헤더를 직접 읽던 FroxelIntegrate / FroxelQueuePrepare / VsmLocalMarkAir 수정.
- 후보 압축을 배치별 ballot(countbits 접두)로 결정적으로.

## [실측] 통계 (욕탕 bath_reference, 광원 146, 그림자 92, 1080p 정지 300프레임, 자동 노출)
| | 항목 | 잘린 목록 | 잃은 항목 | 최대/프록셀 |
|---|---|---|---|---|
| 기준 338cadd | 450752 | 914 | 6217 | 50 |
| 새 빌드 | 456969 | 0 | 0 | 50 |

라운지 기준: 1580 truncated, 8501 dropped, max 44. 그림자 오버플로 통계(words needed max 2122957, tiles over capacity 0)는 두 빌드 동일 → 추가된 항목은 비그림자 광원.

## [실측] 시험 (새 빌드, 정확성 락)
FroxelTests 0 failure(결정론: 두 프레임 목록 동일 검사 포함), LocalShadowTests 0, ShadingTests 0(시험이 옛 헤더로 만들던 가짜 목록 두 곳을 새 헤더로 고침), VsmTests 0.
[실측 2026-10-01 17:29, 게임 뒤 chain 4 tests2, 빌드 120bd7d] FroxelTests: 용량 상한 ≥ GPU 필요(5.83 M ≥ 2.50 M 항목, 버퍼 8.39 M) ok, 강제 fallback(FX 없음: 목록 항목별 동일, 잘림·손실 0; FX 있음: 장면 광원만 저장, FX 손실 집계) ok, L4 유계 생략 A/B 평균 5.76e-7·최대 8.5e-4 ok, 분류 페이지 lit 구간 A/B 평균 3.28e-7·최대 4.0e-4 ok; FX 1b 2건 실패는 시험 훅 순서(c8a0531 수정, chain 5 재실행). LocalShadowTests PASS(분류 lit 13,624쌍 위반 0, 쌍둥이 umbra 199쌍 위반 0, 오버플로 접두합 queued = inline, 용량 1 fallback ok). VsmTests PASS. ShadingTests: 14.1b 업로드 해제 오류로 중단(c8a0531 수정, chain 5 재실행; 그 앞 전부 ok, coverage 타일 광원 P99 0).

## 화면 비교
(대기열: city_block 정확성 A/B, 욕탕 8프레임+레이어 A/B, 라운지)

## [실측] timing
(대기열: 욕탕 1080p·1440p 기준 대 새 빌드)

## 파일
`run_tests.ps1`, `run_ab.ps1`, `run_ab2.ps1`, `run_exact.ps1`, `run_layers.ps1`, `run_timing.ps1`, `run_chain2.sh`; 로그 `*.log`(PowerShell `*>`는 UTF-16), 캡처 `*.pfm`, 비교 이미지 `bath_*.png`.
