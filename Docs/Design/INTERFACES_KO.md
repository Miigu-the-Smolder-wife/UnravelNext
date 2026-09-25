# UnravelNext 인터페이스 (v1.8, 2026-09-25)

렌더러를 네 세션이 병렬로 짜기 위한 계약이다(REBUILD_PLAN 14.1). 설계는 `ARCHITECTURE_KO.md`가 정하고, 이 문서는 트랙 사이의 경계만 정한다. **코드의 헤더가 이 문서와 같은 내용을 담고, 둘이 다르면 헤더가 틀린 것이다.** 이 문서에 적힌 파일 경로·함수 이름·레이아웃은 트랙이 바꾸지 않는다.

---

## 0. 지위와 변경 절차

- 소유자: 코어 세션(코어 + V). 문서와 공유 헤더는 코어만 고친다.
- 트랙이 인터페이스 변경(필드 추가, 서비스 추가, 레이아웃 변경, 새 품질 키를 다른 트랙이 읽어야 할 때)이 필요하면 **고치지 않는다.** `Docs/Design/Requests/<날짜>_<트랙>_<주제>.md`에 새 파일로 이유·원하는 변경·영향을 적고(이 폴더는 모든 트랙이 새 파일만 추가할 수 있다) 사용자에게 알린다. 코어가 반영하면 이 문서의 버전 기록(12절)에 남긴다.
- 인터페이스 변경 없이 되는 일: 자기 폴더 안의 모든 코드·커널·테스트·게이트, 자기 품질 파일의 키 추가, 자기 소유 자원의 내부 형식(다른 트랙이 읽지 않는 것).

## 1. 트랙과 소유 폴더

| 트랙 | 세션 | 소유 (쓰기) |
|---|---|---|
| 코어 | 코어 세션 | `CMakeLists.txt`, `cmake/`, `External/`, `Native/Core/`, `Native/Scene/`, `Native/Render/include/`, `Native/Render/src/`, `Native/Render/Frame/`, `Native/Render/CMakeLists.txt`, `Native/Render/Passes/Common/`, `Native/Render/Passes/Test/`, `Tests/`, `Tools/CI/`, `Tools/ShaderCompiler/`, `Tools/Microbench/`, `Docs/Design/`(Requests 제외), `Docs/Status/`, `Config/quality/output.toml` |
| V 가시성 | 코어 세션 | `Native/Render/Passes/Visibility/`, `Tools/ClusterBuilder/`, `Config/quality/visibility.toml` |
| M 재질·셰이딩 | M 세션 (v1.2) | `Native/Render/Passes/Material/`, `Native/Render/Passes/Shading/`, `Config/quality/material.toml`, `Config/quality/shading.toml` |
| S 그림자·하늘 | S 세션 | `Native/Render/Passes/Shadow/`, `Native/Render/Passes/Atmosphere/`, `Config/quality/shadow.toml`, `Config/quality/atmosphere.toml` |
| R 광선·GI·반사 | R 세션 | `Native/Render/RayTracing/`, `Native/Render/Passes/GI/`, `Native/Render/Passes/Reflection/`, `Config/quality/raytracing.toml`, `Config/quality/gi.toml`, `Config/quality/reflection.toml` |
| C 기준·콘텐츠 | C 세션 | `Reference/`, `Tools/SceneGen/`, `Config/quality/reference.toml` |
| I 통합 | I 세션 (v1.6) | `Native/Host/`, `Config/quality/host.toml` (이전 저장소 쪽 `Assets/UnravelNextBridge/`) |
| 모두 | — | `Docs/Design/Requests/`(새 파일만), `Results/<트랙>/`(자기 결과), `Docs/Status/<트랙>_STATUS_KO.md`(자기 상태) |

- 소유 폴더 밖은 읽기만 한다. 다른 트랙의 공개 HLSL 헤더(5.6)는 `#include`해서 쓴다.
- 트랙 진입점 파일(`Native/Render/Passes/<폴더>/*Track.cpp`, `RayTracing/RayTracingTrack.cpp`)은 코어가 만든 빈 구현이고 해당 트랙이 채운다. 시그니처(`Native/Render/include/unx/render/Tracks.h`)는 바꾸지 않는다.
- 폴더 → 트랙 대응은 `cmake/Tracks.cmake`(코어)에 있다. 새 모듈·도구 폴더는 코어가 그 표와 이 절에 추가한다(없으면 구성이 실패한다).

## 2. 빌드 — 폴더 자동 등록

공유 파일(`CMakeLists.txt` 등)을 고치지 않고 파일만 추가하면 빌드에 들어간다(`cmake/Modules.cmake`).

### 2.1 렌더 모듈 (`Native/Render/Passes/<이름>`, `Native/Render/RayTracing`)

| 넣는 것 | 결과 |
|---|---|
| `*.cpp` (하위 폴더 포함, `Tests/`·`Gates/` 제외) | 정적 라이브러리 `unx_module_<이름소문자>` — `unx_render`, `unx_scene` 링크 |
| `include/` | 그 라이브러리의 공개 include 경로 |
| `module.cmake` (선택) | 라이브러리 생성 뒤 include됨. 변수 `UNX_MODULE_TARGET`, `UNX_MODULE_DIR`, `UNX_MODULE_NAME`. 추가 의존성·정의용 |
| `Tests/*.cpp` | 파일마다 실행 파일 `unx_test_<모듈>_<파일>` — 정확성 검사, GPU 잠금 없음 |
| `Gates/*.cpp` | 파일마다 실행 파일 `unx_gate_<모듈>_<파일>` — 성능 게이트, GPU 잠금 필수(3.3) |
| `*.hlsl` (첫 줄 `// unx-kernel: <프로필> <진입점>`, 선택 `// unx-variants: NAME=a,b`) | 빌드 때 DXC로 컴파일. **커널 이름 = `Native/Render` 기준 상대 경로에서 `.hlsl`을 뺀 것** + 변형 접미사. 예: `Passes/Shadow/VsmTaps`, `Passes/Shadow/VsmTaps.MODE1`. DXIL 200 KB 초과면 빌드 실패 |
| `*.hlsli` | include 전용. include 경로: `Native/Render/Passes/Common`, `Native/Render` (`#include "Passes/GI/GiCache.hlsli"`) |

- Tests·Gates 실행 파일은 렌더러 전체(`unx_renderer`: 모든 모듈 + `unx_frame`)를 링크하고 `UNX_SOURCE_DIR`가 정의된다.
- 모듈끼리 C++로 서로 링크하지 않는다. 트랙 간 호출은 `FrameServices`(5.3, 5.4)와 공개 HLSL 헤더(5.6)로만 한다.

### 2.2 도구와 C 트랙

- `Tools/<이름>/CMakeLists.txt`가 있으면 자동 추가된다(`Microbench`·`ShaderCompiler` 제외). `Reference/CMakeLists.txt`도 자동 추가된다. 이 파일들은 그 폴더 소유 트랙이 고친다.
- `Tools/SceneGen`: 라이브러리 `unx_scenegen`(소스가 `src/`에 생기면 정적 라이브러리, 없으면 헤더만).
- `Reference`: 라이브러리 `unx_metrics`, 도구 `unx_metrics`, 자가 검사 `unx_test_metrics`(코어가 만든 골격, 10.3).

### 2.3 외부 의존성

- 공식 배포본만, 버전·해시 고정: `External/Dependencies.cmake`(코어 소유). 현재: Agility SDK 1.618.5(NuGet, SHA-256), NVAPI R590(서브모듈), NVIDIA FLIP v1.7(서브모듈), meshoptimizer v1.2(서브모듈, `Unx::meshoptimizer`).
- 트랙이 새 라이브러리(Embree, meshoptimizer, D3D12MA, cgltf 등)가 필요하면 요청(0절)한다. 코어가 `External/`에 고정해 `Unx::<이름>` 타깃으로 내보내면 트랙은 자기 `module.cmake`·`CMakeLists.txt`에서 링크한다.

### 2.4 명령

```text
powershell -File Tools/CI/Build.ps1 -Track <core|M|S|R|C|I> [-Target <타깃>]      → build/<트랙>, 코어 + 그 트랙만 (2.5; I는 V;M;S;R;I)
powershell -File Tools/CI/Build.ps1 -Track all                                    → build/all, 모든 트랙(통합: 게이트 측정용)
build/<트랙>/bin/unx_unit_tests.exe                                               코어 단위 테스트(디버그 레이어)
build/<트랙>/bin/unx_gate_empty_frame.exe --validate                              debug layer + GPU-based validation
powershell -File Tools/CI/GpuLock.ps1 -Track <트랙> -- <성능 측정 명령>            성능 측정(3.3)
```

### 2.5 트랙 선택 빌드 (v1.1)
네 세션이 작업 트리 하나를 같이 쓰므로, 한 트랙의 작성 중 파일이 다른 트랙의 빌드를 깨면 안 된다.
- CMake 캐시 변수 `UNX_TRACKS`: `all` 또는 `V;M;S;R;C;I`의 부분집합(코어는 항상 켜짐). 꺼진 트랙의 렌더 모듈(`*.cpp`), 커널(`*.hlsl`), 도구(`Tools/<폴더>`, `Reference`), Tests·Gates 실행 파일은 구성하지 않는다. 꺼진 트랙의 진입점(`Tracks.h`)은 코어의 빈 구현(`Native/Render/Frame/Stubs/Track<트랙>.cpp`, 패스 없음, 로그 한 번)이 대신한다.
- `Build.ps1 -Track <이름>`이 기본 선택을 정한다: `core` → V(코어 세션, v1.2), `M` → M, `S` → S, `R` → R, `C` → C, `V` → V, `I` → V;M;S;R;I(호스트 DLL은 렌더러 전체를 링크, v1.6), `all` → 전부. I 폴더(`Native/Host`)는 I가 켜졌을 때만 최상위 `CMakeLists.txt`가 `add_subdirectory`한다(그 안의 `CMakeLists.txt`는 I 소유). 다른 조합은 `-Tracks "S;V"`처럼 준다.
- 트랙 세션의 개발·정확성 검사는 자기 선택 빌드(`build/<트랙>`)로 한다. **모든 트랙을 켜는 통합 빌드(`-Track all`, `build/all`)는 게이트 측정 때만** 쓰고, 통합 빌드가 실패하면 원인 파일의 소유 트랙이 고친다.
- 꺼진 트랙의 공개 HLSL 헤더(5.6)는 소스 트리에 있으면 include할 수 있다(커널만 컴파일하지 않는다). 다른 트랙 헤더는 그 트랙이 커밋한 뒤에 쓴다.

## 3. 병렬 작업 규칙

### 3.1 빌드 폴더
세션마다 자기 빌드 폴더 `build/<트랙>`만 쓴다(`Build.ps1 -Track`). 그 폴더는 코어 + 자기 트랙만 켜서 구성된다(2.5). 다른 세션의 빌드 폴더에서 실행 파일을 실행하거나 덮어쓰지 않는다. 실행 중인 실행 파일·로드된 DLL은 덮어쓰지 않는다. 통합 빌드 `build/all`은 게이트 측정 직전에 측정하는 세션이 만든다.

### 3.2 커밋
- `git add`는 **자기 소유 경로만 명시해서** 한다. `git add -A`, `git add .`, `git commit -a` 금지.
- **커밋도 경로를 명시한다**: `git commit -m "<메시지>" -- <자기 소유 경로들>`. 스테이징 영역은 네 세션이 같이 쓰므로, 경로 없는 `git commit`은 다른 세션이 스테이징해 둔 변경까지 커밋한다(v1.1).
- 커밋 전에 `git diff --cached --name-only`로 자기 경로만 들어갔는지 확인한다.
- `.git/index.lock` 때문에 실패하면 수 초 뒤 다시 시도한다(최대 1분). 다른 세션의 lock 파일을 지우지 않는다.
- 커밋 메시지 첫 줄 앞에 트랙을 붙인다: `[S] VSM page cache with dirty rules`.
- 다른 트랙의 파일이 작업 트리에서 바뀌어 있어도 되돌리거나 커밋하지 않는다.

### 3.3 GPU 잠금 (성능 측정만)
- 성능 측정(하네스 `Harness::run`, 마이크로벤치, 게이트, 타임스탬프 비교 실험)은 `Tools/CI/GpuLock.ps1 -Track <트랙> -- <명령>`으로만 실행한다. 잠금은 세션 전체에서 한 번에 하나다(이름 있는 mutex `Local\UnravelNext.GpuMeasurement`). 현재 보유자 `.gpulock/current.json`, 기록 `.gpulock/history.log`.
- 코드가 강제한다: `Harness::run`과 `requireGpuLock()`을 부르는 도구는 `UNX_GPU_LOCK`이 없으면 측정을 거부한다. 트랙의 게이트도 측정 전에 `unx::render::requireGpuLock("<게이트 이름>")`을 부른다.
- 정확성 실행(단위 테스트, 디버그 레이어·GPU 검증, 기준 영상 비교, 디버그 캡처)은 잠금 없이 동시에 해도 된다.
- 사용자의 다른 GPU 앱은 닫지 않는다. 측정은 4K·1440p만(하네스가 강제), 1.5초 워밍업, 중앙값·P95·P99.

### 3.4 결과·상태
- 게이트 결과: `Results/<트랙>/<게이트>/`(JSON 요약·로그는 커밋, 프레임별 CSV는 커밋하지 않음). 모든 보고에 해상도·품질 해시·빌드 identity·드라이버·큐 우선순위·GPU 잠금 보유자가 들어간다(하네스가 기록).
- 트랙 상태: `Docs/Status/<트랙>_STATUS_KO.md`. 실측/예상 표기 규칙은 설계서와 같다.

## 4. Render graph API (`Native/Render/include/unx/render/RenderGraph.h`)

```cpp
TextureRef RenderGraph::createTexture(const TextureDesc&);        // 트랜지언트: 수명 = 첫 사용 ~ 마지막 사용, 메모리 aliasing
BufferRef  RenderGraph::createBuffer(const BufferDesc&);          // size > 0, stride 0 = raw (ByteAddressBuffer)
TextureRef RenderGraph::importTexture(ID3D12Resource*, const TextureDesc&, D3D12_BARRIER_LAYOUT);  // 지속 자원; 프레임 시작·끝 레이아웃
BufferRef  RenderGraph::importBuffer(ID3D12Resource*, const BufferDesc&);
void RenderGraph::addPass(std::string_view name, QueueType, SetupFn setup, ExecuteFn execute);
//   setup(PassBuilder& b): b.use(ref, Use), b.keep()
//   execute(PassContext& c): c.cmd (ID3D12GraphicsCommandList7), c.srv/uav(ref) → bindless 인덱스, c.rtv/dsv/dsvReadOnly(ref),
//                            c.resource(ref), c.address(buffer), c.desc(texture), c.computeConstants/graphicsConstants(data, dwords),
//                            c.bindFrameConstants(ViewResources::frameConstants)
```

| `Use` | 쓰기 | sync | access | layout |
|---|---|---|---|---|
| SrvCompute | 아니오 | COMPUTE_SHADING | SHADER_RESOURCE | SHADER_RESOURCE |
| SrvGraphics | 아니오 | ALL_SHADING | SHADER_RESOURCE | SHADER_RESOURCE |
| UavCompute | 예(RMW) | COMPUTE_SHADING | UNORDERED_ACCESS | UNORDERED_ACCESS |
| UavComputeDisjoint | 예 | COMPUTE_SHADING | UNORDERED_ACCESS | UNORDERED_ACCESS |
| UavGraphics | 예(RMW) | ALL_SHADING | UNORDERED_ACCESS | UNORDERED_ACCESS |
| RenderTarget | 예 | RENDER_TARGET | RENDER_TARGET | RENDER_TARGET |
| DepthWrite / DepthRead | 예 / 아니오 | DEPTH_STENCIL | DEPTH_STENCIL_WRITE / _READ | 같음 |
| IndirectArgs | 아니오 | EXECUTE_INDIRECT | INDIRECT_ARGUMENT | — |
| CopySrc / CopyDst | 아니오 / 예 | COPY | COPY_SOURCE / COPY_DEST | 같음 |
| AccelerationStructureWrite | 예 | BUILD_RAYTRACING_ACCELERATION_STRUCTURE | RAYTRACING_ACCELERATION_STRUCTURE_WRITE | — (버퍼 전용) |
| AccelerationStructureRead | 아니오 | BUILD_RAYTRACING_ACCELERATION_STRUCTURE \| ALL_SHADING | RAYTRACING_ACCELERATION_STRUCTURE_READ | — |
| AccelerationStructureInput | 아니오 | BUILD_RAYTRACING_ACCELERATION_STRUCTURE | SHADER_RESOURCE | — |
| AccelerationStructureScratch | 예 | BUILD_RAYTRACING_ACCELERATION_STRUCTURE | UNORDERED_ACCESS | — (트랜지언트는 ALLOW_UNORDERED_ACCESS로 생성) |

- AS 사용(v1.1, R 요청)은 버퍼 전용이고 뷰를 만들지 않는다(텍스처에 쓰면 거부). AS 결과 버퍼는 소유 트랙이 `D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE`(+`ALLOW_UNORDERED_ACCESS`)로 만들어 `importBuffer`하고 AS SRV 서술자도 직접 만든다. 한 패스가 같은 AS 버퍼를 Write와 Read로 함께 선언할 수 있다(제자리 refit). 검증: `unx_unit_tests graph_acceleration_structure_uses` — GPU가 쓴 입력 → BLAS 빌드 → TLAS 빌드 → 인라인 광선을 한 프레임·그래프 배리어만으로 연결, 광선 거리 5.000000 = 기대값, 디버그 레이어 오류 0. 트랜지언트 입력 버퍼에서 디버그 레이어가 WARNING 926(같은 VA 범위의 aliased 자원)을 내는데, 그래프 aliasing의 정상 동작이다.
- DispatchRays·RayQuery가 읽고 쓰는 텍스처·버퍼는 `SrvGraphics`/`UavGraphics`로 선언한다. sync ALL_SHADING은 레이트레이싱 셰이더 단계를 포함한다(COMPUTE_SHADING은 포함하지 않는다). 컴퓨트 커널 안의 RayQuery가 읽는 TLAS는 `AccelerationStructureRead`(ALL_SHADING 포함)로 충분하다.

규칙:
1. 패스는 건드리는 자원을 **전부** `use`로 선언한다. 배리어는 그래프가 낸다(enhanced barriers, 실제 위험·레이아웃 변경에만). 패스 안에서 `Barrier`/`ResourceBarrier`/`ExecuteCommandLists`/큐 조작을 하지 않는다.
2. 한 패스가 같은 자원을 SRV와 UAV로 동시에 쓰면 거부된다(UAV끼리, DepthWrite+DepthRead는 허용).
3. `UavComputeDisjoint`: 같은 자원의 서로 겹치지 않는 영역에 쓰는 연속 패스(재질 클래스별 타일, 캐스케이드별 범위) 사이에 배리어를 두지 않는다. 겹치면 결과가 틀리므로 겹치지 않음을 보장할 때만 쓴다.
4. 큐: 기본 정책은 모든 패스를 그래픽스 큐 하나, 프레임당 커맨드 리스트 1개(설계서 4.3 실측: 큐 간 동기 62~79 µs). `QueueType::Compute`로 선언해도 그래픽스로 간다. 프레임 안 async compute는 쓰지 않는다.
5. 패스 이름: `<트랙>.<단계>` 소문자 (`s.vsm.mark`, `r.gi.trace`, `v.raster.bandA`, `m.shade.opaque`). 타임스탬프는 자동(패스 경계당 1개).
6. 루트 시그니처는 하나(`Device::rootSignature`): 루트 상수 32 DWORD(b0, `P[8]` uint4), 루트 CBV b1 = 뷰 프레임 상수(5.5), 정적 샘플러 s0 point-clamp, s1 linear-clamp, s2 linear-wrap, s3 aniso16-wrap, s4 comparison(GREATER_EQUAL). 서술자는 bindless(`ResourceDescriptorHeap[]`).
7. 파이프라인: `ShaderLibrary::compute("<커널 이름>")`, `ShaderLibrary::mesh("<이름>", MeshPipelineDesc)`. 파일당 커널 하나, 모드는 컴파일 변형으로.
8. 지속 자원(VSM 풀, GI 캐시, TLAS 등)은 소유 트랙이 `Device`로 만들어 매 프레임 `import`한다. 해제는 `Device::deferRelease`(GPU가 끝낸 뒤).

## 5. 프레임 구성 (`Frame.h`, `Tracks.h`, `FrameRenderer.h`)

### 5.1 뷰와 자원 게시판

`ViewDesc`: 종류(Main, PlanarReflection), 크기, view/proj/viewProj/prevViewProj/invViewProj(행 우선, 열 벡터), 위치, near, fov, 클립 평면(`dot(n,p)+w >= 0` 유지), `mirrored`, EV100.

`ViewResources` (뷰마다; [생산자] → 소비자):

| 필드 | 형식 | 생산 | 소비 |
|---|---|---|---|
| frameConstants | b1 CBV 주소 | 코어 | 모두 |
| depth | D32_FLOAT reversed-Z | V | M, S, R |
| visId | R32_UINT (7.1) | V | M, R |
| visibleClusters | `gpu::VisibleCluster[]` | V | M, R |
| hiz | R32_FLOAT 전 밉. 밉 k 텍셀 (i, j) = 픽셀 [i·2^(k+1), (i+1)·2^(k+1)) 블록의 가장 먼 깊이(reversed-Z 최솟값). 유효 크기 ⌈W/2^(k+1)⌉×⌈H/2^(k+1)⌉, 할당은 2의 거듭제곱(D3D 밉 크기는 내림이라 올림 체인을 담기 위함; 유효 영역 밖 텍셀은 정의되지 않음) | V | S(페이지 표시), R |
| coverageFragments / coverageHeads | 7.1 | V | M |
| gbuffer | RG32_UINT (7.2) | M | S, R |
| shadowVisibility | R32_UINT (7.3) | S | M |
| screenProbes | R 내부 형식, HLSL API로 읽음(5.6) | R | M |
| reflection | RGBA16F (5.6) | R | M |
| reflectionLobeTiles | R8_UNORM ⌈W/8⌉×⌈H/8⌉: 8×8 타일 안 표면 픽셀의 min `reflectionLobeHalfAngle(지각 거칠기, NoV)` / π (R의 `Reflection.hlsli`와 같은 식, 하늘만인 타일 = 1), 재질 해석의 부산물 (v1.3; v1.2의 roughnessTiles 대체) | M | R |
| color | 메인: 7.5 출력, 보조 뷰: RGBA16F 선형 | M | 호출자 / R |

`FrameResources` (프레임당): 대기 LUT 4개·VSM 풀·페이지 테이블·프록셀·프록셀 광원 리스트 [S], TLAS 정적/동적·GI 캐시 [R]. 소비자는 이 참조를 자기 패스에서 `use`하고 인덱스를 커널에 넘긴다.

### 5.2 트랙 진입점과 순서

`FrameRenderer::record`가 설계서 4.1 순서로 부른다(큐 하나, 순서는 선언 의존성으로만 의미가 있다):

```text
S atmosphere(fc)                    하늘·aerial LUT (태양·날씨 변경 시)
R accelerationStructures(fc)        정적/동적 TLAS, BLAS refit
V visibility(fc, main)              컬링 2단계, 대역 분류, 대역 A vis buffer, HiZ, coverage 층(B/C)
M materialResolve(fc, main)         G-buffer
S shadowPages(fc, main)             VSM 페이지 표시·할당·dirty 래스터 (FrameServices::rasterizeDepth)
S froxels(fc, main)                 광원 리스트 + 프록셀 적분
R globalIllumination(fc, main)      캐시 갱신 광선, 화면 프로브, 근거리 가림
R reflections(fc, main)             K/G/M 광선, 평면 반사(FrameServices::renderView)
S shadowVisibility(fc, main)        가시성 패스 → shadowVisibility
M shading(fc, main)                 셰이딩 커널, 가장자리·coverage 합성, 톤맵 → color
```

구현 전의 진입점은 `tracks::pending("<트랙>.<함수>")`만 부르고 패스를 내지 않는다(한 번 로그).

### 5.3 S → V: 깊이 래스터 서비스

`fc.services.rasterizeDepth(fc, DepthRasterRequest)`: V가 자기 클러스터 경로(컬링·LOD·변형·메시 셰이더)로 `instanceMask`에 맞는 인스턴스를 `views`마다 래스터한다. 출력은 요청자가 정한다:
- `depthTarget`에 하드웨어 depth(D32, GREATER_EQUAL, reversed-Z) — 뷰포트는 `RasterView::viewport*`.
- 또는 요청자의 픽셀 커널(`pixelKernel`, 예: 페이지 테이블 간접 + 원자적 깊이 쓰기)과 그 커널이 쓰는 자원(`textureUses`, `bufferUses`). 이때 렌더 타깃·깊이 없는 UAV 전용 래스터(1 표본)이고, 뷰포트는 16384²까지 된다. 루트 상수 0~15는 V, 16~31은 요청자(`pixelConstants`).
- **픽셀 커널 입력(v1.1)**: `struct DepthRasterPixel`(`Passes/Visibility/DepthRaster.hlsli`, V 소유) = `SV_Position`, `float2 uv : TEXCOORD0`, `nointerpolation uint userData : USERDATA`(RasterView::userData), `nointerpolation uint material : MATERIAL`(인스턴스 교체 적용된 재질), `nointerpolation uint instance : INSTANCE`(v1.4, 장면 인스턴스 번호). 커널은 무엇이든 쓰기 전에 `depthRasterCovered(p)`를 부르고 거짓이면 `discard`한다(alpha-tested 재질이 본 뷰·기준과 같은 모양으로 잘림).
- **LOD 척도**: `RasterView::lodPixelsPerMetre`. 원근 viewProj면 거리 1에서의 미터당 텍셀(초점 거리), 직교 viewProj(마지막 행 (0,0,0,1), V가 판별)면 거리와 무관한 미터당 텍셀(S는 1/τ_k).
- **컬 모드(v1.1)**: `DepthRasterRequest::cull`, 기본 `D3D12_CULL_MODE_NONE`(양면; 그림자). `BACK`이면 one-sided 재질만 뒷면을 버린다(two-sided 재질은 늘 양면).
- **타일 마스크 컬링(v1.1, S 요청)**: `DepthRasterRequest::cullMask`(raw 버퍼, 같은 프레임의 앞선 패스가 GPU로 씀)와 `cullTilePx`, 뷰마다 `RasterView::cullMaskOffset`(단어 위치, `UINT32_MAX` = 마스크 없음). 뷰마다 ⌈w/tile⌉×⌈h/tile⌉ 비트, 행 우선, 비트 i = 단어 `offset + (i >> 5)`의 `(i & 31)`번째, 1 = 래스터 필요. V는 뷰포트 투영 사각형이 켜진 비트를 하나도 덮지 않는 클러스터(가능하면 메시렛 삼각형)를 버리고, 필요하면 내부에서 계층 마스크를 만든다. **성능용 필터**이고 정확성은 요청자의 픽셀 커널이 지킨다(V가 보수적으로 더 그려도 결과는 같다). V는 `cullMask`를 `SrvCompute`/`SrvGraphics`로 선언한다. v1.7부터 V는 8×8 타일 요약 마스크를 만들어 인스턴스·계층 노드·클러스터 모두를 마스크로 정확히 거른다(이전의 "64 타일 넘는 사각형은 그린다" 생략 없음).
- **타일 국소 래스터(v1.7, S 요청 `20260925_S_page_local_raster.md`)**: `DepthRasterRequest::tileLocal = true`(마스크 필요). 클러스터마다 그 사각형 아래 켜진 타일의 행별 연속 구간마다 한 번(사각형 아래 타일이 모두 켜졌으면 한 번) 그리고, 메시 셰이더가 그 구간 사각형 밖 삼각형을 버리고 나머지를 클립 거리 4개로 잘라 래스터한다. 래스터라이저는 켜진 타일 안에서만 fragment를 만든다(fragment 수 = Σ 삼각형 ∩ 켜진 타일 면적). 픽셀 위치·`DepthRasterPixel`은 같고, 켜진 타일 안의 fragment 집합(픽셀별 개수)은 마스크만 쓴 래스터와 같다. **깊이**는 하드웨어가 타일 경계에서 만든 꼭짓점을 1/256 px 격자에 맞추므로 그 삼각형의 깊이 평면이 반올림 수준으로 다르다: 실측 최대 차이는 깊이 기울기 × 0.0068 px(측면 변위로 1/64 px 이내, V 테스트 `raster_service`). 비용식: 쌍 수 × 메시렛 launch + 삼각형 수 / 래스터 처리율 + Σ(삼각형 ∩ 켜진 타일 면적) fragment.

### 5.4 R → V·M·S: 평면 반사 뷰

1. R이 반사면(평면 `float4 plane`)과 그 화면 사각형(A_r)을 정해 `ViewDesc::planarReflection(main.view, plane, x, y, w, h)`로 뷰를 만든다: 카메라를 평면에 대칭, 투영을 사각형으로 잘라낸 비대칭 절두체, `clipPlane = plane`(원래 카메라 쪽 유지), `mirrored = true`.
2. `ViewResources v = fc.services.renderView(fc, view)`: 코어가 V `visibility` → M `materialResolve` → S `shadowVisibility` → M `shading`을 그 뷰에 기록한다. `v.color`는 RGBA16F 선형(광도 × 노출), 크기 w × h.
3. V는 클립 평면을 `SV_ClipDistance0`과 클러스터 컬링으로 지키고 `mirrored`면 컬링 면을 바꾼다. M은 `viewKind == VIEW_PLANAR_REFLECTION`이면 화면 프로브·반사 결과 대신 R의 GI 캐시 API(5.6)를 쓰고, 반사를 재귀하지 않는다(거친 반사는 캐시 K 경로).
4. 원본 기하 그대로(캐릭터 원본 메시, 바람 적용 잎, coverage 층)라 직접 시야와 같다(설계서 2.6). R은 `v.color`를 자기 반사 결과에 합성한다.

### 5.5 프레임 상수 (b1, `Frame.hlsli` ↔ `gpu::FrameConstants`, 528 B)

뷰 행렬 5개(row_major), 카메라 위치·near, 클립 평면, 뷰 크기·종류·프레임 번호, 시간·dt·노출(`1/(1.2·2^EV100)`)·tan(fov/2), 태양 방향·조도(lux, 대기 위)·색·각반지름, 바람 방향·속도, 장면 버퍼 bindless 인덱스(인스턴스, 메시, 서브메시, 정점, 인덱스, 클러스터, 클러스터 정점 인덱스, 클러스터 삼각형, LOD 레벨, 재질, 재질 리매핑, 광원, 스킨, 본 팔레트 현재/이전, 재질 모델 LUT, LOD 레벨 클러스터 목록 `g_lodLevelClusters`(v1.1, 예비 칸 사용, 크기 불변)), 개수, 장면 revision. 패스는 `c.bindFrameConstants(view.frameConstants)`로 묶는다.

**트랙 지속 상태(v1.1)**: `FramePassContext::state<T>("<트랙>.<이름>")`은 `FrameRenderer`가 소유한 저장소(`TrackState`)에서 T를 처음 쓸 때 만들고, 렌더러 파괴 때 GPU 유휴를 기다린 뒤 없앤다(히스토리 버퍼, 풀, 캐시). 키 하나에는 늘 같은 타입을 쓴다. 렌더러 없이 만든 컨텍스트에서는 실패한다. 트랙이 이미 쓰는 장치 키 레지스트리(`DeviceState.h`)는 그대로 둬도 되고, 수명 훅이 필요하면 이것으로 옮긴다.

### 5.5.1 V ↔ M 경계 (v1.2)
- **V가 낸다**: vis id·depth·visible clusters·HiZ(대역 A), coverage 층(대역 B/C) fragment 목록 — 픽셀별 깊이 순 정렬, fragment마다 정확 면적·32-부표본 마스크·vis id(7.1). V는 fragment를 셰이딩하지 않는다.
- **M이 한다**: 재질 해석(G-buffer), 셰이딩, 가장자리 픽셀(E: 3×3 identity 2개 이상) 검출과 그 픽셀의 해석적 coverage, coverage fragment 셰이딩, 깊이 순 합성, 톤맵 → `color`. E·coverage 합성은 셰이딩 뒤 최종 합성으로 M의 `shading` 진입점 안에 있다.
- **공유 기하 함수**: 삼각형∩픽셀 사각형의 정확 면적과 32-부표본 마스크는 V의 `Passes/Visibility/Coverage.hlsli`(V 소유, 대역 B 래스터와 M의 E 합성이 같은 함수를 쓴다)에 둔다. 시그니처는 V가 대역 B를 구현할 때 이 절에 적는다.
- M은 V의 vis buffer가 나오기 전까지 자기 테스트의 가짜 vis buffer(같은 형식, 7.1)로 개발한다. V는 출력이 준비되면 알린다.

### 5.6 공개 HLSL API (고정된 이름·시그니처)

코어 (`Passes/Common/`, 코어 소유): `Bindless.hlsli`(P[8], 샘플러), `Frame.hlsli`, `Scene.hlsli`(레코드와 `loadInstance/Mesh/Submesh/Cluster/LodLevel/Material/Light`, `loadVertex(mesh, v)`, `loadTriangle(mesh, t)`, `loadClusterTriangle(c, t)`, `octEncode/octDecode`, `transformPoint/Vector`), `VisBuffer.hlsli`, `GBuffer.hlsli`, `Deformation.hlsli`(`deformVertex(inst, mesh, v)`), `MaterialModel.hlsli`(`modelEvaluate` 등, 8.1).

각 트랙이 자기 폴더에 만들어야 하는 헤더 (소비자가 이 이름으로 include한다; 인덱스 묶음 struct는 소비자가 `c.srv(...)`로 채워 루트 상수로 넘긴다):

| 헤더 | 제공 | 필수 함수 |
|---|---|---|
| `Passes/Atmosphere/Atmosphere.hlsli` | S | `struct AtmosphereSrvs { uint transmittance, multiScatter, skyView, aerial; };` `float3 atmosphereSkyRadiance(AtmosphereSrvs, float3 worldDir)`(태양 원반 제외) · `float3 atmosphereSunRadiance(AtmosphereSrvs, float3 worldPos)`(원반 복사휘도, 투과 포함) · `void atmosphereAerial(AtmosphereSrvs, float2 uv, float linearDepth, out float3 inscatter, out float3 transmittance)` |
| `Passes/Atmosphere/Froxel.hlsli` | S | `struct FroxelSrvs { uint lights, lightIndices, scattering, pad; };` `uint2 froxelLightRange(FroxelSrvs, uint2 pixel, float linearDepth)`(offset, count) · `uint froxelLight(FroxelSrvs, uint i)` · `float4 froxelScattering(FroxelSrvs, float2 uv, float linearDepth)`(inscatter rgb, 투과 a) |
| `Passes/Shadow/ShadowVisibility.hlsli` | S | `float shadowSlot(uint packed, uint slot)`(7.3 해독) · `uint shadowSlotOfLight(FroxelSrvs, uint2 pixel, float linearDepth, uint lightIndex)`(1~3 또는 0xFFFFFFFF) · `struct ShadowSrvs { uint pool, pageTable, lights, pad; };` `float shadowVisibilityDirect(ShadowSrvs, uint lightIndex, float3 worldPos, float3 normal)`(슬롯 초과 광원용 느린 경로) |
| `Passes/GI/GiCache.hlsli` | R | `struct GiSrvs { uint cache, hash, pad0, pad1; };` `float3 giCacheIrradiance(GiSrvs, float3 worldPos, float3 normal)` · `float3 giCacheRadiance(GiSrvs, float3 worldPos, float3 dir, float coneHalfAngle)` |
| `Passes/GI/ScreenProbes.hlsli` | R | `struct ProbeSrvs { uint probes, occlusion, pad0, pad1; };` `float4 screenProbeIrradiance(ProbeSrvs, uint2 pixel, float3 normal, float linearDepth)`(rgb 조도, a 근거리 가림 0~1) · `float3 screenProbeRadiance(ProbeSrvs, uint2 pixel, float3 normal, float linearDepth, float3 dir, float coneHalfAngle)`(v1.2: 4프로브 보간, 반각 coneHalfAngle 원뿔로 prefilter한 입사 복사휘도, nit) |
| `Passes/Reflection/Reflection.hlsli` | R | `float4 reflectionRadiance(uint reflectionSrv, uint2 pixel)`: rgb = 반사 lobe로 정규화 적분한 입사 복사휘도(G/M 경로 결과, M이 방향 알베도 항을 곱한다), a = 1이면 유효 · `float reflectionLobeHalfAngle(float perceptualRoughness, float NoV)`(v1.2: 설계서 2.6의 θ_r,narrow, M과 R이 K/G/M을 같은 식으로 가른다). **K 경로(v1.2)**: `a == 0`인 본 뷰 픽셀은 M의 셰이딩 커널이 `screenProbeRadiance(probes, pixel, n, depth, reflect(-v, n), reflectionLobeHalfAngle(r, NoV))`에 재질 모델의 스페큘러 방향 알베도 항을 곱해 평가한다. 보조 뷰(평면 반사)는 `giCacheRadiance` |

## 6. 장면 데이터

### 6.1 규약 (`Native/Core/include/unx/core/Math.h`)
오른손 좌표, +Y 위, 미터. 뷰 공간은 −Z를 본다. 행렬은 행 우선 저장, 열 벡터(`p' = M p`, HLSL `row_major` + `mul(M, v)`). 투영은 reversed-Z 무한 원평면(깊이 = near / 거리, 지우기 0, `GREATER_EQUAL`). 인스턴스 변환은 회전·균일 스케일·평행이동만(`scene::validate`가 검사; 법선은 같은 행렬로 변환). 반시계가 앞면.

### 6.2 CPU 장면 (`Native/Scene/include/unx/scene/SceneData.h`, 파일 `.unxscene` v1)
텍스처(mip 0, 형식 6종), 재질(클래스, baseColor, roughness(지각), metallic, specular, emissive(nit), alphaCutoff, transmission, ior, twoSided, 텍스처 5종), 메시(위치·법선·탄젠트(노멀맵이면 필수)·uv0·인덱스·서브메시·스킨 스트림), 인스턴스(메시, 변환, 플래그 CastShadow/Dynamic/Skinned/Wind, 스켈레톤, 바람, 서브메시별 재질 교체), 스켈레톤 포즈, 광원 6종, 태양, 대기(이전 엔진 수식·기본값), 바람, 카메라, 카메라 경로. `serialize`는 결정적이고 `contentHash`(SHA-256)가 장면 identity다. `validate`가 구조 규칙을 검사한다.

### 6.3 GPU 장면 (`GpuScene.h`, `GpuSceneLayout.h` ↔ `Scene.hlsli`)
`GpuScene::upload(scene)`가 레코드를 채운다(코어 소유). 크기 고정: Instance 144 B, Mesh 80 B, Submesh 16 B, Vertex 32 B(v1 비압축; `loadVertex`로만 읽으므로 V/M이 압축해도 인터페이스 변경 아님), SkinVertex 16 B, Cluster 64 B, LodLevel 16 B, Material 80 B, Light 80 B, VisibleCluster 8 B. 클러스터 계층은 V의 빌더(`Tools/ClusterBuilder`)가 `GpuScene::setClusters(ClusterData)`로 넣는다. `GpuScene::clusters()`는 그 CPU 사본(메시별 `clusterOffset/Count`, `lodLevelOffset/Count`, `lodLevelClusters`), `GpuScene::srv(name)`은 클러스터 버퍼와 V 내부 버퍼(`ClusterData::named`)의 bindless SRV다(v1.1). R은 `GpuScene::buffer("vertices" | "indices" | "clusters" | "clusterVertexIndices" | "clusterTriangles" | "lodLevels" | "lodLevelClusters" | ...)`와 메시의 원본 삼각형(`indexOffset`, `triangleCount`) 또는 LOD 레벨의 클러스터로 BLAS를 만든다.

**프레임 갱신(v1.8, I 요청)**: 호스트는 렌더 프레임 f를 기록하기 전에 `updateTransforms(f, {instance, objectToWorld}[])`, `updateSkeleton(f, skeleton, jointToModel[])`, `setInstanceVisible(instance, bool)`을 부른다. `FrameRenderer::record`가 맨 처음 `flushUpdates`로 반영한다. 반영은 그래픽 큐의 산포 커널(`Passes/Common/SceneUpdate`)이 프레임 그래프보다 먼저 하고, 다른 큐는 그 fence를 기다린다. 뜻:
- `prevObjectToWorld` = **직전 렌더 프레임**의 objectToWorld, 이전 본 팔레트 = 직전 렌더 프레임의 팔레트. 이번 프레임에 안 바뀐 인스턴스 중 직전 프레임에 바뀐 것은 prev = current로 맞춘다(멈추면 움직임 0).
- `transformRevision`, `deformRevision`은 바뀐 프레임마다 1씩 오른다(한 프레임에 여러 번 갱신해도 1).
- 숨김: `Instance::flags`의 `gpu::kInstanceHidden`(1 << 31, HLSL `INSTANCE_HIDDEN`). 모든 소비자가 건너뛴다. V는 인스턴스 컬링에서 건너뛰고(주 뷰, 서비스), R은 TLAS에서 뺀다(R이 반영).
- CPU 사본(`instances()`)은 호출 즉시 바뀐다. `revision()`(장면 구조)은 바뀌지 않는다.
- 비용 [예상]: 갱신 원소(16 B) 수 × 2(업로드 링 + 산포). 인스턴스 2만 개와 본 2.56만 개면 약 2.2 MB, 0.01 ms 수준.

### 6.4 변형
스킨: 선형 블렌드, 조인트 4개, 팔레트 = jointToModel × inverseBind(인스턴스마다, 현재·이전 두 벌). 바람: `windOffset(inst, p, time)`(v1 모델, P3에서 같은 시그니처로 교체), 상한 `windOffsetBound(inst, centre, radius)`(구 안 모든 점·모든 시각), 변화 인자 `windChangeFactor(t0, t1)`(v1.4: 장면 바람과 인스턴스 변환이 그대로일 때 |windOffset(t1) − windOffset(t0)| ≤ windOffsetBound × windChangeFactor; 모델이 바뀌면 세 함수를 같이 바꾼다). **래스터(V), 그림자 페이지(V 서비스), BLAS refit(R)은 모두 `deformVertex`를 쓴다.** Revision: `Instance.transformRevision`, `deformRevision`, `Material.revision`, `Light.revision`, `FrameConstants.sceneRevision` — S의 dirty 규칙과 R의 캐시 무효화가 이것을 읽는다.

### 6.5 클러스터 (V 소유, 형식 고정)
64 삼각형(최대 128), 정점 ≤ 255, 클러스터마다 경계 구, 법선 원뿔, LOD 오차·부모 오차(단조), **최소 특징 폭**(물체 공간 m, 대역 A/B/C 분류), 대역 C 브릭 인덱스. 한 클러스터는 서브메시 하나에만 속한다.

v1.1 세부(헤더 `GpuSceneLayout.h`가 권위):
- `counts` = 정점 수 | 삼각형 수 << 8 | **메시 안 서브메시 번호 << 16**(v1의 lodLevel·flags 대신). 인스턴스 재질 교체까지 적용한 재질은 `clusterMaterial(inst, cluster)`(Scene.hlsli), 서브메시 번호는 `clusterSubmesh(c)`. `clusterLodLevel`은 없어졌다(DAG 깊이는 V 내부).
- `normalCone.w` = meshoptimizer `cone_cutoff`(법선 퍼짐 반각의 sin). 점 p에서 모든 삼각형이 뒷면 ⇔ `dot(c − p, axis) ≥ w·|c − p| + radius`. w ≥ 1이면 원뿔 없음.
- `lodError` = 이 클러스터 기하의 오차(원본 0), `parentLodError` = 이것을 대체하는 그룹의 오차(FLT_MAX = 대체되지 않음). 선택 규칙과 구(sphere)는 V 내부(`ClusterHierarchy.h`).
- `minFeatureWidth` > 0: 입체 폭(투영 폭이 시점 방향과 무관, 관·가지·머리카락), < 0: 폭 |w|의 평판(잎·풀잎; 투영 폭이 법선 원뿔과 시선의 |cos|만큼 준다).
- `LodLevel` = 메시 계층의 균일 오차 절단 하나: `clusterOffset/Count`는 `lodLevelClusters`(클러스터 번호 목록)의 구간, `error` = 절단 오차(0 = 원본), `triangleCount`. 메시의 레벨 수는 `ClusterData::MeshRange::lodLevelCount`.

## 7. 화면 버퍼 배치

### 7.1 vis id · depth · coverage 층 (`VisBuffer.hlsli`)
- vis id R32_UINT = `(visibleCluster << 7 | triangle) + 1`, `VIS_NONE = 0` = 하늘(v1.5). visibleCluster는 그 뷰의 `VisibleCluster` 목록 인덱스(< 2^25 − 1). 소비자는 `packVisId`·`visVisibleCluster`·`visTriangle`·`VIS_NONE`(VisBuffer.hlsli)만 쓰고 비트 연산을 직접 하지 않는다. 0인 이유: UINT 렌더 타깃은 float 클리어 값만 받으므로 0xFFFFFFFF(float로 표현 불가, 실측으로 0이 된다)로 지울 수 없고, 지우기 패스를 따로 두면 대상 전체를 한 번 더 쓴다(4K 33 MB).
- depth D32_FLOAT, reversed-Z, 무한 원평면.
- coverage 층: `coverageHeads` R32_UINT = 첫 fragment 인덱스(하위 24 bit) | 개수 << 24, `coverageFragments`는 픽셀별 깊이 순 정렬된 16 B `{ uint visId; float depth; uint coverageMask32; float area; }`. 대역 C 브릭 fragment는 visId의 삼각형 필드가 `0x7F`.

### 7.2 G-buffer (`GBuffer.hlsli`) — RG32_UINT 8 B
`.x` 월드 셰이딩 법선(팔면체 snorm16×2), `.y` baseColor sRGB8×3 | 지각 거칠기 unorm8(상위 8 bit). metallic·specular·클래스·플래그는 재질 테이블(vis id → 클러스터 → 재질). 픽셀별 metallic/occlusion 맵은 M의 셰이딩 커널이 vis id로 다시 평가한다.

### 7.3 그림자 가시성 (S) — R32_UINT
4 슬롯 × 8 bit unorm(0 = 완전 그림자, 255 = 완전 빛). 슬롯 0 = 태양, 슬롯 1~3 = 그 픽셀 프록셀 광원 리스트 순서에서 그림자를 던지는 첫 세 국소광. 넷째 이후 그림자 광원은 `shadowVisibilityDirect`(5.6)로 평가하고, 발생 수를 S가 통계로 낸다.

### 7.4 프록셀 광원 리스트 (S)
프록셀(24 px × 64 깊이 슬라이스) 당 광원 인덱스 목록, 최대 `atmosphere.froxels.lights_max`개. 초과 시 중요도 상위 `shading.analytic_lights_max`개를 해석 평가하고 나머지는 프록셀 조도로 합친다(합친 에너지를 통계로 기록). 소비자는 `froxelLightRange/froxelLight`로만 읽는다.

### 7.5 출력
- 메인 뷰(표시): RGB10A2_UNORM, 값 = sRGB OETF(PBR Neutral 톤맵(광도 × 노출)).
- 검증 실행(`FrameContext::outputLinearHdr`): RGBA32_FLOAT, 광도(nit) × 노출, 톤맵 전. 기준 영상과 같은 양(10.2).

## 8. 재질·광원·하늘 모델 (기준 경로추적기와 실시간이 같게 구현)

### 8.1 재질 v1 (`Native/Scene/include/unx/scene/MaterialModel.h` 권위, `MaterialModel.hlsli` 미러)
- α = max(r², 1e-4), f0 = lerp(0.08·specular, baseColor, metallic).
- D = GGX, V = Smith 높이 상관, F = Schlick. D는 소거 없는 형태 `α² / (π (|n×h|² + α² (n·h)²)²)`로 평가한다(v1.4: `(n·h)²(α²−1)+1`은 α = 1e-4에서 float로 0이 되어 거울 반사 방향에서 ∞). 다중 산란 보정 f_s = D V F · (1 + f0 (1/E(μ, r) − 1)) (Turquin 2019). E는 32×32 격자(끝점 포함, μ=0은 1e-4에서 평가) 가시 법선 표본 4096개로 만든 방향 알베도 표, 쌍선형.
- f_d = (1 − metallic) baseColor / π (Lambert). f = f_d + f_s (n·l > 0, n·v > 0).
- Foliage: 앞면 f_d × (1 − transmission), 뒷면으로 가는 빛 transmission × (1 − metallic) baseColor / π.
- two-sided: 뒷면에서 법선을 뒤집는다. alpha test: baseColor 텍스처 alpha ≥ alphaCutoff면 불투명(광선 any-hit도 같다).
- 노멀맵: `n_ts = (2r−1, 2g−1, √(1−x²−y²))`, TBN = (정점 보간 탄젠트, sign·cross(n, t), 정점 보간 법선), 결과 정규화. 탄젠트는 장면 데이터에 있는 값만 쓴다(재계산 금지).
- 텍스처: 기준은 mip 0 쌍선형(표본 수로 픽셀 필터를 적분), 실시간은 footprint 밉·이방성 + 노멀→거칠기 필터(설계서 2.2). 이 차이는 품질 정의(설계서 3절 "재질")의 허용 항목이다.
- Hair, Water, Glass, Subsurface 클래스 모델은 해당 단계(P3/P4) 전에 0절 절차로 이 절에 추가한다.

### 8.2 광원
- 점: 조도 = I / d² · w(d), w(d) = saturate(1 − (d/range)⁴)². 스폿: × saturate(cosθ · spotScale + spotOffset)², cosθ = dot(−l, forward).
- 면광원(rect/disk: +forward 한쪽 방출, sphere, tube): 표면 위 균일 휘도 L(nit), 기여 = ∫ f L cos dω × w(광원 중심 거리). 기준은 정확 적분(표본), 실시간은 LTC(설계서 3절: LTC 적합 오차는 품질 정의에 기록).
- 광원 색 `color`는 휘도 정규화 틴트, 세기는 `intensity`.

### 8.3 태양·하늘·대기
- 대기는 모든 경로 구간에 있는 참여 매질이다(설계서 2.3의 물리 모델, 이전 엔진 기본값: Rayleigh, Mie(g 0.8), 오존 텐트, 지면 알베도 0.1, 행성 반지름 6360 km, 대기 상한 6460 km, 원점은 지표면 위). 구름·밤하늘은 v1에 없다.
- 태양: 각반지름 θ_s 균일 원반. 원반 복사휘도 = E_TOA · T(p→태양) · color / (π sin²θ_s).
- 구현이 근사하는 구간(예: 실시간 2차 광선 구간의 대기)은 트랙 상태 문서에 적고 기준 대비 오차로 판정한다.

### 8.4 카메라·노출·톤맵
핀홀(피사계 심도·모션 블러 없음, v1). 픽셀 필터 = 픽셀 사각형 박스. 노출 = 1 / (1.2 · 2^EV100). 톤맵 = Khronos PBR Neutral(`shading.tonemap`), 이어서 sRGB OETF.

## 9. 품질 설정 키 (`Config/quality/<이름>.toml`)
- 파일 `<이름>.toml`은 `<이름>.`으로 시작하는 키만 가질 수 있다(`QualityConfig::loadDirectory`가 강제). 파일 소유는 1절 표.
- 코드는 품질 키에 기본값을 두지 않는다(없으면 오류). 모든 보고는 병합된 키 집합의 SHA-256을 남긴다. 실험용 덮어쓰기(`applyOverride`)도 해시에 들어간다.
- 다른 트랙이 읽는 키(이 목록 밖의 키를 다른 트랙이 읽으려면 0절 요청): `output.resolutions`(코어), `atmosphere.froxels.tile_px`, `atmosphere.froxels.depth_slices`, `atmosphere.froxels.lights_max`(S → M), `shading.analytic_lights_max`(M → S), `gi.screen_probe_spacing_px`(R → M), `shadow.vsm.page_texels`(S → V 래스터 LOD), `visibility.cluster_triangles`(V → R).

## 10. C 트랙 API

### 10.1 테스트 장면 생성기 (`Tools/SceneGen/include/unx/scenegen/SceneGen.h`)
`scene::Scene generate(const Request&)`, `allScenes()`, `sceneName(id)`. 장면: CityBlock, ForestThin(잎 6 cm·풀잎 4 mm), ForestCard(잎 35 cm·풀 카드 30 cm), Waterside(잔잔한 물·파도), Interior(거울·광택 바닥·면광원), CityNight(광원 512·그림자 128). 결정적(같은 요청 = 같은 `contentHash`), `validate` 통과, 카메라 하나 이상과 정지·이동 카메라 경로 포함. `scale`은 게이트 부하 배율(1 = 그 장면의 게이트 정의, 예: P1 장면 ≥ 1천만 삼각형).

### 10.2 기준 경로추적기 (C, `Reference/`)
- 입력: `.unxscene`(또는 `generate` 결과), 카메라 이름 또는 경로 시각, 해상도(4K·1440p), 표본 수(`reference.samples_per_pixel`, ≥ 4096).
- 모델: 8절 전체(재질, 광원, 태양·하늘·대기, 픽셀 박스 필터, 노출). 편향 없는 경로 추적(러시안 룰렛은 `reference.russian_roulette_start_bounce`부터).
- 출력: PFM, 광도(nit) × 노출, 톤맵 전(7.5 검증 출력과 같은 양). 수렴 확인용으로 독립 절반 두 장의 relMSE를 함께 낸다.
- 캐시: `Cache/Reference/<scene>/<camera>_<W>x<H>_<spp>_<sceneHash16>_<qualityHash16>.pfm` + 같은 이름 `.json`(입력 identity, 시간, 절반 간 relMSE). 캐시는 커밋하지 않는다.

### 10.3 지표 (`Reference/Metrics`, 코어가 만든 골격)
- 구현됨: PFM 읽기·쓰기, relMSE `mean((t−r)²/(r²+0.01))`, HDR-FLIP·LDR-FLIP(NVIDIA FLIP v1.7) 평균·P99와 오차 맵, 시간 불안정도(정지 카메라 연속 프레임 `mean|ΔY| / mean Y`). 도구 `unx_metrics --reference r.pfm --test t.pfm [--ldr] [--out report.json] [--error-map e.pfm]`, `unx_metrics --temporal f0.pfm f1.pfm ...`. 자가 검사 `unx_test_metrics`.
- C가 추가: 16-부표본 identity 인구조사(설계서 1.2, 7.2 게이트: "3×3이 놓친 부표본" 픽셀 비율; 입력은 엔진의 vis id 캡처와 기준의 부표본 identity), 장면별 임계값(설계서 3절: FLIP 평균·P99, relMSE, 시간 안정성 — `Config/quality/reference.toml`에 장면별 키로).

## 11. 게이트·보고
- 하네스(`Harness::run`) JSON: 라벨, 해상도, 품질 SHA-256·파일, 빌드(commit, dirty, diff SHA-256), 어댑터·드라이버·D3D12Core·Agility·NVAPI, GPU 잠금 보유자, 큐 우선순위, 워밍업·프레임 수, GPU 프레임 중앙값·P95·P99, CPU 선언·기록·제출, SM 클럭, 그래프 통계, 패스별 분포.
- 게이트 판정은 설계서 7절의 값과 비용식을 기준으로 한다. 실측이 크면 설계 가정 오류인지 구현 비효율인지 먼저 가른다. 품질·표본·부하는 낮추지 않는다.

## 12. 버전 기록
- v1 (2026-09-25): 최초 고정. 코어 커밋 이후 트랙 세션 시작.
- v1.1 (2026-09-25):
  - **R 요청 `20260925_R_acceleration_structure_uses.md` 반영**: `Use::AccelerationStructureWrite/Read/Input/Scratch`(4절 표). DispatchRays 자원은 `SrvGraphics`/`UavGraphics`(ALL_SHADING이 레이트레이싱 포함). 단위 테스트 `graph_acceleration_structure_uses`.
  - **S 요청 `20260925_S_depth_raster_page_mask.md` 반영(인터페이스)**: `RasterView::cullMaskOffset`, `DepthRasterRequest::cullMask/cullTilePx/cull`, 픽셀 커널 입력 `DepthRasterPixel` + `depthRasterCovered`(`Passes/Visibility/DepthRaster.hlsli`), 직교 뷰 LOD 척도, UAV 전용 16384² 래스터 명시(5.3). V의 서비스 구현(컬링에 마스크 검사 포함)은 V 진행에 따라 들어가며, 그 전까지 `rasterizeDepth`는 빈 구현이다.
  - **트랙 선택 빌드**(2.5, 3.1): `UNX_TRACKS`, `Build.ps1 -Track`이 코어 + 자기 트랙만 구성, 꺼진 트랙은 코어 빈 진입점. 통합 빌드 `-Track all`은 게이트 측정용.
  - **커밋 형식**(3.2): `git commit -m "..." -- <자기 경로들>`.
  - **트랙 지속 상태**(5.5): `FramePassContext::state<T>(key)`, `TrackState`(S 상태 문서의 수명 훅 요청).
  - **클러스터 세부**(6.3, 6.5): `counts`의 서브메시 번호, `clusterMaterial`, `normalCone.w` 정의, `minFeatureWidth` 부호, `LodLevel`의 `lodLevelClusters`, `GpuScene::clusters()/srv()`, `FrameConstants`의 `lodLevelClusters`(예비 칸, 528 B 유지).
  - **C 요청 `20260925_C_wind_direction.md` 반영**: `windOffset`의 월드→물체 바람 방향을 전치(`Mᵀ·d`)로 고침(이전 식은 `M·d`라 yaw θ인 수목이 2θ 돌아간 방향으로 흔들렸다). `windOffsetBound` 추가(컬링용 상한).
  - 외부 의존성: meshoptimizer v1.2 (`Unx::meshoptimizer`, V의 클러스터 빌더). C 요청 `20260925_C_embree.md`는 C가 자기 폴더에서 같은 URL·해시로 임시 처리 중이며 코어 반영은 대기(급하지 않음, 조율 세션 전달).
- v1.2 (2026-09-25):
  - **M 트랙을 새 M 세션으로 분리**(사용자 지시): 1절 표의 M 세션, 소유 `Native/Render/Passes/Material/`, `Native/Render/Passes/Shading/`, `Config/quality/material.toml`, `Config/quality/shading.toml`. 코어 세션은 코어 + V. M 폴더에는 코어가 만든 빈 진입점(`MaterialTrack.cpp`, `ShadingTrack.cpp`)만 있다.
  - **V ↔ M 경계**(5.5.1): V는 vis buffer와 coverage fragment 목록까지, M은 재질·셰이딩·E 검출과 해석 coverage·fragment 셰이딩·합성·톤맵. 공유 coverage 함수는 V의 `Coverage.hlsli`.
  - **R 요청 `20260925_R_reflection_k_path.md` 반영**: `ViewResources::roughnessTiles`(생산 M, 소비 R), R의 `screenProbeRadiance`·`reflectionLobeHalfAngle`, M 셰이딩의 K 경로 규칙(5.6 표).
- v1.3 (2026-09-25):
  - **R 요청 `20260925_R_reflection_tiles_quantity.md` 반영**: M → R 타일 값을 최소 거칠기에서 **최소 좁은 로브 반각** `reflectionLobeHalfAngle(r, NoV) / π`로 바꾸고, 양이 바뀌었으므로 이름을 `ViewResources::reflectionLobeTiles`로 바꿨다. 스침각(cos θ_o < 0.18)에서는 거칠기와 무관하게 K가 아니므로 최소 거칠기로는 R이 광선을 생략할 타일을 알 수 없었다. M은 구현 전이다(조율 세션이 결정 뒤 구현하도록 전달).
- v1.4 (2026-09-25):
  - **S 요청 `20260925_S_raster_instance_and_wind_change.md` 반영**: `DepthRasterPixel::instance : INSTANCE`(V의 서비스 메시 셰이더가 낸다), `windChangeFactor(t0, t1)`(`Deformation.hlsli`, 6.4). 페이지 쪽 바람 dirty 판정(페이지당 O(1))에 쓴다.
  - **C 요청 `20260925_C_ggx_precision.md` 반영**: GGX D를 `|n×h|²`로 쓰는 소거 없는 형태로 바꿨다(C++ `distributionGgx(NoH, sinSqNH, alpha)`, HLSL `modelD(NoH, sinSqNH, alpha)`; 8.1). 거칠기 0 거울의 반사 방향에서 D가 유한(1/(πα²))하다.
  - 코어 추가: `FramePassContext::framesInFlight`(트랙이 CPU로 쓰는 프레임별 자원의 슬롯 수), `MeshPipelineDesc::frontCounterClockwise`(미러 뷰의 앞면 방향), `VisBuffer.hlsli`의 `packVisId` 인자 이름(`triangle`은 메시 셰이더 예약어).
- v1.5 (2026-09-25):
  - **vis id 인코딩**(7.1): `VIS_NONE = 0`, vis id = `(visibleCluster << 7 | triangle) + 1`. V의 vis buffer 렌더 타깃을 0으로 fast clear한다(0xFFFFFFFF는 float 클리어 값으로 표현되지 않아 실측으로 0이 되었다). 접근자(`packVisId`, `visVisibleCluster`, `visTriangle`)를 쓰는 코드는 바뀌지 않는다. M 테스트의 가짜 vis buffer처럼 비트를 직접 조합하는 코드는 `packVisId`로 바꾸고 `VIS_NONE`(0)으로 지운다.
- v1.6 (2026-09-25):
  - **I 요청 `20260925_I_host_module.md` A 반영**: I(통합) 트랙 등록. 1절 표의 I 행(소유 `Native/Host/`, `Config/quality/host.toml`), `cmake/Tracks.cmake`의 `I`와 `Host → I`, 최상위 `CMakeLists.txt`가 I가 켜졌고 `Native/Host/CMakeLists.txt`가 있을 때 `add_subdirectory(Native/Host)`(도구 폴더 뒤), `Build.ps1 -Track I` → `build/I`, 트랙 `V;M;S;R;I`(2.5).
  - **`GpuInstance::prevObjectToWorld`의 뜻을 "직전 렌더 프레임의 objectToWorld"로 고정**(I 요청 B의 첫 항목). V의 1단계 HiZ 판정(직전 `viewProj`의 HiZ에 투영)과 S의 페이지 무효화가 이 뜻을 전제한다. 프레임 갱신 API(변환·본 팔레트·숨김)는 다음 판에서.
- v1.7 (2026-09-25):
  - **S 요청 `20260925_S_page_local_raster.md` 반영**: `DepthRasterRequest::tileLocal`(5.3). V 컬링이 (클러스터, 켜진 타일 행 구간) 쌍을 GPU에서 만들고, 서비스 메시 셰이더가 쌍의 사각형 밖 삼각형을 버리고 클립 거리 4개로 자른다. 타일 마스크는 8×8 타일 요약으로 인스턴스·노드·클러스터 단계에서 정확히 거른다. `visibility::Stats::tilePairs`. 실측(city_block 4K, S 클립맵 설정: 16384² 직교 뷰 12개, dirty 페이지 6개, V 게이트 `--service`): 마스크만 쓴 래스터 17.01 ms → 타일 국소 래스터 0.013 ms(+ 컬링 0.156 ms). 요약 마스크 전에는 가시 클러스터가 4,446개(삼각형 16.9만), 뒤에는 266개(삼각형 6,238)다.
- v1.8 (2026-09-25):
  - **I 요청 `20260925_I_host_module.md` B 반영**: `GpuScene::updateTransforms/updateSkeleton/setInstanceVisible`, `FrameRenderer`가 부르는 `flushUpdates`(6.3). 숨김 플래그 `gpu::kInstanceHidden` / `INSTANCE_HIDDEN`(V 인스턴스 컬링 반영, R은 TLAS에서 뺀다). 단위 테스트 `gpu_scene_frame_updates`(GPU 판독: 직전 프레임 규칙, 정착, 리비전, 숨김, 팔레트).
  - 코어 수정: 렌더 그래프가 재사용한 트랜지언트에 새 사용이 필요로 하는 뷰를 만든다(S 신고, 단위 테스트 `graph_views_of_reused_transients`). 버퍼 stride도 재사용 키에 넣었다.
