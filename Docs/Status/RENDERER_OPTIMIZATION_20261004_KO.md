# Renderer 최적화 적용 및 검증 — 2026-10-04

Bathhouse 저장 씬의 1920×1080 출력(내부 1280×720, TSR) 실행에서 GPU 프레임시간 중앙값이 **17.932 → 14.724 ms, 17.89% 감소**했다. 최종 DLL로 실제 World·VFX를 포함한 120개 네이티브 프레임을 기록하고, Play 중 에디터를 종료해 정상 종료 코드 0을 확인했다. 전체 프레임이 몇 배 빨라졌다는 결과는 아니다.

소스는 `C:\Users\USER\UnravelNext-ue6`의 `lumen-ue6` 작업 트리이며 기준 커밋은 `42efeb79bd738e84c2b5259ad7aec9b6fec46902`다. 변경분은 아직 커밋하지 않았다. 직접 빌드한 Renderer DLL과 셰이더를 `C:\Users\USER\Unravel` 및 `C:\Users\USER\UnravelGames\BathhouseTycoon`에 적용했다. 기존 CPU 최적화, TSRReject 변경, Host VFX 수명 수정은 보존했다.

**실제 씬 측정**

RTX 4080, Unity 6000.6.0f1, D3D12, 저장된 `Bathhouse.unity`, 출력 1920×1080·내부 1280×720. 배포 `output.toml`의 배율 0.6667, 적용 최소 높이 1080, 동적 해상도 끔, epic tier와 `FrameRenderer::setupUpscale` 경로에 따른 크기다. 씬은 bloom/vignette만 재정의하고, 로그에 TSR 실행이 있다. 최초 보고에서 출력과 내부 해상도 구분을 빠뜨려 이를 정정했다. 네이티브 1080p 측정은 아니다. 각 실행의 120개 기록 중 처음 40개를 제외한 80개를 비교했다. p95·p99는 nearest rank이고, 같은 이름의 패스가 여러 번 실행되면 프레임 안에서 먼저 합산했다.

| 항목 | 적용 전 | 최종본 | 변화 |
|---|---:|---:|---:|
| GPU 중앙값 | 17.932 ms | 14.724 ms | −17.89% |
| GPU p95 | 18.659 ms | 15.469 ms | −17.10% |
| GPU p99 | 19.147 ms | 16.453 ms | −14.07% |
| CPU 기록 중앙값 | 1.864 ms | 1.872 ms | 거의 같음 |
| 수면 스트림 RT AS 갱신 | 2.480 ms | 0.518 ms | 약 4.8배 |
| 안개 광원 목록 | 0.891 ms | 0.108 ms | 약 8.3배 |
| 수면 메시 생성 합계 | 0.550 ms | 0.285 ms | 약 1.9배 |
| coverage 스트림 | 1.817 ms | 1.819 ms | 이득 확인 안 됨 |
| 수면 태양 맵 래스터 | 0.287 ms | 0.287 ms | 같음 |
| TSR flicker | 0.349 ms | 0.331 ms | 약 5.3% 감소 |

라이브 시뮬레이션을 별도 프로세스에서 실행한 결과다. 같은 프레임을 결정적으로 재생한 영상 비교는 아니다. GPU 작업은 직렬화했고 측정 중 별도 빌드를 돌리지 않았지만, 잠금 도구는 Unity 실행 중 백그라운드 CPU 부하를 기록했다. 이 수치를 모든 씬·장비의 보장치나 에디터 Game Stats 전체 CPU 프레임시간으로 해석하면 안 된다. 시작 첫 GPU 프레임은 538.455 → 490.838 ms였으며, 초기화 지연이 해결됐다는 주장은 하지 않는다.

**남긴 변경**

| 첨부 목록의 관점 | 적용 경로와 보존 조건 |
|---|---|
| 수학·표현 변환 왕복 제거 | `TsrResurrect`, `TsrFlicker`, `TsrThin`에서 저장된 정수 코드와 변경하지 않는 비트를 재사용한다. 기존 TSRReject 방식의 적용 범위를 늘렸으며, rejection·history 출력의 GPU 비트 동등성을 검사했다. |
| 불필요한 계산·LDS 제거, 전용 경로 | 비정렬 froxel 목록은 전용 셰이더를 사용해 소비하지 않는 중요도 계산과 24 KB shared 배열을 없앴다. 정렬 목록은 같은 순서를 보존하면서 LDS 인덱스를 바꿨다. |
| 이웃 조회·정점 계산 중복 제거 | 직사각·원형 수조에서 8×8 셀에 필요한 9×9 정점을 공유한다. 출력 위치·법선·속도·정점 수, 원형 이음새와 중심 fan을 보존한다. |
| GPU 실제 작업량, 생산자 결과 재사용 | GPU draw count로 mesh dispatch 인자를 한 번 만들고 coverage·수면 레이어·태양 맵에서 공유한다. CPU가 정확한 개수를 이미 아는 고정 수면은 인자 생성 패스를 생략한다. |
| 전체 용량 처리 제거 | 유체 메시의 NaN retirement tail을 실제로 퇴역시킬 범위만 간접 디스패치한다. 기존 tail 무효화 결과는 보존한다. |
| RTAS rebuild 제거 | 명시적으로 고정 토폴로지를 보장하는 수조만 BLAS refit한다. 생산자 ID·할당 세대·범위·삼각형 수를 확인하고, 실제 제출이 끝난 기록만 유효하게 저장한다. 활성 삼각형이 바뀌는 유체는 rebuild를 유지한다. |

해상도, 광선 수, 샘플 수, 품질 임계값, 저장 정밀도를 낮추지 않았다. 배포한 품질 설정은 기존 설정에 `raytracing.stream_refit=true`만 추가했다. `onSubmitted`는 기존 제출 통지를 이용하며, 이 최적화 때문에 command list를 분할하거나 새 GPU fence를 삽입하지 않는다.

**개별 경로 A/B 결과**

| 시험 | 원본 → 변경 | 범위 |
|---|---|---|
| TSR resurrect / flicker / thin | 0.636 → 0.552 / 0.471 → 0.409 / 0.293 → 0.281 ms | 각 200개 GPU 표본 |
| 비정렬 / 정렬 froxel 목록 | 0.717 → 0.194 / 0.993 → 0.968 ms | 각 200개 GPU 표본 |
| 직사각 / 원형 메시 | 0.0503 → 0.0256 / 0.0410 → 0.0256 ms | 각 200개 GPU 표본 |
| 희소 스트림 래스터 + 인자 생성 | 0.0359 → 0.0103 ms | 용량 1,048,576·실제 2개 삼각형, 200개 표본. 새 인자 생성 0.0051 ms 포함 |
| 고정 수면 BLAS build / update | 0.6267 → 0.0767 ms | 131,072개 삼각형 수면 4개, 100개 표본 |

RT refit에는 비용 교환이 있다. 위 시험의 BLAS 저장량은 28,472,320 → 33,090,560 B로 약 4.40 MiB 증가했고, probe ray traversal은 0.005127 → 0.009033 ms로 늘었다. AS 갱신과 traversal을 합한 시간은 줄었으며 실제 Bathhouse 반사 trace 중앙값은 약 0.39 ms로 같았다. 이는 모든 변형량과 카메라의 traversal 성능 보장은 아니다.

**출력·통합 검증**

- TSR: 셰이더마다 16,893,536개 픽셀, 총 5개 출력이 비트 단위로 동일했다. reset, 이동, HDR·비유한 입력, 부분 타일, 선택 입력과 timestep을 포함했다.
- Froxel: 114,688개 목록이 동일했다. 0~1,025개 광원, 6개 광원 유형, 정렬/비정렬, cache/recull/fallback을 포함했다. 정상 1920×1080 통합 시험과 queue A/B도 통과했다.
- 수면 메시: 6개 평탄·파형·회전·원점 원거리·정지 입력에서 모든 위치·법선·속도·draw count가 동일했다.
- RT: 19,456개 광선의 hit 거리·primitive·instance·면 방향·barycentrics가 동일했다. 제출하지 않은 graph, 움직이는 곡면, pool 성장, slot 변경, 유체 NaN 활성 전환, 유한 퇴화 삼각형을 포함했다.
- 간접 디스패치: 63개 스트림 × 4프레임의 0·부분·overflow·2차원 분할·CPU/GPU count 혼합을 검사했다. 태양 맵의 depth·normal·medium이 동일했다.
- 기존 유체 표면 검사, 수면 경계, 수면 레이어, stream coverage, Host VFX 수명 검사가 통과했다. 새 동등성 시험은 D3D12 GPU 기반 검증(GBV)을 사용했다.
- 최종 DLL로 Bathhouse 120프레임과 Play 중 정상 종료를 통과했다. 로드된 네이티브 DLL 경로·SHA-256을 기록했다. 이전에 있던 stock model 누락 등 콘텐츠 메시지는 별도로 보존했다.

비트 동등성은 검사한 입력·출력의 증거다. 전체 게임의 모든 카메라·장면 전환·시간 누적 영상에 대한 완전한 화질 승인을 주장하지 않는다. DXIL은 확인했으나 네이티브 GPU ISA, occupancy, spill 수치를 확보했다는 주장은 하지 않는다.

**제외한 후보와 확인된 한계**

`CoverageComposite` padding, `TileLights`·`FroxelIntegrate`의 LDS 배치 변경은 전체 씬에서 이득이 확인되지 않아 제거했다. 수면 경계 타일 캐시도 고밀도 합성 시험에서는 6.4% 빨랐지만 Bathhouse에서는 이득이 없었다. 최종 소스·품질 설정·배포 셰이더에서 제거했고, 실험 소스와 로그만 남겼다.

원형 수조의 기존 체적 검사는 수정본과 원본 모두 같은 지점에서 실패했다: frame 2 체적 변화 `1.43e-6 m³`, 기준 `1e-6 m³`. 원본 Pool/RoundPool 코드와 메시 셰이더를 복구해 다시 빌드한 실행에서도 동일했다. 임계값을 바꾸거나 이 검사를 통과로 표시하지 않았다. 별도의 전체 정점 동등성 검사는 통과했다.

160×90으로 축소한 froxel 통합 시험은 원본 셰이더도 같은 조건에서 실패했다. 정식 1920×1080 queue A/B 검사는 통과했다.

중간 실행 `bathhouse-final`은 Unity 초기화의 `PrefetchFileImpl → mi_free`에서 크래시했다. 크래시 시점의 모듈 목록에는 Renderer DLL이 없고 새 렌더링 기록도 없었다. 시작 크래시 원인은 수정하지 않았으며, 같은 DLL 재실행과 캐시를 제외한 최종 DLL 실행은 모두 정상 종료했다. 이 실패 실행을 성능이나 통합 성공 자료로 사용하지 않았다.

**재현 자료와 적용본**

기록 루트: [RendererSweep20261004](C:/Users/USER/UnravelNext-ue6/Logs/RendererSweep20261004)

- [검증 결과·소스·셰이더 해시](C:/Users/USER/UnravelNext-ue6/Logs/RendererSweep20261004/validation.json)
- [전체 씬 측정 집계](C:/Users/USER/UnravelNext-ue6/Logs/RendererSweep20261004/bathhouse-summary.json)
- [최종 빌드 로그](C:/Users/USER/UnravelNext-ue6/Logs/RendererSweep20261004/accepted-build.log)
- [최종 에디터 실행 로그](C:/Users/USER/UnravelNext-ue6/Logs/RendererSweep20261004/bathhouse-accepted/editor.log)
- [실제 로드된 네이티브 제품](C:/Users/USER/UnravelNext-ue6/Logs/RendererSweep20261004/bathhouse-accepted/probe/loaded-native-products.txt)
- [배포 기록](C:/Users/USER/UnravelNext-ue6/Logs/RendererSweep20261004/deployment.json)

최종 `UnravelNext.dll` SHA-256: `DB653FC7855171BC0AD9E6E32E6F0A22E083731CF25F393F60BDBA05F1F390C9`.
ABI stamp: `0558fffd17bc7c0a`. 런타임 셰이더 823개의 빌드 출력과 두 프로젝트의 적용 파일을 모두 해시 비교했다.

다른 NativeWorld·NativeVfx·NativePhysics·NativeAnimation·TitanNative DLL은 기존 통합 입력으로 그대로 사용했다. 이번 결과는 현재 소스로 빌드한 Renderer의 최적화와 해당 통합 실행 결과이며, 전체 네이티브 엔진을 새로 빌드한 제품 승인으로 대체하지 않는다.
