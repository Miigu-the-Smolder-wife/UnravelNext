# UnravelNext 인터페이스 (v1.84, 2026-10-01)

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
| FX GPU 시뮬레이션 | FX 세션 (v1.24) | `Native/Render/Passes/FX/`, `Config/quality/fx.toml` |
| E 엔진 2 (연기·불 매질, 열 아지랑이, 디버그 드로우, 데칼, 머리카락) | 엔진 2 세션 (재배정 2026-09-26 3a708c49) | `Native/Render/Passes/Volume/`, `Native/Render/Passes/Debug/`, `Native/Render/Passes/Decal/`, `Native/Render/Passes/Hair/`, `Config/quality/volume.toml`, `Config/quality/debug.toml` |
| W 물 (물 표면, 수중, 유체 표면 재구성 B7·B8) | 엔진 1 세션 (재배정 2026-09-26, 조정) | `Native/Render/Passes/Water/`, `Config/quality/water.toml` |
| RPP RPP-1 장면 | RPP 세션 (v1.40) | `Content/RPP1/`, `Tools/RppBuild/`, `Docs/Status/RPP_STATUS_KO.md`, `Results/RPP/` (이전 저장소 쪽 `Assets/RPP1/`) |
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
powershell -File Tools/CI/Build.ps1 -Track <core|M|S|R|C|I|FX|RPP> [-Target <타깃>] → build/<트랙>, 코어 + 그 트랙만 (2.5; I는 V;M;S;R;I, RPP는 C;RPP)
powershell -File Tools/CI/Build.ps1 -Track all                                    → build/all, 모든 트랙(통합: 게이트 측정용)
build/<트랙>/bin/unx_unit_tests.exe                                               코어 단위 테스트(디버그 레이어)
build/<트랙>/bin/unx_gate_empty_frame.exe --validate                              debug layer + GPU-based validation
powershell -File Tools/CI/GpuLock.ps1 -Track <트랙> -- <성능 측정 명령>            성능 측정(3.3)
```

### 2.5 트랙 선택 빌드 (v1.1)
네 세션이 작업 트리 하나를 같이 쓰므로, 한 트랙의 작성 중 파일이 다른 트랙의 빌드를 깨면 안 된다.
- CMake 캐시 변수 `UNX_TRACKS`: `all` 또는 `V;M;S;R;C;I;FX;RPP`의 부분집합(코어는 항상 켜짐). 꺼진 트랙의 렌더 모듈(`*.cpp`), 커널(`*.hlsl`), 도구(`Tools/<폴더>`, `Reference`), Tests·Gates 실행 파일은 구성하지 않는다. 꺼진 트랙의 진입점(`Tracks.h`)은 코어의 빈 구현(`Native/Render/Frame/Stubs/Track<트랙>.cpp`, 패스 없음, 로그 한 번)이 대신한다.
- `Build.ps1 -Track <이름>`이 기본 선택을 정한다: `core` → V(코어 세션, v1.2), `M` → M, `S` → S, `R` → R, `C` → C, `V` → V, `I` → V;M;S;R;I(호스트 DLL은 렌더러 전체를 링크, v1.6), `RPP` → C;RPP(v1.40: RPP-1 장면 빌드 CPU 도구 `Tools/RppBuild`가 `unx_scenegen`·`unx_scene`만 링크), `all` → 전부. I 폴더(`Native/Host`)는 I가 켜졌을 때만 최상위 `CMakeLists.txt`가 `add_subdirectory`한다(그 안의 `CMakeLists.txt`는 I 소유). 다른 조합은 `-Tracks "S;V"`처럼 준다.
- 트랙 세션의 개발·정확성 검사는 자기 선택 빌드(`build/<트랙>`)로 한다. **모든 트랙을 켜는 통합 빌드(`-Track all`, `build/all`)는 게이트 측정 때만** 쓰고, 통합 빌드가 실패하면 원인 파일의 소유 트랙이 고친다.
- 꺼진 트랙의 공개 HLSL 헤더(5.6)는 소스 트리에 있으면 include할 수 있다(커널만 컴파일하지 않는다). 다른 트랙 헤더는 그 트랙이 커밋한 뒤에 쓴다.

### 2.6 요구 하드웨어와 표준 경로 (v1.17, 사용자 지시)
완성된 게임은 개발 PC(RTX 4080)만이 아니라 다른 컴퓨터에서도 돌아야 한다. 지금 다른 GPU용 작업을 하지는 않지만, 렌더러가 개발 PC에서만 도는 구조가 되지 않게 아래를 지킨다.
- **요구 기능(DX12 Ultimate 계열, `Device`가 시작할 때 검사하고 없으면 실패)**: Direct3D 12 기능 수준 12_2, 셰이더 모델 6.6 이상(bindless `ResourceDescriptorHeap`), 메시 셰이더 tier 1, DXR 1.1(raytracing tier 1.1, 인라인 광선 포함), enhanced barriers, resource binding tier 3, resource heap tier 2(버퍼·텍스처를 섞은 과도 힙). 캐스팅 가능한 뷰 형식을 쓰는 자원이 있으면 relaxed format casting(`DeviceCaps::relaxedFormatCasting`)도 필요하다(없으면 그 자원을 만들 때 실패; v1.13).
- **런타임**: Agility SDK 1.618.5(SDK 버전 618)를 실행 파일 옆 `D3D12\`에 둔다. Unity 호스트 안에서는 Unity가 싣는 D3D12Core(6000.6: 1.618.1, 같은 SDK 버전 618)를 쓴다(4.1).
- **표준 경로가 항상 있다**: 벤더 전용 기능(NVAPI: 클럭 판독, OMM, SER 등)은 선택 사항이다. 그것이 없으면 같은 결과를 내는 표준 D3D12 경로로 동작한다. 벤더 경로를 넣는 트랙은 표준 경로와 결과가 같음을 테스트로 보인다. 지금 NVAPI는 하네스의 클럭 기록에만 쓰인다.
- **선을 올리는 변경은 기록한다**: 새 기능이 요구 기능을 늘리면(예: 셰이더 모델 6.8 전용 기능, 작업 그래프, 새 형식 지원) 이 절과 `Device`의 검사에 함께 적고 12절에 남긴다.
- **약한 GPU에서의 동작 정책은 나중에 정한다.** 품질을 몰래 낮추는 경로는 여전히 금지다. 정할 때도 비용식과 품질 정의를 먼저 쓴다.
- 개발 PC 측정값은 그 장치의 실측으로만 적는다. 다른 장치의 성능은 측정 전까지 [예상]이다.

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
- (v1.30, 사용자 결정) 잠금 안에서 하나씩 돌리는 것: 성능 측정(timing), **새 커널의 첫 하드웨어 실행**(`-Kind correctness`, 짧게), **물리 GPU 실행**. 그 밖의 정확성 실행(단위 테스트, 정확성 테스트, 디버그 레이어·GPU 검증, 기준 영상 비교, 디버그 캡처)은 잠금 없이 동시에 해도 된다. (TDR 원인 조사 동안의 "모든 하드웨어 GPU 실행은 잠금 안" 임시 규칙은 끝났다.)
- (v1.28) `-Kind timing|correctness`(기본 timing): 잠금을 잡은 실행의 종류다. `current.json`의 `kind`와 `history.log`의 `acquire <트랙> (<종류>) :: ...`, `release <트랙> (<종류>) exit N`에 남는다. CPU를 많이 쓰는 백그라운드 작업(C 기준 렌더 대기열, WARP 실행)은 `kind`가 timing일 때만 멈춘다(정확성 실행을 잠금 안에서 직렬화하는 동안 25분씩 멈추지 않게).
- (v1.30) **보유 상한과 프로세스 트리**: GpuLock.ps1은 명령을 Job 객체(닫히면 전부 종료)에 넣어 띄운다(일시 정지로 만들어 Job에 넣은 뒤 재개하므로 손자 프로세스도 빠지지 않는다). 콘솔·GUI 실행 파일(Unity.exe)·`.cmd`·`.ps1` 모두 같은 경로로 기다린다(v1.26의 `Start-Process -Wait` 대신).
  - `-TimeoutMinutes N`(기본 45: 기록상 가장 긴 정상 보유는 Unity 테스트 27.6분, FX 멈춤은 31분이었다)을 넘으면 프로세스 트리 전체를 끝내고 `release <트랙> (<종류>) exit 124 TIMEOUT after N min`을 남긴다. 래퍼 종료 코드도 124다. 더 긴 실행은 `-TimeoutMinutes`를 준다.
  - 래퍼가 강제 종료되면 Job의 마지막 핸들이 닫혀 명령의 트리도 함께 끝난다. 잠금이 풀린 뒤에 GPU를 쓰는 고아 프로세스가 남지 않는다.
  - 명령이 끝나고 5초 뒤에도 남은 자손은 끝내고 release 줄에 이름을 적는다(`(ended leftover descendants: ...)`).
  - release 줄 없이 죽은 보유자는 다음 acquire가 `stale release <트랙> (<종류>) holder pid N gone :: <명령>`으로 남긴다(뮤텍스가 abandoned로 돌아오거나, 보유자의 `current.json`이 남아 있고 그 프로세스가 없을 때).
  - `current.json`은 뮤텍스 보유자만 쓴다(임시 파일 + 원자적 이름 바꾸기). 읽는 쪽은 읽기·쓰기·삭제 공유로 연다(여러 세션이 동시에 잠금을 잡을 때 IOException으로 exit 1이 나던 문제, S 신고).
  - 잠금을 기다리는 시간의 상한은 `-WaitMinutes`(기본 120)다(v1.29까지 `-TimeoutMinutes`였다).
  - release 줄의 종료 코드 표기: 87 `DEVICE_REMOVED`, 88 `FENCE_TIMEOUT`, 124 `TIMEOUT`.
- (v1.26, v1.27) **장치 제거(TDR)**: `check()`가 DEVICE_REMOVED/HUNG/RESET/DRIVER_INTERNAL_ERROR를 만나거나 제거된 장치의 펜스(UINT64_MAX)를 읽으면 정책(`setDeviceRemovedPolicy`, `D3D12.h`)을 따른다.
  - `Exit`(기본: 테스트·게이트·도구): 표준 출력 마지막 줄 `UNX_DEVICE_REMOVED <what> hr 0x.. reason 0x..`(GetDeviceRemovedReason) + 종료 코드 **87**(`kDeviceRemovedExitCode`). 곧바로 끝내고 정적 소멸자는 돌지 않는다. GpuLock.ps1은 `history.log`에 `release <트랙> exit 87 DEVICE_REMOVED`로 남긴다.
  - `Throw`(호스트 프로세스: Unity 편집기·Player가 살아남아야 한다. 호스트는 장치를 만들기 전에 설정한다. 호스트 장치(`externalDevice`) 위의 첫 장치는 명시 설정이 없으면 Throw다): 같은 줄을 로그에 남기고 `check()`가 `DeviceRemovedError{ where, hr, reason }`를 던진다. `Queue::signal/waitCpu`, `Device::waitIdle`(소멸자에서도 불린다)는 던지지 않는다. 제거를 기록하고 바로 돌아온다(기다릴 작업이 없다). `deviceWasRemoved()`로 확인한다.
  - Exit 줄은 표준 출력·오류를 비운 뒤 `TerminateProcess`로 끝낸다(v1.30). `_Exit`는 DLL 분리를 돌려 D3D12 디버그 레이어의 live object 보고가 마지막 줄 뒤에 찍혔다.
- (v1.30, 조율 요청) **CPU 펜스 대기 상한**: `Queue::waitCpu`(그리고 `Device::waitIdle`)는 `waitFenceCpu(fence, event, value, what)`(`D3D12.h`)로 기다린다. 상한은 `UNX_FENCE_TIMEOUT_S`초(한 번 읽음, 기본 60, 0 = 없음: PIX 캡처·디버거용)다. TDR에 걸리지 않는 GPU 쪽 교착(아무도 신호하지 않는 펜스를 기다리는 큐)이 잠금을 무기한 쥐지 않게 한다.
  - `Exit`: 마지막 줄 `UNX_FENCE_TIMEOUT <what> value N completed M after S s` + 종료 코드 **88**(`kFenceTimeoutExitCode`). `<what>`은 `Queue::waitCpu(graphics|compute|copy)`다.
  - `Throw`: 같은 줄을 로그에 남기고, 장치를 제거와 같이 잃은 것으로 친다(`deviceWasRemoved()`가 true, 이후 대기는 곧바로 돌아온다). 대기는 던지지 않고 돌아온다. 호스트는 `deviceWasRemoved()`를 보고 오류를 돌려준다.
  - 상한에서 장치가 제거된 것으로 확인되면(펜스 UINT64_MAX 또는 `GetDeviceRemovedReason` 실패) 시간 초과가 아니라 제거(87)로 보고한다.
  - 호스트 펜스를 직접 기다리는 코드(I: `HostBoundary.cpp`, `UnityPlugin.cpp`)도 `waitFenceCpu`를 쓰면 같은 규칙을 따른다.
- (v1.39, 조율 요청) **경합 검출**: 정확성 실행은 잠금 없이 돌므로(사용자 결정), timing 값이 다른 GPU 작업과 겹쳤는지를 GpuLock.ps1이 잰다.
  - 명령이 도는 동안 래퍼 안의 스레드가 1초마다 PDH `\GPU Engine(*)\Running Time`(작업 관리자와 같은 출처, D3DKMT 통계)을 읽는다. 대상은 3D·compute·copy 엔진이고, 명령의 Job 트리 밖 프로세스마다 가장 바쁜 엔진의 바쁜 시간(ms/s, 구간 길이로 상한)을 센다.
  - 50 ms/s 이상(GPU 엔진의 5 %)이면 그 초는 **contended**다. [실측] 배경 UI(브라우저 웹뷰, 런처, Claude 앱)는 6~22 ms/s였다. pid 4(System, 측정 프로세스가 일으키는 레지던시 페이징)는 판정에서 뺀다.
  - release 줄: `contended: <이름> (pid) N s >= 50 ms/s, peak P ms/s; ...`.
  - 명령에는 `UNX_GPU_CONTENTION` = 매초 갱신되는 요약 파일(`.gpulock/contention.<래퍼 pid>.json`, 줄마다 `{"t_ms": unix ms, "pid", "ms_per_s", "name"}`)이 주어진다. `Harness::run`은 측정 창 안의 샘플을 결과 JSON `gpu_contention`에 넣고, 그 초에 제출된 프레임을 `contended_frames`로 세며, 나머지 프레임의 `gpu_frame_ms_uncontended`를 낸다. 요약에도 `GPU CONTENDED` 줄이 붙는다. 하네스를 쓰지 않는 측정(I의 Player JSON 등)도 같은 파일을 읽으면 된다.
  - 샘플 비용은 래퍼 프로세스의 CPU 1~2 ms/s다. GPU 작업은 없다.
  - [실측] 자체 시험(별도 뮤텍스 사본, 진짜 잠금 안): 단독 실행은 contended 0 s, 중앙값 0.266 ms. 같은 게이트를 잡 밖에서 돌리는 경합자를 붙이면 중앙값 0.861 ms, JSON contended 15 s, release 줄에 경합자(29 s, peak 761 ms/s)가 나왔다. 같은 실행 중 다른 세션의 잠금 없는 FX 테스트(`unx_test_fx_particletests`, 2 s)도 잡혔다.
- (v1.41) **경합 판정 대상, CPU 경합, 대기자, HOLD**(FX 신고·조율 결정·C 조각 요청):
  - GPU는 **하드웨어 어댑터의 엔진만** 센다(DXGI 소프트웨어 플래그가 없는 어댑터, 카운터 인스턴스 이름의 LUID로 대조). 이유: WARP(Basic Render Driver)의 가상 엔진이 WARP 스레드가 도는 동안 1000 ms/s로 읽혔다.
  - **우리 프로세스만 경합이다**: 저장소 산출물·도구(이미지 경로가 Unravel* 체크아웃이나 Claude 스크래치 폴더 아래, 또는 이름이 unx*·pd_*·DesignBench*·GpuPathTracer*), Unity와 그 작업자, 도구 사슬(cl, link, ninja, cmake, dxc). 그 밖(브라우저, 채팅, 런처, 오버레이, Claude 앱)은 사용자 측정 환경의 일부라 판정에 넣지 않는다. 200 ms/s를 넘는 시간이 5 s를 넘을 때만 release 줄에 `background: ...`로 적는다.
  - **CPU 경합**: 같은 1초 샘플에서 트리 밖 우리 프로세스의 CPU 시간(user + kernel, `NtQuerySystemInformation` 한 번) 합이 4코어 이상이면 그 초는 `cpu-contended`다. release 줄 `cpu-contended: N s >= 4 cores, peak P cores; top: 이름 (pid) C core-s`, 요약 파일 `cpu_contended_seconds`·`cpu_over`(줄마다 `{"cpu_t_ms", "cores", "top"}`; 하네스의 `t_ms` 샘플과 섞이지 않는다). 이유: WARP 실행·빌드가 CPU 틱 측정(애니메이션, World, 물리)을 흐린다. 배경 프로세스가 4코어를 5 s 넘게 쓰면 `background: cpu ...`로 적는다.
  - 구간 끝의 짧은 샘플(명령 종료로 잘린 반 구간 미만)은 합계에만 넣고 판정하지 않는다(스케줄러 틱 단위 계측이 짧은 구간에서 가짜 최고값을 만든다).
  - **대기자 표시**: 기다리는 쪽은 첫 대기 전에 `.gpulock/waiting/<pid>.json`(`{track, kind, pid, since, command}`, tmp 뒤 `MoveFileEx` 교체)을 쓰고, 얻거나 포기하면 지운다. 죽은 pid의 파일은 보는 쪽이 지운다.
  - **correctness 양보**: kind = correctness인 획득은 살아 있는 timing 대기자가 있으면 250 ms마다 다시 보며 기다린다. 뮤텍스를 얻은 직후에도 한 번 더 보고, 그 사이 timing 대기자나 HOLD가 생겼으면 다시 놓는다. timing끼리는 뮤텍스 순서를 따른다.
  - **HOLD**: `.gpulock/HOLD`가 있으면 아무도 새로 얻지 않는다(사용자가 게임할 때 두는 표지, 내용 = 사유; 조율 규칙).
  - (v1.85, 조율 세션 2026-10-01 13:30, 사용자 지적) **종류와 무관한 선착순, 한 번에 10분**:
    - timing 우선을 없앤다. 차례는 종류와 상관없이 `since` 순, 같으면 pid 순이다. kind는 배경 CPU 작업 정지 여부와 경합 판정에만 쓴다.
    - 한 번 잡는 시간은 약 10분 이하로 한다. 더 긴 묶음은 나눠서 조각 사이에 다시 줄을 선다.
    - 이유: timing 한 묶음이 31분 동안(12:35~13:06) 다섯 세션을 세워 두었다.
    - 프로세스 안 조각(`GpuLock.cpp`, `GpuSlice.cpp`)도 같은 순서로 맞춘다.
    - [실측] 별도 뮤텍스 사본 시험: correctness A, timing T, correctness B, timing U가 차례로 오자 H→A→T→B→U 순서로 얻었다.
  - (v1.83, 조율 세션 2026-10-01) **차례와 공용 폴더**(아래 "timing이 먼저"는 v1.85가 대체한다):
    - 기다리는 쪽은 차례대로 얻는다. timing이 correctness보다 먼저이고, 같은 종류 안에서는 `since` 순, 같으면 pid 순이다. 자기보다 앞선 살아 있는 대기자가 있으면 얻지 않는다. 뮤텍스를 얻은 직후에도 다시 본다.
    - 대기 파일의 pid가 `since`보다 5 s 넘게 늦게 시작한 프로세스의 것이면(재사용된 pid) 그 파일은 낡은 것으로 지운다.
    - 잠금 폴더는 **주 체크아웃의 `.gpulock` 하나**다. worktree의 `.git` 파일 → 그 git 폴더의 `commondir` → 공용 git 폴더의 부모를 따른다. `UNX_GPU_LOCK_DIR`가 있으면 그것을 쓴다.
    - 이유: correctness Verify가 2시간, 하드웨어 유체 실행이 1.5시간 기다리는 동안 나중에 온 실행이 16번 먼저 얻었다. 또 redesign worktree가 자기 `.gpulock`을 따로 써서, 그쪽 timing 대기자·history·HOLD가 주 체크아웃 쪽에 보이지 않았다.
    - 프로세스 안 조각(`GpuLock.cpp`, `GpuSlice.cpp`)도 같은 차례와 폴더 규칙을 따라야 한다. 그때까지 기존 worktree의 `.gpulock`은 주 체크아웃 폴더를 가리키는 디렉터리 정션이다.
    - [실측] 별도 뮤텍스 사본 시험: 보유자 H(8 s)가 있는 동안 correctness A·B, timing T, correctness D가 차례로 오자 H→T→A→B→D 순서로 얻었다. 살아 있는 다른 프로세스의 pid에 어제 `since`를 단 가짜 timing 대기 파일은 지워졌고 아무도 막지 않았다.
  - **프로세스 안 조각**(C의 GPU 기준 경로추적기 등 수 분짜리 correctness 작업): 같은 규약(뮤텍스 이름, current.json 원자 교체, history 줄, 대기자·양보·HOLD)을 프로세스 안에서 조각마다 따른다. history 줄은 `acquire C (correctness) :: slice k/N <what>` / `release C (correctness) exit 0 slice k/N <ms> ms`, 조각이 15 s를 넘으면 `LONG_SLICE`. C++ API는 `GpuLock.h`의 `GpuLockSlice(track, kind, what)`다(v1.42): `acquire(waitLimit, label)`이 false면 못 얻은 것이고 `lastBlocker()`가 이유다. `release(exitCode)`, 소멸자가 놓는다. 조각 동안 그 프로세스의 `UNX_GPU_LOCK` = track이다. 잠금 폴더는 `UNX_GPU_LOCK_DIR` 또는 현재 디렉터리에서 위로 찾은 `.gpulock`(저장소를 작업 디렉터리로 두고 실행)이다. acquire와 release는 같은 스레드에서 부른다(Win32 뮤텍스). [실측] 단위 시험 `gpu_lock_slice_protocol`(별도 뮤텍스·임시 폴더): 얻기·놓기의 current.json과 history 줄, HOLD 차단과 사유, 살아 있는 timing 대기자에게 양보한 뒤 그 프로세스가 끝나면 얻고 대기 파일을 지운다.
  - [실측] 자체 시험(작업 트리 사본, 진짜 뮤텍스): HOLD가 있으면 얻지 않고 사유를 알린다. 살아 있는 가짜 timing 대기자가 있으면 correctness가 양보한다. 그 프로세스가 끝나면 파일이 지워지고 곧 얻는다.
  - [실측] CPU 시험(9 s 명령, 잡 밖에서 powershell 바쁜 루프 5개 8 s): release 줄 `background: cpu 7 s >= 4 cores, peak 5.2 cores`(바쁜 루프 = 배경)와 `cpu-contended: 9 s >= 4 cores, peak 26.9 cores; top: unx_reference.exe 159 core-s, unx_study_material_layers.exe 18, cl.exe 9, unx_test_fx_particletests.exe 9`(같은 시각 다른 세션들의 실제 CPU 작업)가 나왔다. 짧은 끝 구간 규칙 전에는 0.1 s 구간이 "peak 58.1 cores"를 만들었다.
- 사용자의 다른 GPU 앱은 닫지 않는다. 측정은 4K·1440p만(하네스가 강제), 1.5초 워밍업, 중앙값·P95·P99.

### 3.4 결과·상태
- 게이트 결과: `Results/<트랙>/<게이트>/`(JSON 요약·로그는 커밋, 프레임별 CSV는 커밋하지 않음). 모든 보고에 해상도·품질 해시·빌드 identity·드라이버·큐 우선순위·GPU 잠금 보유자가 들어간다(하네스가 기록).
- 트랙 상태: `Docs/Status/<트랙>_STATUS_KO.md`. 실측/예상 표기 규칙은 설계서와 같다.
- **기준 비교 한도(v1.34, C 합의)**: 엔진 영상 대 C 기준 경로추적 영상의 한도는 장면마다 `Config/quality/reference.toml`의 `[reference.threshold.<장면>]` `flip_mean` / `flip_p99` / `relmse`다(C 소유, 측정한 바닥 값을 주석으로 함께 적는다).
  - 한도 = k × 바닥이고 k = 2(잠정)다. 바닥은 같은 해상도에서 기준의 두 반쪽(각 spp/2) 사이 지표다(`unx_metrics --reference <반쪽 A> --test <반쪽 B>`).
  - 전체 기준의 참값 대비 잡음은 relMSE로 바닥의 약 1/4이다. 그래서 k = 2이면 엔진과 기준의 차이가 기준 자체의 불확도보다 충분히 클 때만 실패한다.
  - 바닥이 판정 해상도보다 큰 장면(예: city_block, 창유리 태양 코스틱 반딧불로 FLIP P99 0.42)은 C가 기준을 먼저 수렴시킨 뒤 한도를 둔다. 태양 코스틱은 광원 추적 경로를 더해 경로 공간을 나누는 방식으로 고친다.
  - 한도가 정해지기 전의 비교 결과는 [잠정]으로 적는다.

### 3.5 통합 게이트 빌드: 커밋 기준 (v1.12, 사용자 지시)
- 여러 트랙을 합친 측정(통합 게이트, `-Track all` 또는 여러 트랙 조합)은 **커밋된 코드로만** 빌드한다: `powershell -File Tools/CI/Build.ps1 -Track all -Committed [-Ref <커밋>]`. 저장소 옆 git worktree `..\UnravelNext-gate`를 그 커밋(기본 HEAD)으로 맞추고(서브모듈은 본 저장소의 객체로, 의존성 캐시는 복사) 그 안의 `build\<트랙>`에 빌드한다. 공유 작업 트리의 남의 미커밋 파일은 들어가지 않고, 결과 JSON의 `build.commit`은 그 커밋, `build.dirty`는 false다. worktree는 객체만 공유하고 index는 따로라 다른 세션의 스테이징·커밋과 무관하다. `-Committed` 빌드는 한 번에 하나(이름 있는 뮤텍스, 나중 것이 기다린다).
- 실행: 작업 디렉터리는 본 저장소로 두고 실행 파일은 게이트 worktree에서 부른다. 예: `powershell -File Tools/CI/GpuLock.ps1 -Track <트랙> -- ..\UnravelNext-gate\build\all\bin\unx_gate_<...>.exe --out Results/<트랙>/<게이트>`. 품질 설정은 그 커밋의 것이다(`UNX_SOURCE_DIR` = worktree).
- 트랙 단독 게이트(자기 트랙 + 코어)는 지금처럼 자기 선택 빌드(`build/<트랙>`)를 써도 된다.
- 빌드 폴더의 트랙 집합이 바뀌면 `Build.ps1`이 경고하고 그 폴더를 새로 구성한다. 같은 폴더에서 트랙 집합을 바꿔 다시 구성하면 Ninja dyndep 단언으로 멈췄기 때문이다(I 신고).

### 3.6 데이터 의존 셰이더 루프의 상한 (v1.39, 조율 결정)
- 이 장치는 선점이 된다. 그래서 끝나지 않는 커널이 TDR 없이 계속 돌 수 있다. 그동안 GPU가 시분할되어 데스크톱과 다른 세션이 느려진다.
- 반복 횟수가 데이터에 따라 정해지는 셰이더 루프에는 모두 **하드 반복 상한**을 둔다: 웨이브 집계 루프(웨이브 폭), 트리·사슬·목록 걷기(깊이나 용량), CAS 재시도, 타일·레코드 루프(풀 용량).
- 상한에 닿으면 조용히 끊지 않는다. 트랙의 오류 비트를 세워(V: `Stats::overflow` 0x400 `OVERFLOW_ITERATION_LIMIT`) 게이트를 실패로 만든다. 그래야 livelock이 멈춤이 아니라 검출되는 결함이 된다.
- 하드웨어 재현은 잠금 안에서 `UNX_FENCE_TIMEOUT_S`를 짧게(5~10 s) 하고 프레임 1~3개로 한다.
- **TDR을 일부러 재현하지 않는다**(v1.40, 조율 결정). TDR은 GPU를 리셋해서 다른 세션의 실행과 사용자의 앱까지 끊는다.
  - 멈춤이나 초선형 비용이 의심되면 부하를 1만 → 3만 → 10만 → 30만처럼 늘려 가며 dispatch 시간을 잰다. 성장 곡선(선형인지 제곱인지)으로 원인을 확정한다. 각 실행은 dispatch 하나가 약 0.2 s를 넘지 않는 크기에서 멈춘다.
  - 수정 뒤에도 같은 곡선으로 선형을 확인한다. 전체 크기는 곡선이 선형이고 외삽값이 0.5 s보다 훨씬 작을 때만 한 번 돈다.
  - 데이터 의존 루프 상한은 반복 수를 막을 뿐 dispatch 하나의 시간은 막지 못한다. 깊이·밀도에 따라 한 그룹의 일이 커지는 구조는 병렬화해야 한다(예: 무거운 타일을 블록 단위로 여러 그룹에 나눈다).

## 4. Render graph API (`Native/Render/include/unx/render/RenderGraph.h`)

```cpp
TextureRef RenderGraph::createTexture(const TextureDesc&);        // 트랜지언트: 수명 = 첫 사용 ~ 마지막 사용, 메모리 aliasing
                                                                   // TextureDesc::srvFormat/uavFormat(v1.13): 형식과 다른 뷰 형식
                                                                   // (texel당 비트가 같은 relaxed format casting, 예: UAV R32_UINT로
                                                                   // 쓰고 SRV R9G9B9E5_SHAREDEXP로 하드웨어 필터)
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
6. 루트 시그니처는 하나(`Device::rootSignature`): 루트 상수 32 DWORD(b0, `P[8]` uint4), 루트 CBV b1 = 뷰 프레임 상수(5.5), 정적 샘플러 s0 point-clamp, s1 linear-clamp, s2 linear-wrap, s3 aniso16-wrap, s4 comparison(GREATER_EQUAL), s5 aniso16-clamp(`g_anisoClamp`, v1.10). 서술자는 bindless(`ResourceDescriptorHeap[]`).
7. 파이프라인: `ShaderLibrary::compute("<커널 이름>")`, `ShaderLibrary::mesh("<이름>", MeshPipelineDesc)`. 파일당 커널 하나, 모드는 컴파일 변형으로.
8. 지속 자원(VSM 풀, GI 캐시, TLAS 등)은 소유 트랙이 `Device`로 만들어 매 프레임 `import`한다. 해제는 `Device::deferRelease`(GPU가 끝낸 뒤).

- **밴드 패스 그룹(v1.29, 설계 개정 1 요청 7절)**: `RenderGraph::addBandedGroup(group, height, bands, { BandedPass{ name, queue, setup, execute } ... })`는 그룹의 패스들을 밴드 순서로 선언한다(A(밴드 0) → B(밴드 0) → … → A(밴드 1) …). 패스 이름은 `<group>.<name>.b<밴드>`다.
  - 같은 setup으로 밴드마다 선언되므로, 그래프가 밴드마다 같은 자원의 배리어를 보통 패스처럼 낸다.
  - execute는 `PassContext::band`(`PassBand{ index, count, y0, y1 }`, 행 [y0, y1), y0은 8행 경계)를 읽고 그 행만 쓴다. 그룹 밖 패스는 band = {0, 1, 0, UINT32_MAX}다.
  - 밴드 경계를 넘어 읽으면 이전 밴드의 끝난 행만 보인다. 아래 행을 읽는 패스는 `band.lagged(rows)`로 처리 행을 늦춘다(v1.31): [y0 − rows, y1 − rows)를 0에서 자르고, 첫 밴드는 0행부터, 마지막 밴드는 끝까지다. 그룹의 밴드들에 대해 늦춘 구간도 뷰를 빈틈없이 덮는다. 비어 있지 않은 늦춘 구간은 그 밴드나 앞 밴드가 만든 행만 읽는다(y1' + rows ≤ y1). 지연은 사슬을 따라 더해진다(생산자가 8행 늦으면 그것을 읽는 3×3 소비자는 9행).
  - `passBand(height, count, index)`(v1.31)는 `addBandedGroup`과 똑같이 밴드를 자른다. 트랙은 그룹 앞에서 밴드별 작업 목록(타일 목록, 간접 인자)을 이것으로 나눈다. 원자 카운터 하나로 목록을 채우면 목록 내용은 밴드 순서와 무관하게 정확하다. 간접 인자는 밴드마다 슬롯을 두고 밴드별 시작 오프셋을 기록한다.
  - 밴드 수는 `passBandCount(quality, width, height)` = round(픽셀 수 / `output.band_pixels`), 최소 1이다. **`band_pixels` = 0은 한 밴드(뷰 전체)이고 v1.31부터 기본값이다.** 근거 [실측, M]: M 패스만 4K/8(1036800)로 밴드화했을 때 도시 4K 프레임이 6.725 → 6.800 ms로 순손실이었다. 셰이딩이 지연에 묶여 있어 L2 적중이 밴드마다의 배리어를 갚지 못한다. 셰이딩이 바닥에 가까워지면 1036800으로 다시 잰다(같은 코드, 설정만 바꾼다).
  - 근거 [실측, 설계 벤치 `--only-bands`, UAV↔SRV 전환 포함]: 해석 → 셰이딩 4K 0.740 → 0.470 ms. 실제 프레임 순서에서는 해석과 셰이딩 사이에 전체 화면 소비자(S 페이지 표시, 프록셀, R GI·반사)가 있어서 이 쌍은 성립하지 않는다(M). 그래서 그룹은 아래의 조명 그룹이다.
  - **조명 그룹(v1.31, M·S 요청)**: FrameRenderer가 뷰마다 `tracks::shadowVisibilityPasses(fc, view)`(S) + `tracks::shadingPasses(fc, view)`(M)를 한 그룹으로 선언한다. 이름은 `lit`, 밴드 안 순서는 목록 순서(S → M)다. 그 뒤 `tracks::shadingComposite(fc, view)`(M: 가장자리 합성처럼 그룹 결과를 뷰 전체로 읽는 일)를 부른다(`Tracks.h`).
    - `*Passes` 함수는 자기 자원을 만들고, 그룹 앞에 돌 보통 패스(클리어, 밴드별 타일 목록)를 직접 선언한 뒤 밴드 패스 목록을 돌려준다. setup은 밴드마다 불리므로 사용 선언만 한다(자원을 만들지 않는다). execute는 값 캡처이고 자기 밴드의 행만 쓴다.
    - S 가시성은 지연 0행이다(S: 이웃 읽기는 그룹 앞에 이미 있는 깊이 ±1 픽셀뿐). M 검출·셰이딩은 `lagged(8)`이다.
    - `shadowVisibility`와 `shading`은 S·M이 새 함수를 커밋할 때까지 그대로 불린다. 둘 다 들어오면 코어가 FrameRenderer를 한 커밋으로 바꾼다. 꺼진 빌드용 코어 스텁은 빈 목록을 돌려준다.
  - 검증 [실측]: 단위 테스트 `graph_banded_group_covers_every_row`(1·3·8밴드, 999행, 모든 셀 일치, 밴드가 행을 빈틈없이 덮음, 기록된 행 = `passBand`, 패스 수 = 패스 × 밴드), `graph_band_rows_and_lag`(높이 1~2160, 밴드 1~300, 지연 0·1·8·16의 9,156개 밴드: 연속·8행 경계·늦춘 구간 연속·y1' + rows ≤ y1).

### 4.1 디바이스와 호스트 통합 (v1.9, I 요청 `20260925_I_unity_queue_device.md`)
`Device(DeviceOptions)`가 디바이스·큐 셋·서술자 힙·루트 서명을 만든다. 호스트(Unity) 안에서는 I가 다음을 쓴다:
- `DeviceOptions::externalDevice`: 호스트 디바이스 위에 만든다. 디버그 레이어를 켜지 않는다(`debugLayer = true`면 실패한다. 디바이스가 있는 프로세스에서 켜면 그 디바이스가 제거된다). 어댑터를 열거하지 않고 디바이스 LUID로 찾는다. 기능 검사는 그대로 한다.
- `DeviceOptions::externalGraphicsQueue`: 호스트의 DIRECT 큐를 그래픽스 `Queue`로 감싼다. fence는 `Device` 것이고 큐의 이름·우선순위는 바꾸지 않는다. compute·copy 큐는 디바이스에 새로 만든다(`queuePriority`는 그 둘에만 적용된다). 프레임은 호스트 큐 위의 리스트 하나이고 큐 사이 동기화가 없다.
- `Queue::setExecuteHook(fn)`: `execute`만 호스트 경로(Unity `IUnityGraphicsD3D12v8::ExecuteCommandList`)로 보낸다. `signal`·`waitGpu`는 큐에 직접 한다. 호스트가 큐 접근을 허용하는 동안(렌더 이벤트)에만 설정한다.
- 단위 테스트 `device_on_host_device_and_queue`.

## 5. 프레임 구성 (`Frame.h`, `Tracks.h`, `FrameRenderer.h`)
- **헤더 배치(v1.42, 인프라 요청 `20260926_Infra_header_split.md`)**: 자주 바뀌는 계약은 작은 헤더에 있다. 필요한 것만 include한다.
  - `ViewDesc.h`(`ViewDesc`), `FrameResources.h`(`ViewResources`, `FrameResources`), `FrameContext.h`(`FrameContext`, `kGpuSimulation*`, `kDiscontinuity*`), `DepthRaster.h`(`RasterView`, `DepthRasterRequest`).
  - `GraphTypes.h`(`Use`, `TextureDesc`, `BufferDesc`, `TextureRef`, `BufferRef`; `RenderGraph.h`가 include), `ViewKind.h`(`gpu::ViewKind`; `GpuSceneLayout.h`가 include), `TrackPending.h`(`tracks::pending`).
  - `Frame.h`는 `TrackState`, `FrameServices`, `FramePassContext`, `passBandCount`를 두고, 당분간 위 헤더와 예전 include(`GpuSceneLayout.h`, `RenderGraph.h`, `Shaders.h`, `Device.h`, `SceneData.h`) 전부를 계속 include한다. 그래서 바꾸지 않아도 컴파일된다. `Tracks.h`도 `Frame.h`를 계속 include한다.
  - 각 트랙은 인프라가 보내는 목록(HeaderCost)대로 자기 파일의 include를 작은 헤더로 옮긴다. 다 옮기면 `Frame.h`의 호환 include를 지운다(코어, 공지 뒤).
  - 새 헤더는 모두 단독 컴파일된다[실측, 프로젝트 플래그 /W4 /WX로 헤더마다 한 번역 단위].

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
| coverageTiles / coverageRecords / coverageTileList / coverageTilePixels / coverageDepthRange | 7.1 (v1.41 타일 구간·픽셀 순서, 깊이 구간; v1.38 레코드 기반 opaqueCovered) | V | M, S |
| gbuffer | RG32_UINT (7.2) | M | S, R |
| shadowVisibility | R32_UINT (7.3) | S | M |
| shadowOverflowTiles / shadowOverflow / shadowOverflowFallbackTiles | 7.3 (v1.20; v1.22부터 평면 뷰도 그 뷰 리스트 기준) | S | M |
| shadowFragmentVisibility / shadowFragmentSun | 7.3 (v1.41, S 요청 `20260926_S_fragment_visibility.md`) | S | M |
| particleLayer / particleDepthRange / particleEdges | FX의 `Passes/FX/ParticleLayer.hlsli` (v1.41, FX 요청 `20260926_FX_particle_render_pass.md` 8a) | FX | M |
| distortionOffset / distortionDepth (열 아지랑이) / volumeSlices (입자 매질, 주 뷰) | E의 `Passes/Volume` (FrameResources.h 주석; 요청 `20260925_FX_particle_render_rules.md` 3b, FEATURES_GAME 0.A-8) | E | M(굴절 합성), S(프록셀 적분 합류) |
| froxelLights / airVolume (뷰 단위, v1.22) | 7.4, v1.15 형식 | S(메인 뷰: 코어가 FrameResources 참조를 넣음) | M |
| screenProbes | R 내부 형식, HLSL API로 읽음(5.6) | R | M |
| screenProbeMaps | 화면 프로브가 쓰는 캐시 항목의 K 경로 지도 아틀라스, 하드웨어 필터 가능(형식·배치는 R의 `ScreenProbes.hlsli`가 정한다; 권장: UAV R32_UINT로 쓰고 SRV R9G9B9E5_SHAREDEXP로 읽기, texel 4 B) (v1.13) | R | M(`SrvCompute`) |
| reflection | RGBA16F (5.6) | R | M |
| reflectionLobeTiles | R8_UNORM ⌈W/8⌉×⌈H/8⌉: 8×8 타일 안 표면 픽셀의 min `reflectionLobeHalfAngle(지각 거칠기, NoV)` / π (R의 `Reflection.hlsli`와 같은 식, 하늘만인 타일 = 1), 재질 해석의 부산물 (v1.3; v1.2의 roughnessTiles 대체) | M | R |
| color | 메인: 7.5 출력, 보조 뷰: RGBA16F 선형 | M | 호출자 / R |

`FrameResources` (프레임당): 대기 LUT 4개·VSM 풀·페이지 테이블·프록셀·프록셀 광원 리스트 [S], TLAS 정적/동적·GI 캐시 [R]. 소비자는 이 참조를 자기 패스에서 `use`하고 인덱스를 커널에 넘긴다.

### 5.2 트랙 진입점과 순서

`FrameRenderer::record`가 설계서 4.1 순서로 부른다(큐 하나, 순서는 선언 의존성으로만 의미가 있다):

```text
M prepareScene(fc)                  장면 텍스처 적재·재질 레코드 게시 (어떤 프레임 상수보다 먼저, v1.11)
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
- **coverage 모드(v1.26, 설계 개정 1 요청 4절; 시그니처 확정, V 구현은 새 coverage 층 뒤)**: `DepthRasterRequest::coverage = true`는 보존 래스터이고, 요청의 뷰에서 대역 B인 클러스터만 그린다. 대역은 뷰의 텍셀로 판정한다(`RasterView::lodPixelsPerMetre`): A ≥ 1.5텍셀, B 0.25~1.5, C < 0.25(요청자의 브릭 march). 픽셀 커널은 `DEPTH_RASTER_COVERAGE 1`을 정의하고 `DepthRaster.hlsli`를 include한다. `DepthRasterCoverage depthRasterCoverage(DepthRasterPixel p)` = `{ float area; uint mask; float depth; }`다.
  - area: 가까운 평면으로 자른 삼각형∩텍셀의 정확 면적(텍셀 단위). 알파 테스트 재질은 cutout coverage를 곱한다(통과 부표본 비율).
  - mask: 32 부표본(`coverageSample`).
  - depth: 덮인 영역 무게중심의 device depth.
  - area 0이면 그 텍셀에 기여가 없다(쓰지 않고 반환).
  - 알파 판정이 안에 있어 `depthRasterCovered`는 필요 없다. 깊이 대상은 쓰지 않는다(픽셀 커널 필수).
  - `DepthRasterRequest::bands`(1 = A, 2 = B, 4 = C, 기본 7 = 지금처럼 모든 대역을 깊이로)는 투과율 층 VSM이 A를 깊이로, B를 coverage 모드로, C는 march로 나눌 때 쓴다.
  - V 구현 전에는 `coverage = true`나 `bands != 7`이 fail한다(조용히 빈 결과를 내지 않는다). V 컴파일 검사 커널은 `Passes/Visibility/Tests/TestCoveragePixel.hlsl`이다.

`fc.services.rasterizeDepth(fc, DepthRasterRequest)`: V가 자기 클러스터 경로(컬링·LOD·변형·메시 셰이더)로 `instanceMask`에 맞는 인스턴스를 `views`마다 래스터한다. 출력은 요청자가 정한다:
- `depthTarget`에 하드웨어 depth(D32, GREATER_EQUAL, reversed-Z) — 뷰포트는 `RasterView::viewport*`.
- 또는 요청자의 픽셀 커널(`pixelKernel`, 예: 페이지 테이블 간접 + 원자적 깊이 쓰기)과 그 커널이 쓰는 자원(`textureUses`, `bufferUses`). 이때 렌더 타깃·깊이 없는 UAV 전용 래스터(1 표본)이고, 뷰포트는 16384²까지 된다. 루트 상수 0~15는 V, 16~31은 요청자(`pixelConstants`).
- **픽셀 커널 입력(v1.1)**: `struct DepthRasterPixel`(`Passes/Visibility/DepthRaster.hlsli`, V 소유) = `SV_Position`, `float2 uv : TEXCOORD0`, `nointerpolation uint userData : USERDATA`(RasterView::userData), `nointerpolation uint material : MATERIAL`(인스턴스 교체 적용된 재질), `nointerpolation uint instance : INSTANCE`(v1.4, 장면 인스턴스 번호). 커널은 무엇이든 쓰기 전에 `depthRasterCovered(p)`를 부르고 거짓이면 `discard`한다(alpha-tested 재질이 본 뷰·기준과 같은 모양으로 잘림).
- **LOD 척도**: `RasterView::lodPixelsPerMetre`. 원근 viewProj면 거리 1에서의 미터당 텍셀(초점 거리), 직교 viewProj(마지막 행 (0,0,0,1), V가 판별)면 거리와 무관한 미터당 텍셀(S는 1/τ_k).
- **컬 모드(v1.1)**: `DepthRasterRequest::cull`, 기본 `D3D12_CULL_MODE_NONE`(양면; 그림자). `BACK`이면 one-sided 재질만 뒷면을 버린다(two-sided 재질은 늘 양면).
- **타일 마스크 컬링(v1.1, S 요청)**: `DepthRasterRequest::cullMask`(raw 버퍼, 같은 프레임의 앞선 패스가 GPU로 씀)와 `cullTilePx`, 뷰마다 `RasterView::cullMaskOffset`(단어 위치, `UINT32_MAX` = 마스크 없음). 뷰마다 ⌈w/tile⌉×⌈h/tile⌉ 비트, 행 우선, 비트 i = 단어 `offset + (i >> 5)`의 `(i & 31)`번째, 1 = 래스터 필요. V는 뷰포트 투영 사각형이 켜진 비트를 하나도 덮지 않는 클러스터(가능하면 메시렛 삼각형)를 버리고, 필요하면 내부에서 계층 마스크를 만든다. **성능용 필터**이고 정확성은 요청자의 픽셀 커널이 지킨다(V가 보수적으로 더 그려도 결과는 같다). V는 `cullMask`를 `SrvCompute`/`SrvGraphics`로 선언한다. v1.7부터 V는 8×8 타일 요약 마스크를 만들어 인스턴스·계층 노드·클러스터 모두를 마스크로 정확히 거른다(이전의 "64 타일 넘는 사각형은 그린다" 생략 없음).
- **타일 국소 래스터(v1.7, S 요청 `20260925_S_page_local_raster.md`)**: `DepthRasterRequest::tileLocal = true`(마스크 필요). 클러스터마다 그 사각형 아래 켜진 타일의 행별 연속 구간마다 한 번(사각형 아래 타일이 모두 켜졌으면 한 번) 그리고, 메시 셰이더가 그 구간 사각형 밖 삼각형을 버리고 나머지를 클립 거리 4개로 잘라 래스터한다. 래스터라이저는 켜진 타일 안에서만 fragment를 만든다(fragment 수 = Σ 삼각형 ∩ 켜진 타일 면적). 픽셀 위치·`DepthRasterPixel`은 같고, 켜진 타일 안의 fragment 집합(픽셀별 개수)은 마스크만 쓴 래스터와 같다. **깊이**는 하드웨어가 타일 경계에서 만든 꼭짓점을 1/256 px 격자에 맞추므로 그 삼각형의 깊이 평면이 반올림 수준으로 다르다: 실측 최대 차이는 깊이 기울기 × 0.0068 px(측면 변위로 1/64 px 이내, V 테스트 `raster_service`). 비용식: 쌍 수 × 메시렛 launch + 삼각형 수 / 래스터 처리율 + Σ(삼각형 ∩ 켜진 타일 면적) fragment.
- **타일 아틀라스(v1.32, S 요청 `20260925_S_vsm_depth_atlas.md`, 설계 개정 11.4 (6))**: `DepthRasterRequest::atlasSlots`(raw 버퍼, 마스크 비트마다 uint 하나)와 `atlasTilesPerRow`. 조건은 `tileLocal`, `cullMask`, `depthTarget`이다.
  - 켜진 타일마다 따로 그린다(쌍 = (클러스터, 타일 하나)). 뷰의 타일 i(마스크 비트 i)의 슬롯은 `atlasSlots`의 단어 `cullMaskOffset × 32 + i`이다. 슬롯의 아틀라스 위치는 (슬롯 % `atlasTilesPerRow`, 슬롯 / `atlasTilesPerRow`) × `cullTilePx` 픽셀이다.
  - 메시 셰이더가 타일의 픽셀을 슬롯의 픽셀로 옮긴다. 옮기는 양은 정수 픽셀이고, 클립 공간의 선형식 x' = x·s + w·o다. z와 w는 그대로여서 깊이와 원근은 같다. 클립 거리 4개가 타일 경계를 자른다. 패스 뷰포트는 아틀라스 전체이고, 뷰의 뷰포트 위치는 무시한다(뷰포트 크기가 타일 격자를 정한다).
  - 깊이 대상 형식은 `D32_FLOAT` 또는 `D16_UNORM`이고 요청자가 요청마다 고른다(VSM: 천정각 < 76°면 D16, 설계 개정 11.4 (6)). 이 규칙은 모든 요청에 적용된다(v1.32부터 서비스가 대상의 형식으로 파이프라인을 고른다). 아틀라스를 지우는 것(깊이 fast clear)은 요청자가 한다.
  - 픽셀 커널은 선택이다. `[earlydepthstencil]`이면 깊이 테스트 뒤에 돈다(바람 메타 같은 것). 커널이 보는 `SV_Position`은 아틀라스 픽셀이다.
  - 정확도 [실측, V 테스트 `raster_service_tile_atlas`, 1024² 직교 지형, 128 px 타일 30개를 3 × 11 슬롯에 섞어 배치]: 슬롯의 깊이는 타일 국소 래스터와 텍셀마다 같다. 491,520텍셀 중 87.1 %가 비트 단위로 같고, 나머지는 모두 국소 깊이 기울기 × 2/256 px 안이다(최대 0.67 스냅 단계; 래스터라이저가 p + k의 float 좌표를 1/256 px로 스냅하므로 p를 옮긴 것과 반올림이 다르다). 가장자리 뒤집힘 0, 그 밖 0. D16 슬롯은 D32 슬롯과 최대 8.6e-6(반 단계) 차이다. 쓰지 않은 슬롯에 쓰인 텍셀은 0이다.
  - 비용식: 쌍 수 × 메시렛 launch + Σ(삼각형 ∩ 켜진 타일 면적) fragment × c_rop(설계 벤치 0.010 ns/fragment, D32, PS 없음). 실제 VSM 부하(매 프레임 전부 다시 그리기)의 c_rop와 블록 계층은 S가 잰다(설계 개정 12절).
  - [실측, v1.33, V 게이트 `--service local,atlas16,atlas32 --service-pages ring`, city_block, GPU 잠금 timing, 300프레임 중앙값] 합성 부하는 움직이는 태양의 수신자 요청이다. 단 k마다 지면 점이 카메라 절두체 안이고 거리가 [d_k, 2 d_k)인 페이지를 요청한다(d_k = 텍셀 / 픽셀각). 12단이고 4K 6,758페이지 = 110.7 M 텍셀로, S 실측 3,744의 1.8배다.

    | 경로 | 4K 래스터 | 1440p 래스터 |
    |---|---|---|
    | 타일 국소 + 원자 max 픽셀 커널(지금 S 경로의 V 쪽 모사) | 2.418 ms | 1.150 ms |
    | 아틀라스 D16 | 1.095 ms | 0.573 ms |
    | 아틀라스 D32 | 1.111 ms | 0.573 ms |

    4K 아틀라스는 요청 텍셀당 0.010 ns로 설계 벤치의 c_rop와 같다. 아틀라스 clear는 0.06 ms다.
- **(v1.33) 타일 마스크 존재 판정**: 인스턴스·노드 컬링의 마스크 검사는 이제 "범위 안에 켜진 타일이 있는가"만 판정한다(`tileAnySet`). 범위 안에 온전히 든 켜진 요약 칸에서 곧바로 참이고, 범위 경계에 걸린 칸만 행으로 본다. 이전에는 클러스터 쌍 계산용 `tileVisit`(켜진 타일 전부를 세는 걸음)을 불러, 큰 구가 스레드 하나에서 수천 번 돌았다. 출력은 같다(가시 클러스터 17,681, 타일 쌍 154,258).
  - [실측, 위와 같은 4K 부하] 서비스 컬링 합: 0.790 → 0.282 ms. 인스턴스 0.125 → 0.012, 노드(5단계) 0.44 → 0.05, 클러스터 0.19(쌍 계산·쓰기, 그대로).
  - 설계 개정 11.4 (2)의 방향 독립 1회 순회는 단별로 반복되는 인스턴스·노드·클러스터 판정을 합친다. 그 몫이 단 수에 비례하는지 V 게이트로 쟀다(v1.34, `--service-levels N --service-spacing s`, 4K city 링 부하, 아틀라스 D32):
| 단 수 × 간격 | 요청 페이지 | 컬링 합 | 인스턴스 | 노드 | 클러스터 |
    |---|---|---|---|---|---|
    | 12 × 2 | 6,758 | 0.286 ms | 0.013 | 0.058 | 0.189 |
    | 23 × √2(같은 범위) | 5,767 | 0.224 ms | 0.012 | 0.058 | 0.128 |

    같은 거리 범위를 두 배 많은 단으로 덮어도 단별 반복 몫(인스턴스 + 노드 0.07 ms)은 늘지 않았다. 페이지(−15 %)와 클러스터 쌍 몫은 줄었다. 비용은 단 수가 아니라 요청 페이지와 그 위 클러스터에 비례한다. 40단(√2, 더 넓은 범위)과 20단(× 2)은 아틀라스가 8,192페이지를 넘어 한 장에 들지 않아 이번 게이트에서 실패했다. 16,384페이지까지 받게 고친 게이트로 다시 잰다(용량을 올린 채).

### 5.4 R → V·M·S: 평면 반사 뷰

1. R이 반사면(평면 `float4 plane`)과 그 화면 사각형(A_r)을 정해 `ViewDesc::planarReflection(main.view, plane, x, y, w, h)`로 뷰를 만든다: 카메라를 평면에 대칭, 투영을 사각형으로 잘라낸 비대칭 절두체, `clipPlane = plane`(원래 카메라 쪽 유지), `mirrored = true`.
2. `ViewResources v = fc.services.renderView(fc, view)`: 코어가 V `visibility` → M `materialResolve` → S `shadowVisibility` → M `shading`을 그 뷰에 기록한다. `v.color`는 RGBA16F 선형(광도 × 노출), 크기 w × h.
3. V는 클립 평면을 `SV_ClipDistance0`과 클러스터 컬링으로 지키고 `mirrored`면 컬링 면을 바꾼다. M은 `viewKind == VIEW_PLANAR_REFLECTION`이면 화면 프로브·반사 결과 대신 R의 GI 캐시 API(5.6)를 쓰고, 반사를 재귀하지 않는다(거친 반사는 캐시 K 경로).
4. 원본 기하 그대로(캐릭터 원본 메시, 바람 적용 잎, coverage 층)라 직접 시야와 같다(설계서 2.6). R은 `v.color`를 자기 반사 결과에 합성한다.

### 5.5 프레임 상수 (b1, `Frame.hlsli` ↔ `gpu::FrameConstants`, 544 B)

뷰 행렬 5개(row_major), 카메라 위치·near, 클립 평면, 뷰 크기·종류·프레임 번호, 시간·dt·노출(`1/(1.2·2^EV100)`)·tan(fov/2), 태양 방향·조도(lux, 대기 위)·색·각반지름, 바람 방향·속도, 장면 버퍼 bindless 인덱스(인스턴스, 메시, 서브메시, 정점, 인덱스, 클러스터, 클러스터 정점 인덱스, 클러스터 삼각형, LOD 레벨, 재질, 재질 리매핑, 광원, 스킨, 본 팔레트 현재/이전, 재질 모델 LUT, LOD 레벨 클러스터 목록 `g_lodLevelClusters`(v1.1, 예비 칸 사용, 크기 불변)), 개수, 장면 revision. 패스는 `c.bindFrameConstants(view.frameConstants)`로 묶는다.
- (v1.34) 마지막 행(16 B, 528 → 544 B): `coverageMaskLut`(coverage 마스크 LUT SRV, 5.5.1), `giRaysThisFrame`(이 프레임의 GI 광선 몫, 설계 개정 10.3. 배분은 R의 GiSystem이 `FrameContext::gpuSimulation`으로 하고, 이 칸은 R이 진단용으로 채운다. 코어는 0을 쓴다), 예비 2칸. HLSL은 `g_coverageMaskLut`, `g_giRaysThisFrame`이다. 앞 필드의 오프셋은 바뀌지 않는다.

**트랙 지속 상태(v1.1)**: `FramePassContext::state<T>("<트랙>.<이름>")`은 `FrameRenderer`가 소유한 저장소(`TrackState`)에서 T를 처음 쓸 때 만들고, 렌더러 파괴 때 GPU 유휴를 기다린 뒤 없앤다(히스토리 버퍼, 풀, 캐시). 키 하나에는 늘 같은 타입을 쓴다. 렌더러 없이 만든 컨텍스트에서는 실패한다. 트랙이 이미 쓰는 장치 키 레지스트리(`DeviceState.h`)는 그대로 둬도 되고, 수명 훅이 필요하면 이것으로 옮긴다.

### 5.5.2 이력 불연속과 결정성 (v1.35, I 요청 `20260925_I_history_discontinuity.md`, S·R·M 목록)
- **신호**: 호스트가 사건 뒤 첫 프레임에 `FrameContext::discontinuity`를 켠다. I의 ABI는 `UnxFrameSetDiscontinuity(r, flags)`이고, 떨어진 패킷의 비트는 다음 패킷에 OR로 합쳐진다.
  - `kDiscontinuityRestore`: World 스냅숏 복원, 세이브 로드, 분기 변경이다. 호스트는 committed World의 epoch·branch·StateGeneration이 바뀌거나 tick이 뒤로 가는 것으로 안다.
  - `kDiscontinuityCut`: 카메라 컷이다. 게임 코드가 `UnravelNextRenderer.MarkDiscontinuity(Cut)`을 부른다. 카메라 점프를 추정해서 감지하지는 않는다.
  - 개체 하나의 순간이동은 전역 신호가 아니라 인스턴스별 `InstanceTransformUpdate::flags = kTransformTeleport`다(6.3).
- **코어가 하는 것**:
  - 두 비트 모두: 메인 뷰 `prevViewProj = viewProj`(카메라 움직임 0).
  - Restore: `GpuScene::resetMotion()`으로 이 프레임의 이전 변환과 이전 팔레트를 지금 것으로 둔다(개체 움직임 0).
- **트랙이 하는 것**(시간 상태 목록, 각 트랙 제출):

  | 트랙 | 상태 | Restore | Cut | 재수렴 |
  |---|---|---|---|---|
  | S | VSM 태양·국소광 페이지 캐시, 움직이는 태양 스케줄 | 버림 | 버림(시점 페이지) | 1프레임. 신호 프레임에는 전부 다시 그리므로 지금 경로에서 4K 9.4 ms 스파이크가 생긴다. 한 경로 VSM(매 프레임 전부 다시 그리기)이면 이 항목과 스파이크가 없다 |
  | S | 대기 표(투과율, J_ms, 하늘 뷰) | 유지(매질·태양·고도의 순수 함수) | 유지 | 0 |
  | S | 날씨의 여러 프레임 분할 재구축 | 버리고 새 입력으로 한 프레임에 | 같음 | 1 |
  | R | GI 월드 캐시(4K 138.8 MB) | 버림 | **유지**(세계 공간, 시점 무관) | 1~13프레임(백색로 13, 열린 하늘 2, 햇빛 지면 1; 한도 `gi.relight_frames_max` = 8) |
  | R | 반사 거리 이력(16.6 MB), 화면 프로브 | 버림 | 버림 | 1 |
  | R | RayScene 프록시 절단 이력·정확 집합 | 현재 거리로 다시 정함 | 같음 | framesInFlight + 1 |
  | M | 없음(모든 패스가 그 프레임 입력만 읽는다) | — | — | 0 |

- **같음의 수준(계약)**:
  1. 같은 GPU·드라이버에서 Restore 신호 뒤 입력 열이 같으면 결정적 부분집합(아래 결정성 항의 원천을 뺀 것)은 비트 동일하다. M·S는 지금 그렇다(M 확인, S는 VsmTests의 discontinuity 절로 실측 예정).
  2. 전체 렌더러의 비트 동일(**사용자 결정, v1.36**: 결정 모드 스위치로 두고, 합계표에 여유가 확인되면 늘 켬으로 올린다):
     - 결정 모드(R의 품질 키 `gi.deterministic` 등, 시험과 재생 기록)에서는 Restore 뒤 입력 열이 같으면 전체 렌더러가 비트 동일하다.
     - 결정 모드가 꺼진 게임 중 Restore는 재설정 뒤 K(≤ 13)프레임 재수렴으로 같아진다(아래 3). 영상 품질은 두 모드에서 같고, 동률 처리의 결정성만 다르다.
     - 배경: R GI의 비결정 원천은 세 가지다: 원자 free list 순서에서 나온 광선 seed, 갱신 선택의 원자 선착순, 용량 압박 때 할당 순서. 결정적 대안(64-bit 키 seed, hash 우선순위 radix select)의 비용은 평시 +0.03~0.05 ms, 압박 장면 +0.08 ms[R 예상]이다. 평면 뷰 대 광선 선택은 실측 GPU 시간 적합을 결정 모드에서 기기별 고정 계수로 바꾼다. 선택지: (i) 늘 켬, (ii) 결정 모드 스위치(시험·재생 기록에서 켬). 합계표의 여유(숲·수변 목표선 ±0.06)와 함께 설계 개정·사용자 결정으로 정한다.
  3. 신호 뒤 K프레임이 지나면 영상은 신호 없이 이어진 실행과 기준 비교 한도(3.4) 안이다. K = 트랙 재수렴의 최댓값이다(지금 R GI 13).

### 5.5.1 V ↔ M 경계 (v1.2)
- **V가 낸다**: vis id·depth·visible clusters·HiZ(대역 A), coverage 층(대역 B/C) fragment 목록 — 픽셀별 깊이 순 정렬, fragment마다 정확 면적·32-부표본 마스크·vis id(7.1). V는 fragment를 셰이딩하지 않는다.
- **M이 한다**: 재질 해석(G-buffer), 셰이딩, 가장자리 픽셀(E: 3×3 identity 2개 이상) 검출과 그 픽셀의 해석적 coverage, coverage fragment 셰이딩, 깊이 순 합성, 톤맵 → `color`. E·coverage 합성은 셰이딩 뒤 최종 합성으로 M의 `shading` 진입점 안에 있다.
- **공유 기하 함수**: 삼각형∩픽셀 사각형의 정확 면적과 32-부표본 마스크는 V의 `Passes/Visibility/Coverage.hlsli`(V 소유, 대역 B 래스터와 M의 E 합성이 같은 함수를 쓴다)에 둔다. 시그니처(v1.16): `float coverageTriangleArea(float2 a, float2 b, float2 c, float2 pixel)` — 화면 픽셀 좌표의 삼각형과 픽셀 사각형 [pixel, pixel + 1)²의 교집합 면적(네 변에 대한 Sutherland-Hodgman 클리핑 + shoelace, 감김 무관, float 반올림까지 정확; M 테스트가 해석해와 비교해 오차를 보고한다) · `uint coverageTriangleMask(float2 a, float2 b, float2 c, float2 pixel)` — 32 부표본(`coverageSample(i)`: x = (i + 0.5)/32, y = 비트 반전 i/32 + 1/64, 32×32 격자의 행·열마다 하나) 중 삼각형 안의 것 · `COVERAGE_SAMPLES` = 32.
- (v1.34, 설계 개정 11 b) `uint coverageTriangleMaskLut(float2 a, float2 b, float2 c, float2 pixel, StructuredBuffer<uint> lut)`: 같은 마스크를 변 마스크 3개의 AND로 낸다. 변마다 표 한 번 조회이고, `lut`는 `ResourceDescriptorHeap[g_coverageMaskLut]`다.
  - 표는 각 64 × 거리 64(`GpuScene.h` `coverageMaskTable()`, 16 KB)다. 칸 (k, j)는 안쪽 법선 각 φ = (k + ½)π/64 ∈ [0, π)와 픽셀 중심의 부호 거리 h = −R + (j + ½)·2R/64(R = √2/2)이다. 비트 i는 dot(n, x_i − 중심) ≥ −h이다.
  - 법선을 [0, π)로 접을 때는 거리를 반대로 하고 마스크의 보수를 쓴다.
  - 칸 안에서 변에 아주 가까운 부표본은 반대쪽으로 갈 수 있다. `coverageTriangleMask`는 정확한 기준으로 남는다.
  - 품질 게이트 |coverage 차| P99 ≤ 1/32 [실측, V 테스트 `coverage_mask_lut_matches_exact`, 무작위 삼각형 786,432개, 픽셀을 자르는 것만]:
- 정점 0.75 px 안: 픽셀을 자르는 삼각형 220,101개, 정확 마스크와 다른 것 0, 정확 검사 부표본은 교차 삼각형당 2.84개(최대 14)
    - 3 px 안: 122,718개, 다른 것 0, 1.88개(최대 12)
    - 30 px 안: 15,893개, 다른 것 0, 1.50개(최대 10)
    - 처음 설계대로 칸 중심 마스크 표(64 × 64)만 쓰면 이 게이트를 통과하지 못했다: P99 2/32, 최대 5/32. 그래서 칸마다 "확실히 안 / 확실히 밖" 보수적 마스크 두 개(1/128 px 여유, 32 KB)를 두고, 남는 모호 부표본만 `coverageTriangleMask`와 같은 식으로 검사한다. 결과는 비트 단위로 같다.
  - 속도 [실측, forest_card 4K, coverage 층 켬, V 게이트, GpuLock timing, 300프레임 중앙값]: coverage 래스터 4.487 → 2.940 ms(−34 %, fragment 1.18 M로 같음, 3.79 → 2.48 ns/fragment). V의 coverage 래스터 PS와 깊이 래스터 coverage 모드(`coveragePolygonMask`)가 이것을 쓴다.
- M은 V의 vis buffer가 나오기 전까지 자기 테스트의 가짜 vis buffer(같은 형식, 7.1)로 개발한다. V는 출력이 준비되면 알린다.

### 5.6 공개 HLSL API (고정된 이름·시그니처)

코어 (`Passes/Common/`, 코어 소유): `Bindless.hlsli`(P[8], 샘플러), `Frame.hlsli`, `Scene.hlsli`(레코드와 `loadInstance/Mesh/Submesh/Cluster/LodLevel/Material/Light`, `loadVertex(mesh, v)`, `loadTriangle(mesh, t)`, `loadClusterTriangle(c, t)`, `octEncode/octDecode`, `transformPoint/Vector`), `VisBuffer.hlsli`, `GBuffer.hlsli`, `Deformation.hlsli`(`deformVertex(inst, mesh, v)`), `MaterialModel.hlsli`(`modelEvaluate` 등, 8.1).

각 트랙이 자기 폴더에 만들어야 하는 헤더 (소비자가 이 이름으로 include한다; 인덱스 묶음 struct는 소비자가 `c.srv(...)`로 채워 루트 상수로 넘긴다):

| 헤더 | 제공 | 필수 함수 |
|---|---|---|
| `Passes/Atmosphere/Atmosphere.hlsli` | S | `struct AtmosphereSrvs { uint transmittance, multiScatter, skyView, aerial; };` `float3 atmosphereSkyRadiance(AtmosphereSrvs, float3 worldDir)`(태양 원반 제외) · `float3 atmosphereSunRadiance(AtmosphereSrvs, float3 worldPos)`(원반 복사휘도, 투과 포함) · `void atmosphereAerial(AtmosphereSrvs, float2 uv, float linearDepth, out float3 inscatter, out float3 transmittance)`(v1.15: 완성된 공기 합성 — 대기 단일·다중 산란, 캐스터가 그림자를 드리운 공기, 국소광의 공기 산란; 공기 볼륨 3D fetch 2회) · `void atmosphereAirView(AtmosphereSrvs, float2 uv, float linearDepth, out float3 inscatter, out float3 transmittance, out float3 sunIlluminance)`(v1.15: 위에 더해 표면점의 비차폐 태양 조도 lux, fetch 1회 더; 메인 뷰 픽셀용) · `atmosphereSunIlluminance`(임의 위치용, R) |
| `Passes/Atmosphere/Froxel.hlsli` | S | `struct FroxelSrvs { uint lights, lightIndices, scattering, pad; };` `uint2 froxelLightRange(FroxelSrvs, uint2 pixel, float linearDepth)`(offset, count) · `uint froxelLight(FroxelSrvs, uint i)`(v1.19: 마스크된 광원 인덱스; 항목의 bit 15는 S의 그림자 슬롯 유무 — `froxelLightShadowed`, 광원 한도 32767) (v1.15: `froxelScattering` 삭제 — 공기 산란은 `atmosphereAerial`/`atmosphereAirView`에 포함) |
| `Passes/Shadow/ShadowVisibility.hlsli` | S | `float shadowSlot(uint packed, uint slot)`(7.3 해독) · `uint shadowSlotOfLight(FroxelSrvs, uint2 pixel, float linearDepth, uint lightIndex)`(1~3 또는 0xFFFFFFFF) · `struct ShadowSrvs { uint pageTable, pool, blocks, searchBound; uint constants, lights, pad0, pad1; };`(v1.18) · `float shadowSunVisibilityAt(ShadowSrvs, float3 worldPos, float3 normal, float footprint, out bool resident)`(v1.18: 광선 hit의 태양 가시성, 직접 뷰 가시성 패스와 같은 추정량, footprint에 맞는 단 또는 세 단 더 촘촘한 단의 상주 페이지; 없으면 resident = false로 R이 그림자 광선; 채우는 값은 `FrameResources::vsmPageTable/vsmPool/vsmBlocks/vsmSearchBound/vsmConstants`) `float shadowSunTransmittanceAt(ShadowSrvs, float3 worldPos, float footprint, float reach)`(v1.26: VSM 투과율 층. 단 k = footprint의 단, 밉 = log2(reach / 텍셀)의 블록 프로파일 1탭(2×2, 높이 보간). 층 없는 페이지와 페이지 없는 곳은 1이다. `shadowSunVisibilityAt`과 가시성 슬롯 0은 V_opaque × T를 돌려준다. `ShadowSrvs.pad1` → `layers` = `FrameResources::vsmLayers`) · `float shadowVisibilityDirect(ShadowSrvs, uint lightIndex, float3 worldPos, float3 normal)`(뷰 픽셀이 없는 호출자용 느린 경로; v1.19: 가시성 슬롯과 같은 추정량을 가장 촘촘한 상주 mip으로 계산하고, `ShadowSrvs.lights` = `FrameResources::vsmLocalLights`, `.pad0` = `vsmSlotOfLight`) · v1.21 픽셀 기준(오버플로 fallback이 슬롯·오버플로 값과 비트 단위로 같다): `struct ShadowPixelReceiver { float3 world; float footprint; float3 normal; uint valid; };` `ShadowPixelReceiver shadowPixelReceiver(uint2 pixel, uint depthSrv, uint gbufferSrv)`(바인딩된 프레임 상수의 뷰, 깊이에서 위치·기하 법선, 픽셀 footprint m; 하늘·뷰 밖 valid = 0) `float shadowLocalVisibilityAtReceiver(ShadowSrvs, uint lightIndex, ShadowPixelReceiver)`(슬롯 1~3·오버플로와 같은 계산, 같은 양자화 `round(saturate(v)·255)/255`; 그림자 슬롯 없는 광원 1; `searchBound`는 읽지 않음) `float shadowLocalVisibilityAtPixel(ShadowSrvs, uint lightIndex, uint2 pixel, uint depthSrv, uint gbufferSrv)`(둘을 합친 것) |
| `Passes/GI/GiCache.hlsli` | R | `struct GiSrvs { uint cache, hash, pad0, pad1; };` `float3 giCacheIrradiance(GiSrvs, float3 worldPos, float3 normal)` · `float3 giCacheRadiance(GiSrvs, float3 worldPos, float3 dir, float coneHalfAngle)` |
| `Passes/GI/ScreenProbes.hlsli` | R | `struct ProbeSrvs { uint probes, occlusion, pad0, pad1; };` `float4 screenProbeIrradiance(ProbeSrvs, uint2 pixel, float3 normal, float linearDepth)`(rgb 조도, a 근거리 가림 0~1) · `float3 screenProbeRadiance(ProbeSrvs, uint2 pixel, float3 normal, float linearDepth, float3 dir, float coneHalfAngle)`(v1.2: 4프로브 보간, 반각 coneHalfAngle 원뿔로 prefilter한 입사 복사휘도, nit) |
| `Passes/Material/MaterialTextures.hlsli` | M (v1.11) | 게시된 텍스처 형식(6.3) · `MATERIAL_TEXTURE_*` 비트 · `float4 materialBaseColorGrad(GpuMaterial, float2 uv, float2 duvdx, float2 duvdy)` · `float4 materialBaseColorLevel(GpuMaterial, float2 uv, float lod)`(선형 rgb, a = coverage, 텍스처 없으면 1; clamp 비트에 따라 `g_anisoClamp`/`g_anisoWrap`). V의 알파 테스트(`AlphaTest.hlsli`)가 이것으로 읽는다 |
| `Passes/Reflection/Reflection.hlsli` | R | `float4 reflectionRadiance(uint reflectionSrv, uint2 pixel)`: rgb = 반사 lobe로 정규화 적분한 입사 복사휘도(G/M 경로 결과, M이 방향 알베도 항을 곱한다), a = 1이면 유효 · `float reflectionLobeHalfAngle(float perceptualRoughness, float NoV)`(v1.2: 설계서 2.6의 θ_r,narrow, M과 R이 K/G/M을 같은 식으로 가른다). **K 경로(v1.2)**: `a == 0`인 본 뷰 픽셀은 M의 셰이딩 커널이 `screenProbeRadiance(probes, pixel, n, depth, reflect(-v, n), reflectionLobeHalfAngle(r, NoV))`에 재질 모델의 스페큘러 방향 알베도 항을 곱해 평가한다. 보조 뷰(평면 반사)는 `giCacheRadiance` |

## 6. 장면 데이터

### 6.1 규약 (`Native/Core/include/unx/core/Math.h`)
오른손 좌표, +Y 위, 미터. 뷰 공간은 −Z를 본다. 행렬은 행 우선 저장, 열 벡터(`p' = M p`, HLSL `row_major` + `mul(M, v)`). 투영은 reversed-Z 무한 원평면(깊이 = near / 거리, 지우기 0, `GREATER_EQUAL`). 인스턴스 변환은 회전·균일 스케일·평행이동만(`scene::validate`가 검사; 법선은 같은 행렬로 변환). 반시계가 앞면.

### 6.2 CPU 장면 (`Native/Scene/include/unx/scene/SceneData.h`, 파일 `.unxscene` v1)
텍스처(mip 0, 형식 6종), 재질(클래스, baseColor, roughness(지각), metallic, specular, emissive(nit), alphaCutoff, transmission, ior, twoSided, 텍스처 5종), 메시(위치·법선·탄젠트(노멀맵이면 필수)·uv0·인덱스·서브메시·스킨 스트림), 인스턴스(메시, 변환, 플래그 CastShadow/Dynamic/Skinned/Wind, 스켈레톤, 바람, 서브메시별 재질 교체), 스켈레톤 포즈, 광원 6종, 태양, 대기(이전 엔진 수식·기본값), 바람, 카메라, 카메라 경로. `serialize`는 결정적이고 `contentHash`(SHA-256)가 장면 identity다. `validate`가 구조 규칙을 검사한다.

### 6.3 GPU 장면 (`GpuScene.h`, `GpuSceneLayout.h` ↔ `Scene.hlsli`)
- (v1.35) `InstanceTransformUpdate::flags`의 `kTransformTeleport`: 그 인스턴스는 이 프레임에 `prevObjectToWorld = objectToWorld`(움직임 0)다. I의 `UnxTransformUpdate` 예약 칸이 `UNX_TRANSFORM_TELEPORT`로 이어진다. 같은 프레임에 순간이동 뒤 다시 움직이면 이전 = 순간이동 위치다. `GpuScene::resetMotion()`은 이 프레임에 바뀐 모든 인스턴스의 이전 변환과 이전 팔레트를 지금 것으로 둔다(Restore, 5.5.2). `GpuScene::palette(instance)`(R 요청)는 이번 프레임 본 팔레트의 CPU 사본이다. 조인트마다 float4 3행(행 우선 3 × 4, jointToModel × inverseBind)이고, 다음 `updateSkeleton`까지 유효하다. [실측] 단위 테스트 `gpu_scene_frame_updates`(순간이동, resetMotion, 팔레트 접근자).
`GpuScene::upload(scene)`가 레코드를 채운다(코어 소유). 크기 고정: Instance 144 B, Mesh 80 B, Submesh 16 B, Vertex 32 B(v1 비압축; `loadVertex`로만 읽으므로 V/M이 압축해도 인터페이스 변경 아님), SkinVertex 16 B, Cluster 64 B, LodLevel 16 B, Material 80 B, Light 80 B, VisibleCluster 8 B. 클러스터 계층은 V의 빌더(`Tools/ClusterBuilder`)가 `GpuScene::setClusters(ClusterData)`로 넣는다. `GpuScene::clusters()`는 그 CPU 사본(메시별 `clusterOffset/Count`, `lodLevelOffset/Count`, `lodLevelClusters`), `GpuScene::srv(name)`은 클러스터 버퍼와 V 내부 버퍼(`ClusterData::named`)의 bindless SRV다(v1.1). R은 `GpuScene::buffer("vertices" | "indices" | "clusters" | "clusterVertexIndices" | "clusterTriangles" | "lodLevels" | "lodLevelClusters" | ...)`와 메시의 원본 삼각형(`indexOffset`, `triangleCount`) 또는 LOD 레벨의 클러스터로 BLAS를 만든다.

**재질 텍스처(v1.10, M 요청 `20260925_M_material_textures.md`)**: M의 텍스처 시스템이 `GpuScene::setMaterialTextures(std::vector<gpu::MaterialTextures>)`로 재질마다 bindless SRV `{baseColor, normal, roughMetal, emissive, occlusion}`와 clamp 비트(`MaterialTextureBit`)를 게시한다. 코어는 재질 버퍼를 다시 올린다(새 SRV). 바뀐 재질의 `revision`과 장면 `revision()`을 올린다. SRV 수명은 M이 소유한다. `gpu::Material::textureClamp`(이전 `pad0`)의 비트 1 = clamp 주소(`g_anisoClamp`), 0 = wrap. 텍스처 형식(알파 coverage 보존 밉 등)은 M의 공개 헤더 `Passes/Material/MaterialTextures.hlsli`(5.6)가 정한다. 프레임 상수에 재질 버퍼 SRV가 들어가므로, 게시는 그 프레임의 어떤 프레임 상수보다 먼저 한다. M의 진입점 `tracks::prepareScene(fc)`에서 하고, `FrameRenderer::record`가 맨 처음 부른다(v1.11). `FramePassContext`를 직접 만드는 테스트 프레임은 `prepareScene`을 한 번 부르면 텍스처가 게시된다.

**프레임 갱신(v1.8, I 요청)**: 호스트는 렌더 프레임 f를 기록하기 전에 `updateTransforms(f, {instance, objectToWorld}[])`, `updateSkeleton(f, skeleton, jointToModel[])`, `setInstanceVisible(instance, bool)`을 부른다. `FrameRenderer::record`가 맨 처음 `flushUpdates`로 반영한다. 반영은 그래픽 큐의 산포 커널(`Passes/Common/SceneUpdate`)이 프레임 그래프보다 먼저 하고, 다른 큐는 그 fence를 기다린다. 뜻:
- `prevObjectToWorld` = **직전 렌더 프레임**의 objectToWorld, 이전 본 팔레트 = 직전 렌더 프레임의 팔레트. 이번 프레임에 안 바뀐 인스턴스 중 직전 프레임에 바뀐 것은 prev = current로 맞춘다(멈추면 움직임 0).
- `transformRevision`, `deformRevision`은 바뀐 프레임마다 1씩 오른다(한 프레임에 여러 번 갱신해도 1).
- 숨김: `Instance::flags`의 `gpu::kInstanceHidden`(1 << 31, HLSL `INSTANCE_HIDDEN`). 모든 소비자가 건너뛴다. V는 인스턴스 컬링에서 건너뛰고(주 뷰, 서비스), R은 TLAS에서 뺀다(R이 반영).
- 움직임 끊김(v1.45): 순간이동(`kTransformTeleport`)이나 `resetMotion()`(Restore)으로 이전 변환·팔레트를 지금 것으로 둔 인스턴스는 그 프레임 하나 동안 `gpu::kInstanceMotionBreak`(1 << 30, HLSL `INSTANCE_MOTION_BREAK`)를 달고, `Instance::breakCentre`(옛 pad0..2)에 직전 렌더 프레임에서 경계 구의 월드 중심을 담는다. 움직임은 0이다. 전에 그린 자리에 묶인 캐시(S의 VSM 페이지)는 prev로 점프를 잴 수 없으므로 이 플래그로 옛 자리와 새 자리를 무효화한다. 복원 검사에서 복원 전 자리의 그림자가 남던 원인이다.
- CPU 사본(`instances()`)은 호출 즉시 바뀐다. `revision()`(장면 구조)은 바뀌지 않는다.
- 비용 [예상]: 갱신 원소(16 B) 수 × 2(업로드 링 + 산포). 인스턴스 2만 개와 본 2.56만 개면 약 2.2 MB, 0.01 ms 수준.

**commit 뒤 장면 편집(v1.44, I 요청 `20260926_I_game_features.md` 1·2, D0)**: 호출자는 먼저 자기 장면(`source()`)을 고치고, 바뀐 번호를 알린다.
- `setInstances(indices)`: `source()->instances[i]`에서 레코드를 다시 만든다. i = `instances().size()`면 뒤에 붙인다(순서대로).
  - 변환은 움직임 0이다(prev = current). 메시는 이미 올라간 것이어야 한다(새 메시는 `upload`). 플래그는 장면 값이라 숨김도 풀린다. 재질 교체는 remap 표에 붙인다. 바람 값도 넣는다.
  - 스킨 인스턴스는 추가하거나 갈아 끼울 수 없다(팔레트 자리가 upload에서 정해진다). D0 뒤 항목이다.
  - **제거**는 `setInstanceVisible(false)`다. 빈 번호는 호출자가 관리하고, 재사용할 때 `setInstances`로 갈아 끼운다.
- `setMaterials(indices)`: `source()->materials[i]`에서 값을 다시 만든다(i = 수면 추가). M이 게시한 텍스처 SRV는 M의 텍스처 시스템이 다시 게시할 때까지 유지한다. 장면에 텍스처를 추가했으면 그 텍스처를 쓰는 재질로 `setMaterials`를 부른다.
- 두 호출 모두 `revision()`을 올린다. 그래서 장면에서 파생한 상태를 트랙마다 다시 만든다: V 계층, R 광선 장면(BLAS·TLAS), S 페이지, GI 에포크. M은 텍스처 내용·참조·컷이 바뀌었을 때만 다시 올린다. 정확성 우선이다. 편집 한 번의 비용(주로 R 광선 장면 재구축)은 실측으로 적는다. 인스턴스 집합만 바뀐 경우에 트랙별로 더 빠른 경로를 넣는 것은 각 트랙 몫이다.
- 인스턴스 표는 CPU 사본에서 통째로 다시 올린다(새 SRV). 산포용 raw UAV는 새 서술자로 만들고, 옛 것은 GPU가 끝난 뒤 푼다. 같은 프레임의 `updateTransforms`·`setInstanceVisible`은 그대로 쌓인다. 호출은 막힘형이다(편집 시점).
- `FrameConstants::materialCount`는 이제 GPU 재질 표의 크기다.
- [실측] 단위 시험 `gpu_scene_edits_after_upload`: 추가·교체·재사용, remap, revision, 되읽은 GPU 표가 CPU 사본과 같다. 교체한 표에 프레임 산포를 한 뒤에도 같다. debug layer 오류 0.

### 6.4 변형
스킨: 선형 블렌드, 조인트 4개, 팔레트 = jointToModel × inverseBind(인스턴스마다, 현재·이전 두 벌). 바람: `windOffset(inst, p, time)`(v1 모델, P3에서 같은 시그니처로 교체), 상한 `windOffsetBound(inst, centre, radius)`(구 안 모든 점·모든 시각, 이번 프레임의 바람).
- **장면 바람 변경(v1.23, I 요청·S·R 검토)**: 호스트는 프레임 기록 전에 `GpuScene::source()` 장면의 `windDirection`·`windSpeed`를 바꿀 수 있다(태양과 같은 경로). 프레임 상수 `g_windDirection`·`g_windSpeed`가 그 프레임의 바람이다. 바람 revision은 없다. 소비자는 끝점을 비교한다.
- **무기억 계약**: `windOffset`은 시각과 그 시각의 바람만의 함수다(이력 없음). P3 모델도 이 성질을 지킨다. 지키지 않으면 아래 상한이 이력을 받는 형태로 바뀐다.
- `float windOffsetScale(GpuInstance, float3 centre, float radius)`: 속도 무관 부분이다. `windOffsetBound` = scale × `g_windSpeed`².
- `float windChangeBound(float scale, float t0, float s0, float3 d0, float t1, float s1, float3 d1)`: 같은 인스턴스 변환에서 |windOffset(t1; s1, d1) − windOffset(t0; s0, d0)|의 상한이다. 사이에 바람이 어떻게 바뀌었어도 성립한다.
  - v1 식: scale × [s1²·0.4·min(2, 1.7|t1 − t0|) + |s1² − s0²| + 2 sin(Δθ/2)·s0²].
  - 바람을 재사용하는 소비자(S 페이지, R BLAS)는 그린 순간의 (t0, s0, d0)와 scale을 저장하고 이 상한으로 판정한다.
- `windChangeFactor(t0, t1)`는 "바람이 그대로일 때"의 인자로 남는다(= 같은 바람의 `windChangeBound` / `windOffsetBound`)., 변화 인자 `windChangeFactor(t0, t1)`(v1.4: 장면 바람과 인스턴스 변환이 그대로일 때 |windOffset(t1) − windOffset(t0)| ≤ windOffsetBound × windChangeFactor; 모델이 바뀌면 세 함수를 같이 바꾼다). **래스터(V), 그림자 페이지(V 서비스), BLAS refit(R)은 모두 `deformVertex`를 쓴다.** Revision: `Instance.transformRevision`, `deformRevision`, `Material.revision`, `Light.revision`, `FrameConstants.sceneRevision` — S의 dirty 규칙과 R의 캐시 무효화가 이것을 읽는다.

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
- coverage 층(v1.41 타일 구간; v1.37 요청 `20260926_V_coverage_layer_v2.md`의 레코드, 설계 COVERAGE_REDESIGN 4.5·4.6; 배치는 `Passes/Visibility/CoverageTiles.hlsli`가 정한다). 8×8 픽셀 타일 단위이고, 타일 번호 = tx + ty·⌈W/8⌉이다.
  - **레코드 16 B** `CoverageFragment { visId; depthBits; mask; packed }`:
    - `depthBits`는 덮인 영역 무게중심의 device depth(reversed Z, float 비트)다. 부호 비트(`COV_DEPTH_SEE_THROUGH`, depth는 음수가 아니다)는 시야에 투과인 재질(유리·물)의 표시다(v1.38). 읽기는 `coverageFragmentDepth`, `coverageFragmentOpaque`로 한다.
    - `mask`는 `coverageSample(i)` 32 부표본이다.
    - `packed` = 팔면체 법선 8+8 bit | 면적 × 1023 반올림 << 16 | 타일 안 픽셀 x + 8y << 26.
    - 면적은 (가까운 평면으로 잘린) 다각형 ∩ 픽셀의 정확 면적이다. 알파 테스트 재질은 통과 부표본 비율을 곱한다.
    - 법선은 무게중심에서 원근 보정으로 보간한 월드 법선이다. 뒤에서 본 양면 재질은 뷰어 쪽으로 뒤집는다.
    - 대역 C 브릭 fragment는 visId의 삼각형 필드가 `0x7F`다.
  - **타일 구간(v1.41)**: `coverageRecords`(`StructuredBuffer<uint4>`, 원소 하나가 레코드 하나)에서 목록 타일마다 레코드가 **한 구간으로 이어져 있고, 구간 안은 픽셀 순서(pixel-major)**다.
    - 목록 타일 j(목록 순서)는 원소 [recordBase, recordBase + records)를 가진다. 그 픽셀 p(x + 8y)의 레코드는 [recordBase + start(p), recordBase + start(p + 1))이다. start(p) = `coverageTilePixels` 워드 j·64 + p, start(64) = records다.
    - 한 픽셀 안의 레코드 순서는 정해져 있지 않다. 읽는 쪽이 (깊이, visId)로 정렬한다. 하드웨어가 잘라 조각으로 래스터한 프리미티브는 같은 visId·같은 값으로 두 번 나올 수 있으니 인접 중복은 하나로 친다.
    - 읽기 도우미: `coverageTileInfo(list, j)`, `coveragePixelStart(tilePixels, info, j, p)`, `coverageBlockTile(list, b)`(블록 b를 가진 목록 타일, 이분 탐색 ≤ 32단계), `coverageLoadRecord(records, element)`.
    - **왜 바꿨나(v1.40 → v1.41)**: v1.37~1.40은 타일 청크(CAS로 풀에서 받음) + 확장 트리였다. 깊은 타일 성장 곡선 [실측, 4K, 한 타일에 fragment 10 k~300 k]에서 타일 패스는 선형이었지만, 타일당 그룹 하나가 레코드를 다 걸어 300 k에 2.05 ms였다(풀 최대면 ≈ 0.9 s, 3.6 최악 dispatch 규칙 위반). CAS에 진 헛 청크도 81 %였다.
  - **만드는 과정**(모든 패스가 그룹당 일정한 일, 그룹 수는 데이터 비례; `CoverageLayer.hlsli`):
    1. 래스터 픽셀 커널이 웨이브당 전역 원자 1회로 스트림에 레코드(16 B)와 키(타일 × 64 + 픽셀, 4 B)를 붙인다. 타일 원자·청크·CAS는 없다.
    2. 카운트(1,024 원소/그룹): 키마다, 타일마다 `WaveMatch`로 모은 웨이브의 원자 1회. 타일의 첫 레코드가 타일을 목록에 올린다.
    3. 스캔(그룹 하나 × 1,024 스레드, 스레드마다 ⌈타일 수 / 1,024⌉ 이하의 목록 타일): 목록 순서의 레코드·블록 기저(배타 접두합).
    4. 오프셋(목록 타일당 그룹 1개): 64 폭 접두합으로 픽셀 시작, 픽셀 카운터 = 픽셀 끝(절대).
    5. 흩뿌리기(1,024 원소/그룹): 키마다 웨이브의 감소 원자 1회로 레코드를 자기 픽셀 구간에 넣는다.
    6. 블록(1,024 레코드/그룹): 픽셀별 불투명 마스크 합집합·가장 먼 불투명 깊이·가장 가까운/먼 깊이. 블록 하나인 타일은 여기서 끝낸다. 여러 블록 타일은 scratch 슬롯에 원자로 모으고 마무리 패스(타일당 그룹 1개)가 끝낸다.
  - **타일 머리 8 워드**(`coverageTiles`): 0 = 레코드 수, 1 = record base, 2 = 목록 번호 + 1(0 = 이번 프레임 목록에 없음), 3·4 = opaqueCovered 64 bit, 5..7 = V 내부. 목록에 없는 타일의 워드 1·5..7은 정의되지 않는다.
    - opaqueCovered(v1.38, 설계 4.6 합집합 규칙): 그 픽셀 불투명 레코드 마스크의 합집합이 가득이고, 대역 A 표면이 그 레코드들 중 가장 먼 깊이보다 뒤인 픽셀이다(하늘 포함). 그 대역 A 표면은 합성의 마스크 합집합 가림에서 가중치가 0이다.
      - 불투명 = 유리·물이 아닌 재질이다. 알파 테스트 재질은 테스트 뒤 마스크로 기여하고, 투과 잎도 시야 가림에는 불투명이다.
      - 블록 패스가 레코드에서 만든다(픽셀별 groupshared OR·min). fragment마다 원자로 하는 판은 DesignBench 실측에서 +0.11~+0.17 ns/fragment라 기각했다.
      - 래스터는 그 뒤 fragment를 버리지 않는다. 뒤 fragment 컷은 합성이 자기 정렬에서 한다.
  - **`coverageTileList`**(raw) 머리: 0..2 = 목록 타일 위 DispatchIndirect 인자(타일당 그룹 1개, x ≤ 65535 다음 y), 3 = 목록 타일 수, 4 = 저장한 레코드 수, 5 = 블록 수(타일마다 `COV_BLOCK` = 1,024 레코드씩, 마지막은 부분), 6 = 레코드 용량, 7 = 타일 열 수, 8..10 = 블록 위 인자(블록당 그룹 1개), 11..15 = V 내부. 워드 16 + 4j = 목록 타일 j의 `{ tile, records, record base, block base }`(block base도 목록 순서 배타 접두합).
  - **`coverageDepthRange`**(R32G32_UINT, 픽셀당; S 요청 `20260926_S_fragment_visibility.md`): x = 그 픽셀 레코드 중 가장 가까운 깊이 비트(max), y = 가장 먼 것(min). 투과 레코드도 넣고 표시 비트는 지운다. 레코드가 없으면 (0, 0xFFFFFFFF)다. 블록 패스(또는 여러 블록 타일의 마무리 패스)가 쓰고, 지난 프레임 타일은 지우기 패스가 되돌린다.
  - **용량**: 직전 완료 프레임에 붙인 fragment 수(필요량) × 1.5를 64 K 레코드 단위로 쓴다(프레임의 V 패스 앞에서 읽는다). 늘릴 때는 바로 늘리고, 반 아래일 때만 줄인다.
    - 하한은 `visibility.coverage_pool_min_fragments_per_pixel` × 픽셀이다(1.0: 4K 8.3 M 레코드).
    - 상한은 구조 버퍼 뷰의 2^27 원소(134 M 레코드)다.
    - 메모리: 용량 C 레코드당 37 B(스트림 20 + 구간 16 + scratch ≈ 1; 스트림은 흩뿌리기 뒤 풀린다) + 해상도 고정분(4K: 픽셀 카운터 33.2 MB + 타일 머리 4.1 + 목록 2.1 + 픽셀 시작 33.2 + 깊이 구간 66.4 = 139 MB).
    - 필요량이 용량을 넘으면 그 프레임의 fragment가 빠지고 `Stats::overflow` 0x100(게이트 실패)이 선다. fragment를 버리는 경로는 이것뿐이다. 저장된 부분은 일관된다(목록·구간·깊이 구간이 저장 레코드와 맞는다).
    - 일관성 검사(키가 뷰 밖, 타일 수 > 용량, 픽셀 접두합 ≠ 타일 레코드 수, 흩뿌리기 대상이 용량 밖)는 결함이고 `Stats::overflow` 0x400이다.
  - **켜기**: `visibility.coverage_layer`(기본 false). M 합성이 이 층을 읽게 되면 켠다. 평면 반사 뷰는 아직 vis buffer로 그린다.
  - v1.25 형식(`coverageHeads/coverageFragments/coveragePixels`, overflow 0x200)과 v1.37~1.40 타일 청크(`coverageChunkTable`, 확장 트리, overflow 0x200)는 폐기했다. 키 `max_coverage_fragments`, `coverage_table_slots`, `coverage_heavy_tile_fragments`는 읽지 않는다. 이전 바이너리가 시작되도록 설정 파일에만 남겨 두고, 다음 통합 재빌드 뒤 지운다.
  - **전환 기간(v1.41, M 합성이 구간을 읽을 때까지)**: `ViewResources::coverageChunkTable`은 늘 무효라 v1.40 판독기가 꺼진다. `coverageChunks` = `coverageRecords`다. `CoverageTiles.hlsli`의 v1.40 이름(`coverageChunkOf` 등)은 컴파일만 되고 층을 읽지 않는다. M 전환 커밋 뒤 셋 다 지운다.

### 7.2 G-buffer (`GBuffer.hlsli`) — RG32_UINT 8 B
`.x` 월드 셰이딩 법선(팔면체 snorm16×2), `.y` baseColor sRGB8×3 | 지각 거칠기 unorm8(상위 8 bit). metallic·specular·클래스·플래그는 재질 테이블(vis id → 클러스터 → 재질). 픽셀별 metallic/occlusion 맵은 M의 셰이딩 커널이 vis id로 다시 평가한다.

### 7.3 그림자 가시성 (S) — R32_UINT
4 슬롯 × 8 bit unorm(0 = 완전 그림자, 255 = 완전 빛). 슬롯 0 = 태양, 슬롯 1~3 = 그 픽셀 프록셀 광원 리스트 순서에서 그림자를 던지는 첫 세 국소광. 넷째 이후 그림자 광원은 오버플로 목록에 같은 방법으로 계산한 가시성이 있다(v1.20, M 요청 `20260925_M_shadow_overflow.md`, S 검토 d18e9ec). 슬롯 1~3과 오버플로는 그 뷰의 프록셀 리스트(`ViewResources::froxelLights`) 순서다. v1.22부터 평면 반사 뷰도 채운다.
- `shadowOverflowTiles` R32_UINT ⌈W/8⌉ × ⌈H/8⌉(8×8 타일 = M의 셰이딩 타일). 값은 셋 중 하나다. 매 프레임 0으로 지운다.
  - 0 = 타일에 넷째 이후 그림자 광원이 없다.
  - 0xFFFFFFFF = 용량 초과(fallback).
  - 그 밖 = 1 + `shadowOverflow` 안 타일 블록의 시작 워드.
- `shadowOverflow` raw uint: 오버플로 타일마다 블록 하나다.
  - 워드 0~63: 픽셀(행 우선 8×8)마다 `count << 24 | 런 시작`(블록 기준 워드 오프셋). count는 넷째 이후 그림자 광원 수(≤ 29)이고, 0 = 없음이다.
  - 런: 8비트 unorm 가시성(이 절과 같은 부호화). uint 하나에 4개, 하위 바이트부터, 프록셀 리스트 순서다.
- 할당: 블록은 타일당 원자 가산 1회다. 같은 프레임 안에서 정확하고, 블록 위치만 비결정적이다. 용량은 S가 직전 필요량으로 관리한다.
- 넘친 타일은 `shadowOverflowFallbackTiles`(raw: 워드 0 = 개수, 워드 1..3 = DispatchIndirect 인자(개수, 1, 1), 워드 4.. = 타일 `y << 16 | x`)에 오른다. M 주 셰이딩 커널은 넘친 타일을 건너뛴다. M fallback 커널이 이 목록 위에서(바이트 오프셋 4의 인자) 슬롯 밖 광원을 `shadowVisibilityDirect`로 VSM에서 직접 탭한다. 결과는 정확하다.
- S는 overage(넘친 타일 수·픽셀 수)를 통계로 낸다. 게이트는 0을 요구한다.
- **coverage fragment 가시성(v1.41, S 요청 `20260926_S_fragment_visibility.md`)**: coverage 층 픽셀의 fragment용이다.
  - `shadowFragmentVisibility`(`StructuredBuffer<uint3>`, 픽셀당 12 B, 색인 y × width + x). `coverageDepthRange`에 레코드가 있는 픽셀만 유효하다.
    - `.x` = 선분 z_near → z_far(뷰 광선 위, 픽셀 중심)의 4점 z_k = z_near + (z_far − z_near)·k/3에서의 태양 가시성, unorm8 바이트 k. M은 fragment 깊이로 선형 보간한다(선형 뷰 깊이 기준).
    - `.y` = z_near에서 잰 국소광 슬롯 1~3(바이트 1~3, 이 절의 부호화, 255 = 슬롯 없음). 바이트 0 bit 0 = pair 플래그.
    - `.z` = 같은 값을 z_far에서 잰 것. 바이트 0은 예비다.
  - `shadowFragmentSun`(raw, `coverageRecords` 원소당 1 B): pair 플래그가 선 픽셀의 fragment만 유효하다. 바이트 e = 원소 e 레코드의 태양 가시성(unorm8). 플래그 없는 픽셀의 바이트는 그 프레임에 쓰이지 않으니 플래그 없이 읽지 않는다.

### 7.4 프록셀 광원 리스트 (S)
프록셀(24 px × 64 깊이 슬라이스) 당 광원 인덱스 목록, **가변 길이**(v1.80, RENDERER_REDESIGN_V2 14.1 L1): 개수 패스, 접두 합(FroxelScan), 채우기 패스로 같은 프레임 안에 정확히 할당되며, 프록셀에 닿는 광원은 모두 목록에 든다(잘림·합침 없음). 헤더는 프록셀당 두 워드(첫 항목, 개수), 런은 짝수 항목에서 시작한다. 목록의 앞 `atmosphere.froxels.lights_max`개(짝수, 96 이하)는 중요도 순(그림자 슬롯 1~3은 그중 첫 캐스터), 나머지는 결정적 순서. 버퍼 용량은 CPU가 같은 프레임에 계산한 장면 광원 항목 수의 상한(`FroxelSystem.cpp froxelListBound`: 광원마다 슬라이스별로 도달 검사를 통과할 수 있는 타일 수) + FX 입자 광원 몫(GPU가 범위를 계산하므로 실측 초과분에서 자라는 몫) 이다. 그래서 장면 광원은 잘릴 수 없고, FX 광원이 몫을 넘긴 프레임만(`needed` > `capacity`) 장면 광원만의 접두합(FroxelScan의 둘째 합, 상한 안)으로 런을 잡아 장면 광원은 그대로(머리+꼬리) 싣고 FX 광원만 그 프레임에 뺀다. 뺀 항목은 잘린 목록·잃은 항목 통계로 세고(게이트는 0을 요구) 다음 프레임부터 몫이 자란다. FX 입자 광원은 수(슬롯 용량)는 정해져 있지만 범위를 GPU가 밝기에서 계산하므로 구조 상한이 없다. `list_capacity_forced`는 시험용이다. 소비자는 `froxelLightRange/froxelLight`로만 읽는다. 리스트는 뷰 단위다(v1.22, `ViewResources::froxelLights`). 메인 뷰는 `FrameResources::froxelLights`와 같고, 평면 반사 뷰는 S `shadowVisibility`가 그 뷰 크기의 격자로 만든다. 호출자는 그 뷰의 SRV를 넘기고 그 뷰의 프레임 상수를 바인딩한다. 항목의 bit 15는 "그 광원에 S의 그림자 슬롯이 있음"이다(v1.19, `froxelLightShadowed`). `froxelLight`는 그 비트를 뺀 광원 인덱스를 돌려주고, 장면 광원 한도는 32767이다.

### 7.5 출력
- 메인 뷰(표시): RGB10A2_UNORM, 값 = sRGB OETF(PBR Neutral 톤맵(광도 × 노출)).
- 검증 실행(`FrameContext::outputLinearHdr`): RGBA32_FLOAT, 광도(nit) × 노출, 톤맵 전. 기준 영상과 같은 양(10.2).

## 8. 재질·광원·하늘 모델 (기준 경로추적기와 실시간이 같게 구현)

### 8.1 재질 v1 (`Native/Scene/include/unx/scene/MaterialModel.h` 권위, `MaterialModel.hlsli` 미러)
- α = max(r², 1e-4), f0 = lerp(0.08·specular, baseColor, metallic).
- D = GGX, V = Smith 높이 상관, F = Schlick. D는 소거 없는 형태 `α² / (π (|n×h|² + α² (n·h)²)²)`로 평가한다(v1.4: `(n·h)²(α²−1)+1`은 α = 1e-4에서 float로 0이 되어 거울 반사 방향에서 ∞). 다중 산란 보정 f_s = D V F · (1 + f0 (1/E(μ, r) − 1)) (Turquin 2019). E는 32×32 격자(끝점 포함, μ=0은 1e-4에서 평가) 가시 법선 표본 4096개로 만든 방향 알베도 표, 쌍선형.
- **스펙큘러 방향 알베도 분할 (A, B)** (v1.25, R 요청 `20260925_R_hit_shading.md` 1번): `scene::model::specularAlbedoTable()`(32×32 float2, E와 같은 격자·같은 가시 법선 표본을 Schlick F로 나눔, A + B = E), `specularAlbedo(μ, r)`(쌍선형). f0·A + B가 Schlick lobe의 알베도다. GPU: `FrameConstants::specularAlbedoLut`(`g_specularAlbedoLut`, StructuredBuffer<float2>, `GpuScene`이 올림)와 `MaterialModel.hlsli`의 `modelSpecularAlbedo(NoV, r)`. M·R은 각자의 복사본(`ShadingSystem`의 표, R `SpecularAlbedo.cpp`)을 지우고 이것을 쓴다.
- f_d = (1 − metallic) baseColor / π (Lambert). f = f_d + f_s (n·l > 0, n·v > 0).
- Foliage: 앞면 f_d × (1 − transmission), 뒷면으로 가는 빛 transmission × (1 − metallic) baseColor / π.
- two-sided: 뒷면에서 법선을 뒤집는다. alpha test: baseColor 텍스처 alpha ≥ alphaCutoff면 불투명(광선 any-hit도 같다).
- 노멀맵: `n_ts = (2r−1, 2g−1, √(1−x²−y²))`, TBN = (정점 보간 탄젠트, sign·cross(n, t), 정점 보간 법선), 결과 정규화. 탄젠트는 장면 데이터에 있는 값만 쓴다(재계산 금지).
- 셰이딩 법선 규칙(v1.65, 기준 `RtScene.cpp`와 같다): 최종 셰이딩 법선이 기하 법선 반대쪽이면 삼각형 평면에 대해 반사한다. 셰이딩하는 면을 보는 경우(앞면, 또는 two-sided의 뒷면)에 n·v < 1e-4이면 n ← normalize(n + v (1e-4 − n·v))로 굽힌다. BRDF는 n·v > 0에서만 정의되므로, 이 규칙이 없으면 비스듬히 보는 노멀맵 텍셀이 빛을 전혀 반사하지 않는다(U2의 검은 점). 실시간: M의 해석과 coverage 조각(`mNormalOnGeometricSide`, `mNormalTowardsViewer`), 그리고 ShadeOpaque가 G버퍼 양자화 뒤에 다시 굽힌다.
- 텍스처: 기준은 mip 0 쌍선형(표본 수로 픽셀 필터를 적분), 실시간은 footprint 밉·이방성 + 노멀→거칠기 필터(설계서 2.2). 이 차이는 품질 정의(설계서 3절 "재질")의 허용 항목이다.
- Hair, Water, Glass, Subsurface 클래스 모델은 해당 단계(P3/P4) 전에 0절 절차로 이 절에 추가한다.
- Hair 클래스(v1.66, B10): 셰이딩은 E의 `Passes/Hair/HairBsdf.hlsli`(d'Eon·Chiang 섬유 산란, 모든 내부 차수)다. 매개변수는 β_M = roughness, η = ior(케라틴 1.55), `hairBetaN`, `hairTilt`, 흡수 σ_a = `scene::model::hairAbsorption`이다. 멜라닌(`hairEumelanin`, `hairPheomelanin`)이 있으면 d'Eon 2011, 없으면 baseColor를 목표 색으로 보고 Chiang 2016 역변환을 쓴다. 단위는 PBRT 규약(섬유 반지름당)이다. 간접광 = 캐시 조도 × 섬유 방향 알베도이며, 정확 조건은 섬유 둘레 방향 입사 복사가 평탄하고 캐시 텍셀 각 폭이 섬유 로브보다 좁은 것이다.
- Cut 클래스(v1.66 필드, v1.71 정의, A11): 셰이딩은 Standard 그대로이고 텍스처 값만 uv0 대신 객체 공간 투영 셋에서 온다(`Passes/Material/CutFace.hlsli`, CPU 기준 `Reference/PathTracer/src/CutFace.h`).
  - 투영 k(k = x, y, z): uv_k = cutScale·(s_k p_b, p_c), (b, c) = ((k+1)%3, (k+2)%3), s_k = 객체 공간 기하 법선의 k 성분 부호. 가중치 w_k = |n_k|⁴ / Σ. 틀은 접선 s_k e_b, 종접선 e_c, 법선 s_k e_k다.
  - 기본색·거칠기 계수·금속 계수 = Σ w_k · tap_k(mip 0 기준).
  - 셰이딩 법선 = normalize(Σ w_k r_k). r_k = `cutFaceWhiteout`(탭 k의 접선 공간 법선, 투영 k, 객체 공간 보간 법선 n) = (tn.x + n·T) T + (tn.y + n·B) B + tn.z (n·N) N. 평평한 텍셀이면 정확히 n이다. M의 필터 판은 각 틀에서 기울기 모멘트를 w_k로 섞는다.
  - 가장자리 손상(외관 모델): 삼각형의 경계 변(클러스터 삼각형 워드 비트 24..26, 비트 i = 꼭짓점 i 맞은편 변)까지 객체 거리 d. 문턱 t = cutDamageWidth·(0.35 + 0.65·잡음)(값 잡음 3옥타브, 셀 = 폭, 해시는 두 구현이 같음). 덮임 = saturate(0.5 + (t − d)/발자국)이고 기준은 발자국 0이라 계단 함수다. 적용: 기본색 × (1 − 0.45 d), 거칠기 → 1 쪽으로 0.6 d.
  - 경계 변: LOD 0에서 Cut 서브메시가 한 번만 쓰는 변(이웃이 다른 재질이거나 없음, 정확 위치 용접)이다. 단순화 클러스터에서는 두 끝이 경계 꼭짓점이고 중점이 LOD 0 경계 위(클러스터 자기 오차 안)인 변도 포함한다. 열린 테두리는 단순화에서 잠기지 않아 테두리 위 새 변이 생기기 때문이다.

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

### 8.5 Unity 콘텐츠 → 렌더러 장면 매핑 (v1.44, D0; core 결정 — M 정지 중, 조율 지시)
호스트(I)의 레벨 적재기가 Unity 에셋을 `scene::Scene`(UnxScene*)으로 바꿀 때의 규칙이다. 목표는 두 가지다. 같은 입력이면 늘 같은 장면이 나와야 하고, 8.1~8.4 모델로 정확히 옮길 수 없는 입력은 조용히 바꾸지 않고 **콘텐츠 보고**(적재 로그 + 적재기 결과 목록)에 남긴다.
- **좌표**: Unity(왼손, y 위)에서 렌더러 공간으로 z를 반전한다(S = diag(1, 1, −1)).
  - 위치·법선: z → −z. 탄젠트: xyz의 z → −z, **w → −w**(반사가 틀의 손방향을 뒤집는다).
  - 인스턴스 행렬: M_r = S · M_u · S. 삼각형 순서는 그대로 둔다(Unity의 시계 방향 앞면이 반사 뒤 반시계 방향 앞면이 된다).
  - 카메라도 같은 S로 바꾼다.
  - **World 데이터는 호스트 경계 한 곳에서만 S를 적용한다(2026-09-27, 계약).** 물리·VFX·표면 상태처럼 World(Unity 축)로 사는 GPU 데이터(스트림 앵커·입자·유체 입자·표면 벽돌 키)는 제 공간에 그대로 두고 시뮬레이션한다. 렌더러 출력 단계에서 `FrameContext::streamAxes`(UnxRendererCreate가 (1, 1, −1)로 둔다)와 `worldOrigin`(원점 이동 누적)으로 바꾼다. 위치는 streamAxes ⊙ p, 방향·법선은 성분별 곱, 틀·회전은 스트림 공간에서 만들고 출력만 바꾼다(M R M). C#에서 넘기는 장면 값(인스턴스, 뼈, 카메라, 해, 구름 바람, 수조 중심·요)은 `UnravelNextRenderer.Space`로 바꾼다. 새 경로는 둘 중 하나만 쓴다. 적용 확인(2026-09-27): FX(ca4725e), Volume(482759c), 유체 표면(4afd044)은 streamAxes를 쓴다. 표면 상태(A7)는 엔진 2가 고친다. 런타임 인스턴스(C2)와 바다는 아직 C# 호출자가 없다.
- **UV와 텍스처 행**: 텍스처는 행을 위→아래(`UnxTextureDesc.texels` 계약)로 넘긴다. Unity `GetPixels`는 아래→위라서 행을 뒤집고, 대신 **v' = 1 − v**로 둔다.
  - 탄젠트와 노멀맵 초록 채널은 둘 다 Unity의 +v를 가리킨 채로 둔다. 둘이 같은 방향을 가리키므로 TBN은 맞다.
  - 재질의 타일링·오프셋(`_BaseMap_ST`/`_MainTex_ST`)은 uv0에 굽는다: uv = uv·scale + offset, 그다음 v 반전. 한 정점을 ST가 다른 두 재질이 쓰면 정점을 복제한다.
- **재질(URP Lit, Built-in Standard; 색은 `Color.linear`, 텍스처 색 공간은 가져오기 설정 그대로)**:
  - `baseColor` = `_BaseColor`/`_Color`.rgb(선형). 기본 맵 → Rgba8Srgb. 기본 색 alpha는 텍스처 alpha에 곱해 굽는다.
  - 금속 흐름(기본): `_MetallicGlossMap`이 없으면 metallic = `_Metallic`, roughness = 1 − `_Smoothness`(또는 `_Glossiness`). Unity의 smoothness는 지각 값이고 GGX α = (1 − s)²라서 8.1(α = r²)과 같다.
    - 맵이 있으면 metallic = roughness = 1(상수)로 두고, Rg8RoughMetal 텍스처를 재질마다 굽는다: R = 1 − a·`_Smoothness`(`_GlossMapScale`), G = 맵.r. a는 `_SmoothnessTextureChannel`에 따라 맵 alpha 또는 기본 맵 alpha다.
  - 스펙큘러 흐름(`_WorkflowMode` = 0, `_SpecColor`): f0이 무채색이고 0.08 이하면 metallic 0, specular = f0/0.08로 정확하다. 확산색이 검정이면 metallic 1, baseColor = f0로 정확하다. 그 밖은 8.1로 정확히 옮길 수 없으므로 콘텐츠 보고를 남기고 첫 규칙으로 바꾼다.
  - 노멀(`_BumpMap`): 가져온 원본(OpenGL +Y)을 풀어 x, y = 2c − 1로 하고, `_BumpScale`을 xy에 곱한 뒤 x² + y² ≤ 1로 자른다. 결과는 Rg8Normal(0.5 + 0.5·xy)이다. Unity 압축형(DXT5nm AG, BC5 RG)을 읽을 때는 풀어서 같은 값으로 만든다. 노멀맵이 있는 메시는 탄젠트를 반드시 넘긴다(Unity 메시 탄젠트, 위 변환).
  - 가림(`_OcclusionMap`): R8Linear = lerp(1, 맵.g, `_OcclusionStrength`).
  - 방출: nit = S · `_EmissionColor`(선형, HDR 세기 포함) × 방출 맵이다. S = 1.2 · 2^EV100_ref이고 EV100_ref는 레벨 설정(기본 14 = `ViewDesc::ev100` 기본값)이다. 이 척도에서는 Unity가 흰색(1.0)으로 보이는 방출이 렌더러에서도 EV100_ref 노출에서 흰색으로 보인다.
  - 알파 컷: URP `_AlphaClip` / Standard Cutout이면 alphaCutoff = `_Cutoff`다. 반투명(URP `_Surface` = Transparent, Standard Fade/Transparent)은 v1에 알파 혼합 경로가 없으므로 콘텐츠 보고를 남기고 불투명으로 그린다(유리·물은 Glass/Water 클래스가 들어올 때 따로 매핑).
  - 양면: `_Cull` = Off(0)면 twoSided다. 디테일 맵, 시차(높이) 맵, 투명 코트는 v1에 없으므로 콘텐츠 보고를 남긴다.
  - 재질 클래스: Standard다. 셰이더가 투과 값(`_Translucency`, `_TransmissionScale` 등)을 가지면 Foliage이고 transmission = 그 값이다. 값 없이 이름만으로 추정하지 않는다. 그 밖의 사용자 셰이더는 콘텐츠 보고를 남기고 `_BaseColor`·`_BaseMap`만 읽는다.
- **광원(URP 물리 감쇠 기준)**: 같은 S로 바꾼다. Unity Lambert(알베도 × 세기 × N·L / d²)와 렌더러(알베도/π × 조도)가 EV100_ref 노출에서 같은 화소값이 된다.
  - 점·스폿: 광도 I(cd) = π · S · intensity. range는 그대로다(8.2의 창 함수가 URP와 같다).
  - 스폿: spotScale = 1 / max(cos θ_in − cos θ_out, 1e-3), spotOffset = −cos θ_out · spotScale. θ는 반각이다(Unity의 spotAngle·innerSpotAngle은 전각이다).
  - 방향광: 태양의 방향만 쓴다. 세기와 색은 물리 하늘(환경 기본값, 8.3)이 정한다. 사용자가 태양 조도를 따로 저작하면 UnxFrameSetSun으로 준다.
  - 면광원(Area, 굽는 전용): v1 면광원으로 옮긴다. 휘도 L = S · intensity / π다.
- **메시**: 인덱스는 32비트, 서브메시 = Unity 서브메시(재질 슬롯)다. 법선이 없으면 가져오기 설정대로 계산한 법선을 쓴다. 퇴화 삼각형(넓이 0)은 적재기가 지운다. `scene::validate`가 거부하는 입력은 적재 실패로 보고한다.
- **정적·동적**: Unity Static 플래그와 상관없이 모든 인스턴스는 같은 경로로 들어간다. 편집 모드의 이동은 `UnxFrameSetTransforms`, 추가·교체는 `GpuScene::setInstance`, 제거는 숨김 + 슬롯 재사용이다(6.3).

## 9. 품질 설정 키 (`Config/quality/<이름>.toml`)
- 파일 `<이름>.toml`은 `<이름>.`으로 시작하는 키만 가질 수 있다(`QualityConfig::loadDirectory`가 강제). 파일 소유는 1절 표.
- 코드는 품질 키에 기본값을 두지 않는다(없으면 오류). 모든 보고는 병합된 키 집합의 SHA-256을 남긴다. 실험용 덮어쓰기(`applyOverride`)도 해시에 들어간다.
- 다른 트랙이 읽는 키(이 목록 밖의 키를 다른 트랙이 읽으려면 0절 요청): `output.resolutions`(코어), `atmosphere.froxels.tile_px`, `atmosphere.froxels.depth_slices`, `atmosphere.froxels.lights_max`(S → M), `gi.screen_probe_spacing_px`(R → M), `shadow.vsm.page_texels`(S → V 래스터 LOD), `visibility.cluster_triangles`(V → R).

## 10. C 트랙 API

### 10.1 테스트 장면 생성기 (`Tools/SceneGen/include/unx/scenegen/SceneGen.h`)
`scene::Scene generate(const Request&)`, `allScenes()`, `sceneName(id)`. 장면: CityBlock, ForestThin(잎 6 cm·풀잎 4 mm), ForestCard(잎 35 cm·풀 카드 30 cm), Waterside(잔잔한 물·파도), Interior(거울·광택 바닥·면광원), CityNight(광원 512·그림자 128), RidgeSunset(v1.17: 능선·탑 그림자의 공기 산란, 낮은 태양 17°, 20 km, 바람 없음; 번호 6, 기존 번호 불변). ForestCombat(v1.38, C bb73ea1: RPP-1 숲·전투 게이트 장면, forest_thin 바탕 + 전투 지점 둘레의 닫힌 수관 숲, 카메라 eye·up·edge·vista(8ba4247); 번호 7; 메타 `Results/C/Scenes/forest_combat_meta*.md`). (v1.40, C 195fda3) 게이트 장면 다섯 개(city_block, city_night, forest_combat, waterside, interior)에 RPP-1 동적 강체 1,024개가 들어갔다(`InstanceDynamic`, 장면 해시 바뀜). 공개 함수 `dynamicContent`·`dynamicContentJson`·`generateWithContent`와 구조체 `DynamicBody`·`CharacterSlot`·`DynamicContent`가 추가됐다. V 게이트는 `--camera NAME`으로 카메라를 고른다. 결정적(같은 요청 = 같은 `contentHash`), `validate` 통과, 카메라 하나 이상과 정지·이동 카메라 경로 포함. `scale`은 게이트 부하 배율(1 = 그 장면의 게이트 정의, 예: P1 장면 ≥ 1천만 삼각형).

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
  - (v1.39) `gpu_contention`(3.3 경합 검출; 샘플러가 없으면 null)과 `queues`를 더했다. `queues`는 큐별(graphics, compute)로 프레임당 명령 목록 수와 패스 바깥 시간 `head_ms`·`tail_ms`·`gap_ms`의 분포다.
  - 프로파일러(`GpuProfiler`, I 요청 `20260926_I_profiler_gaps.md`): 명령 목록마다 시작 마크와 끝 마크(목록의 마지막 배리어 뒤)를 찍는다(`listBegin`/`listEnd`가 `frameMark`를 대신한다). 패스는 목록 시작 마크부터 이어서 잰다. `FrameTiming::queues[]`(`QueueTiming`)는 lists, headMs(프레임 첫 타임스탬프 → 이 큐의 첫 목록 시작), tailMs(목록마다 마지막 패스 끝 → 목록 끝의 합: 목록을 닫는 배리어, 마지막 목록에서는 프레임 끝 레이아웃 전환), gapMs(목록 끝 → 다음 목록 시작의 합)다. 한 큐에서는 프레임 = head + 패스 합 + tail + gap이다. 목록이 하나면 타임스탬프 수는 v1.38과 같다.
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
- v1.9 (2026-09-25):
  - **I 요청 `20260925_I_unity_queue_device.md` 반영**: `DeviceOptions::externalDevice`, `DeviceOptions::externalGraphicsQueue`, `Queue::setExecuteHook`(4.1). 둘 다 null이면 기존 동작이다. 트랙 코드는 바뀌지 않는다.
- v1.10 (2026-09-25):
  - **M 요청 `20260925_M_material_textures.md` 반영(코어 부분)**: `gpu::MaterialTextures`, `MaterialTextureBit`, `gpu::Material::textureClamp`(이전 `pad0`, HLSL `GpuMaterial.textureClamp`), `GpuScene::setMaterialTextures`, `GpuScene::materials()`, `GpuScene::buffer("materials")`, 정적 샘플러 s5 `g_anisoClamp`(aniso 16, clamp). 단위 테스트 `gpu_scene_material_textures`. 진입점 `tracks::prepareScene`은 M이 자기 모듈에 정의를 커밋한 뒤 `Tracks.h` 선언과 `FrameRenderer` 호출을 넣는다.
- v1.11 (2026-09-25):
  - **M 요청 `20260925_M_material_textures.md` 마무리**: 진입점 `tracks::prepareScene(fc)`(`Tracks.h`, M 구현 89e0817, 꺼진 M은 코어 빈 구현), `FrameRenderer::record`가 어떤 프레임 상수보다 먼저 부른다(5.2 순서 표 첫 줄). 5.6 표에 `Passes/Material/MaterialTextures.hlsli`. V의 알파 테스트가 `materialBaseColorGrad`(텍스처 주소 방식·발자국 필터·coverage 보존 알파 밉)로 읽는다. 알파 테스트 재질이 이제 실제로 잘린다.
  - **렌더 그래프 앨리어싱(S 신고)**: 과도 자원 배치를 세 풀로 나눴다. 깊이-스텐실 텍스처, 렌더 타깃 텍스처, 나머지가 각자 풀 안에서만 메모리를 공유한다. 깊이 버퍼가 쓰던 메모리에 놓인 3D UAV 텍스처(S 공기 볼륨)가 쓰기를 전부 잃었다 [실측]. 배리어는 API 규칙대로였다. 선행 점유자 비활성화 배리어(쓰기 플러시)를 추가했다. 진단 스위치 `UNX_GRAPH_DUMP=1`(플랜·배리어·배치·수명 로그)과 `UNX_GRAPH_NO_ALIAS=1`을 넣었다. 서술자 이중 해제를 검출한다. `RenderGraph::sharesMemory`(테스트용). 단위 테스트 `graph_depth_memory_not_shared`, `graph_aliased_buffers_keep_their_writes`.
- v1.12 (2026-09-25):
  - **통합 게이트는 커밋 기준 빌드(3.5, 사용자 지시)**: `Build.ps1 -Committed [-Ref]` → `..\UnravelNext-gate` worktree. 트랙 집합이 바뀐 빌드 폴더는 새로 구성한다(I 신고, Ninja dyndep 단언).
  - 렌더 그래프: 과도 버퍼의 첫 사용은 항상 활성화 배리어를 받는다. 앞 프레임에 앨리어스 선행자로 비활성화(NO_ACCESS)된 버퍼가 접근 불가로 남던 결함을 고쳤다(S 신고, 디버그 레이어 1332).
- v1.13 (2026-09-25):
  - **R 요청 `20260925_R_probe_lookup_structure.md` 반영(코어 부분)**: `ViewResources::screenProbeMaps`(5.1 표). `TextureDesc::srvFormat/uavFormat`는 캐스팅 가능한 뷰 형식이다(4절; `DeviceCaps::relaxedFormatCasting`, 지원 안 하면 실패). 선택지 (a)가 이 장치에서 된다 [실측]: 단위 테스트 `graph_castable_view_formats`에서 R32_UINT UAV로 쓰고 R9G9B9E5_SHAREDEXP SRV로 읽었고 4,096 texel 중 틀린 것 0. RGBA16F 아틀라스(8 B/texel)의 절반 대역이다. `screenProbeGather`는 R의 헤더(5.6)다.
- v1.14 (2026-09-25):
  - **I 요청 `20260925_I_skin_normals.md` 반영**: `Deformation.hlsli`의 `skin()`이 법선을 관절 3×3의 여인수 × sign(det)으로 변환한다(`cofactorNormal`: 역전치 방향, 비균일 스케일·전단·거울 관절에서도 정확). 위치·탄젠트는 관절 행렬 그대로다. 회전 + 균일 스케일에서는 결과가 같다. V 래스터, S 페이지(V 서비스), R refit이 한 번에 바뀐다. C의 기준 경로추적기가 스킨을 지원할 때도 같은 식을 쓴다. 단위 테스트 `skin_normals_use_the_cofactor`.
- v1.15 (2026-09-25):
  - **S 요청 `20260925_S_air_volume.md` 반영(기록)**: 공기 볼륨 한 장(S `froxels()`, 프록셀 격자 위 Texture3D RGBA16F `gridX × gridY × 3(S+1)`; 부분 0 in-scattering × 노출, 1 광학 깊이, 2 노드의 태양 투과율). `FrameResources::aerialPerspective`와 `::froxels`가 이 볼륨이다. `atmosphere()`의 64×64×32 볼륨은 없어졌다. `atmosphereAerial`은 완성된 공기 합성을 돌려준다. `atmosphereAirView`는 새 함수다. `froxelScattering`은 삭제했다(5.6). 정확도 조건은 요청 파일 "정확도 조건" 절이다(방향 보간 Mie 0.1 %, 깊이 선형·가중 1/512, 대기 적분 3e-4, fp16 상대 1e-3 [예상]). 코드는 S f1f6f8a, M d3d6431이 사용한다.
- v1.16 (2026-09-25):
  - **`Passes/Visibility/Coverage.hlsli` 커밋과 시그니처 기록(5.5.1)**: M의 커밋된 `EdgeComposite.hlsl`/`EdgeAreaProbe.hlsl`이 include하는데 커밋에 없어 `-Committed` 빌드가 깨졌다(R 신고).
  - `Build.ps1`: git 호출을 종료 코드로 판정한다. PowerShell 안에서 `& Build.ps1`로 부를 때 git의 정보성 stderr가 오류로 끝나지 않는다(R 신고).
- v1.17 (2026-09-25):
  - **요구 하드웨어와 표준 경로(2.6, 사용자 지시)**: `Device`가 검사하는 요구 기능(FL 12_2, SM 6.6, 메시 셰이더, DXR 1.1, enhanced barriers, binding tier 3, heap tier 2, 캐스팅 형식을 쓰면 relaxed format casting), Agility SDK 618, 벤더 전용 경로는 선택이며 표준 경로가 항상 있다, 선을 올리는 변경은 기록한다, 약한 GPU 정책은 나중에 정한다(몰래 품질을 낮추지 않는다).
- v1.18 (2026-09-25):
  - **S 요청 `20260925_S_sun_visibility_at.md` 반영(R 요청 `20260925_R_sun_visibility_at_hits.md`의 답)**: 5.6의 `ShadowSrvs` = `{ pageTable, pool, blocks, searchBound, constants, lights, pad0, pad1 }`와 `shadowSunVisibilityAt`(S 구현 c1d8462). `FrameResources`: `vsmPool`을 `BufferRef`(raw)로 바꿨고(`TextureRef`는 쓰는 곳이 없었다), `vsmBlocks`, `vsmSearchBound`, `vsmConstants`(CBV 인덱스)를 추가했다. 생산은 S `shadowPages`, 소비는 R(`SrvGraphics`/`SrvCompute`). 비용 [예상]: hit당 약 80 ns(가시성 패스 픽셀당 일과 같음). 반사 hit 그림자 광선 약 1.3 ms [실측]을 대체한다(주 뷰가 요청한 페이지만 상주, 나머지는 광선).
- v1.19 (2026-09-25):
  - **S 요청 `20260925_S_local_shadow_lookups.md` 반영**: `FrameResources::vsmLocalLights`(StructuredBuffer<VsmLocalLight>, 48 B × 그림자 슬롯 128)와 `vsmSlotOfLight`(StructuredBuffer<uint>, 장면 광원마다 그림자 슬롯 또는 0xFFFF)를 추가했다. 둘 다 이번 프레임의 SRV 디스크립터 인덱스다(S 업로드 링, 그래프 자원 아님). 생산은 S `shadowPages`. 5.6의 `shadowVisibilityDirect`는 `ShadowSrvs.lights` = `vsmLocalLights`, `.pad0` = `vsmSlotOfLight`로 읽는다(S 구현 1faa52e). 시그니처는 그대로다. footprint 인자(픽셀 발자국에 맞는 mip)는 소비자가 필요하다고 할 때 추가한다.
  - 프록셀 리스트 항목의 bit 15 = S 그림자 슬롯 유무(`froxelLightShadowed`), `froxelLight`는 마스크된 인덱스, 광원 한도 32767(7.4, 5.6 `Froxel.hlsli` 행). 반사 뷰의 슬롯 1~3은 아직 255다(리스트가 메인 뷰 것, S의 다음 항목).
- v1.20 (2026-09-25):
  - **M 요청 `20260925_M_shadow_overflow.md` 반영(S 검토 d18e9ec)**: 슬롯 밖(넷째 이후) 그림자 광원의 오버플로 목록. `ViewResources::shadowOverflowTiles`(R32_UINT ⌈W/8⌉×⌈H/8⌉), `shadowOverflow`(raw), `shadowOverflowFallbackTiles`(raw, 인자 워드 1..3)를 추가했다. 생산 S(메인 뷰 가시성 패스: 세기 → 타일당 원자 할당 → 평가), 소비 M(광원 루프의 순번 > 3 로드, 넘친 타일 건너뛰기, fallback 커널). 7.3 문구와 5.1 표를 바꿨다. 셰이딩 커널 안 VSM 탭 금지를 지키면서 리스트의 그림자 광원 전부(≤ 32)가 같은 가시성 계산을 받는다.
- v1.21 (2026-09-25):
  - **S 요청 `20260925_S_pixel_local_visibility.md` 반영(기록)**: 5.6 `ShadowVisibility.hlsli`에 픽셀 기준 국소광 가시성 `ShadowPixelReceiver`, `shadowPixelReceiver`, `shadowLocalVisibilityAtReceiver`, `shadowLocalVisibilityAtPixel`(S 구현 9e73555). v1.20 오버플로의 넘친 타일을 M fallback 커널이 평가할 때 가시성 슬롯·오버플로와 같은 receiver·footprint·양자화를 쓰므로 값이 비트 단위로 같다(7.3의 "결과는 정확하다"의 조건). 새 자원·필드는 없다. `shadowVisibilityDirect`는 뷰 픽셀이 없는 호출자용으로 남는다.
- v1.22 (2026-09-25):
  - **S 요청 `20260925_S_planar_view_products.md` 반영**: 평면 반사 뷰에 S의 뷰 단위 산출물이 없어 반사 속 국소광·거울 뒤 공기·국소광 그림자가 빠지던 품질 누락을 막는다. `ViewResources::froxelLights`(7.4 형식)와 `airVolume`(v1.15 형식)을 뷰 단위 필드로 추가했다. 메인 뷰는 코어(`FrameRenderer`)가 S `froxels` 직후 `FrameResources::froxelLights/aerialPerspective`와 같은 참조를 넣는다. 평면 뷰는 S `shadowVisibility`가 채운다.
    - 평면 뷰의 공기 볼륨은 거울면 교차 t_p에서 적분을 시작한다(앞 구간 기여 0, 투과 1). 메인 뷰 셰이딩이 반사 복사휘도에 거울 픽셀의 공기를 곱하므로 두 구간의 곱이 실제 경로의 공기와 같다(타일 중심 광선 근사, 메인 뷰와 같은 수준).
    - 7.3: 슬롯 1~3과 오버플로는 그 뷰 리스트 기준이다. "메인 뷰 전용"을 지웠다. 조회 함수의 형식은 그대로다.
    - 비용 [예상, S]: 평면 뷰 픽셀당 약 0.08 ns(R의 평면 뷰 사전값 0.26 ns/px에 더해짐, 36bf02b).
  - **R 요청 `20260925_R_planar_view_mask.md`의 계약**: `ViewDesc::planarMask`(R8_UINT 뷰 크기, 0 아님 = 거울 픽셀, 무효 = 모든 픽셀)와 선택 `planarTileMask`(R8_UINT ⌈W/8⌉×⌈H/8⌉, 0 아님 = 거울 픽셀이 있는 타일), 생산 R.
    - V: 거울 픽셀이 없는 8×8 타일 위의 클러스터를 컬링하고(래스터 서비스와 같은 타일 마스크 컬링), 래스터 전에 거울 픽셀이 아닌 픽셀의 깊이를 가장 가까운 값(1)으로 채운다. 그 픽셀은 조기 깊이 판정으로 모든 fragment를 버리고 `VIS_NONE`으로 남는다.
    - M·S: 타일 분류에서 거울 픽셀이 없는 타일을 건너뛴다.
    - V 구현은 GPU 검증(게임 뒤) 뒤에 들어간다. 그 전까지 V는 마스크를 무시하고 모든 픽셀을 그린다(결과는 같고 비용만 크다).
- v1.23 (2026-09-25):
  - **I 요청 `20260925_I_wind_change.md` 반영(S 검토 `20260925_S_wind_change_review.md`, R 의견)**: 커밋 뒤 장면 바람 변경의 계약(6.4).
    - 호스트는 프레임 기록 전에 source 장면의 바람을 바꾼다(태양과 같은 경로). 바람 revision은 두지 않는다. S와 R 모두 끝점 비교로 충분하다고 했다.
    - `windOffset`의 무기억 계약을 넣었다(P3 모델도 지킨다).
    - `Deformation.hlsli`에 `windOffsetScale`(속도 무관, `windOffsetBound` = scale × s²)과 `windChangeBound`(끝점 상한, S의 더 좁은 식)를 추가했다. `windOffsetBound`와 `windChangeFactor`는 그대로다.
    - S는 페이지 메타에 scale과 그린 순간의 바람을 저장하고, `windChanged` 전체 무효화를 없앤다. R의 바람 BLAS 재사용은 같은 방식이다. I의 `UnxFrameSetEnvironment`가 바람도 받을 수 있다.
- v1.24 (2026-09-25):
  - **FX 트랙 등록(조율 세션 요청, World·VFX 설계 7절 V1 입자 GPU 모듈)**:
    - 1절 소유표에 FX 행: `Native/Render/Passes/FX/`, `Config/quality/fx.toml`.
    - `cmake/Tracks.cmake`: `UNX_ALL_TRACKS`에 FX, `UNX_TRACK_OF_FX`.
    - `Build.ps1 -Track FX`: `build/FX`, 트랙 "FX"(코어 + FX). V·M과 함께 돌릴 때는 `-Tracks "V;M;FX"`.
    - 진입점 `tracks::simulation(fc)`(`Tracks.h`): C0 슬롯(설계 4.1)이다. `FrameRenderer::record`가 `prepareScene`과 메인 뷰 프레임 상수 다음, `atmosphere` 앞에서 부른다. 코어가 만든 빈 구현(`Passes/FX/FxTrack.cpp`)과 꺼진 빌드용 스텁(`Frame/Stubs/TrackFX.cpp`)을 넣었다.
    - `GpuScene::setParticleBuffers` 계약은 FX 요청 파일로 받는다.
- v1.25 (2026-09-25):
  - **스펙큘러 (A, B) 표(8.1, R 요청 `20260925_R_hit_shading.md` 1번)**: `scene::model::specularAlbedoTable/specularAlbedo`, `FrameConstants::specularAlbedoLut`(이전 예비 칸, 크기 528 B 그대로), `MaterialModel.hlsli`의 `modelSpecularAlbedo`(M 파일에 E 조회 옆으로 넣었다. M 확인 요청). [실측] CPU A + B − E 최대 6e-8, GPU 읽기 대 C++ 1.2e-7(`material_specular_albedo_split`, `material_tables_on_the_gpu`).
  - **대역 B coverage 층(V, 7.1)**: `ViewResources::coveragePixels`, 정렬·면적·깊이·마스크의 정의, 켜기 설정 `visibility.coverage_layer`(기본 false, M 합성 전까지)와 `visibility.max_coverage_fragments`.
    - 구현: 보존 래스터 + 정확 면적. 메시 셰이더가 가까운 평면 클리핑과 한쪽 면 뒷면 컬링을 한다. 픽셀별 연결 목록에 쌓고, 빌드에서 할당·정렬·중복 제거를 한다.
    - 하드웨어가 잘라 조각으로 래스터한 프리미티브는 보존 래스터가 절단선 픽셀을 두 번 셰이딩한다. 그 fragment들은 같은 값이라 하나만 남긴다.
    - [실측] `coverage_layer_is_exact`: 3프레임 약 13.8k fragment 전부가 정확 클리핑과 일치했다(면적 최대 오차 5.7e-5 px², 깊이 3.9e-6 상대, 마스크는 가장자리 2e-4 px 밖에서 일치). 누락 0, 남은 heads 0이었다. 뒷면 컬링과 대역 A 가림도 맞았다.
  - **평면 마스크 V 구현(v1.22 계약)**: 거울 픽셀이 없는 8×8 타일의 클러스터 컬링과 깊이 채움을 넣었다. 보조 뷰 통계는 `visibility::latestStats(state, "secondary")`(그 프레임의 마지막 보조 뷰)로 읽는다. [실측] `planar_mask_draws_only_mirror_pixels`: 거울 픽셀은 마스크 없는 뷰와 같다(95,982 픽셀 중 0 차이). 나머지는 VIS_NONE·깊이 1이다. 화면 1/3에만 거울이 있을 때 보이는 클러스터가 654에서 206으로, 삼각형이 38.9k에서 12.7k로 줄었다.
- v1.26 (2026-09-25):
  - **설계 개정 1(`COVERAGE_REDESIGN_KO.md`, 요청 `20260925_D_coverage_redesign.md`) 기록.** 인터페이스 시그니처가 확정된 것부터 넣고, 구현이 들어올 때 각 절을 갱신한다.
    - 요청 4절(5.3 coverage 모드): 시그니처를 확정했다. `DepthRasterRequest::coverage/bands`, `DepthRasterCoverage depthRasterCoverage(DepthRasterPixel)`. V 구현은 새 coverage 층 다음이고, 그때까지는 fail한다.
    - 요청 3절(5.6 S): `shadowSunTransmittanceAt`, `ShadowSrvs.layers`, `FrameResources::vsmLayers`(S 요청 `20260925_S_transmittance_layer_lookup.md`). 층이 채워지기 전에는 T = 1이다.
    - 요청 1절(7.1 새 coverage 층: 24 B fragment, 8×8 타일 청크 목록 + 타일 헤더 zMin/zMax·opaqueCovered, bDepth, 집합체 레코드 32 B)은 V가 구현하면서 7.1과 `ViewResources`를 바꾼다. 그때 옛 16 B 정렬 목록(v1.25)과 "브릭 fragment 0x7F"를 폐기한다. 그 전까지 v1.25 층은 기본 꺼짐 그대로다.
    - 요청 2절(7.3 fragment·집합체 가시성, S), 5절(8.1 Aggregate, M), 6절(6.5 클러스터 메타데이터, V), 8절(`FrameResources::rayHits/rayHitVisibility`, S `shadowHitVisibility`, R·S)은 해당 트랙이 구현할 때 이 문서에 넣는다(순서: 요청 10절).
    - 요청 7절(코어 밴드 패스 그룹 `RenderGraph::addBandedGroup`, `output.band_count`)은 마이크로벤치 3(`--only-bands`) 결과 뒤에 확정한다. 요청 9절 마이크로벤치 4개는 설계 개정 세션이 자기 폴더에서 만든다(조율 결정).
  - **코어 도구·장치**(조율 요청):
    - 장치 제거 보고: 종료 코드 87, `UNX_DEVICE_REMOVED` 마지막 줄, GpuLock `DEVICE_REMOVED` 기록(3.3).
    - GpuLock.ps1이 GUI 서브시스템 실행 파일을 기다린다(3.3).
- v1.27 (2026-09-25):
  - **I 요청 `20260925_I_device_removed_policy.md` 반영**: 장치 제거 정책 `setDeviceRemovedPolicy(Exit | Throw)`, `DeviceRemovedError`, `deviceWasRemoved()`(3.3). 호스트 DLL은 Throw다(`UnityPluginLoad`에서 설정, 호스트 장치 위의 첫 장치도 Throw). 신호·대기·waitIdle은 제거 뒤 던지지 않고 돌아온다. [실측] 단위 테스트: Exit 자식 프로세스는 종료 코드 87, 마지막 줄 `UNX_DEVICE_REMOVED test.child hr 0x887A0006 ...`. Throw는 `DeviceRemovedError`(where, hr)를 던지고, 이어진 `waitIdle`은 던지지 않는다(30/30).
- v1.28 (2026-09-25):
  - **GpuLock `-Kind timing|correctness`**(조율 요청, 3.3): 종류를 `current.json`과 `history.log`에 기록한다. 백그라운드 CPU 작업의 멈춤 규칙은 timing만 대상이다(C의 PauseGate는 `kind`를 읽도록 C가 맞춘다). history 줄의 형식이 `acquire <트랙> (<종류>) :: ...`로 바뀌었다.
  - **M 요청 `20260925_M_planar_mask_apron.md`(R 동의)**: `ViewDesc::planarMask` 값이 1 = 거울 픽셀(R이 읽음), 2 = 에이프런(거울 픽셀의 3×3 이웃, 그리고 셰이딩하지만 R은 읽지 않음), 0 = 건너뜀이 됐다. `planarTileMask`는 팽창된 마스크 기준이다. V·S·M은 "0 아님 = 그림" 그대로라 바뀌는 것이 없다(V의 64 px 컬링 마스크와 깊이 채움은 이미 0 아님으로 판정한다). R의 resolve만 "== 1"로 읽는다. 마스크 생성은 R 몫이다.
  - **클러스터 채움 품질 키(V, 6.5)**: `visibility.cluster_min_triangles`(메시렛이 연결되지 않은 이웃에서 끝날 수 있는 최소 삼각형 수), `visibility.sheet_orientation_min_width`(m, 이보다 좁은 평판 성분은 방향 부류로 나누지 않는다). `cluster_vertices`의 상한은 128이다(V 메시 셰이더 출력).
    - 기본값은 이전 클러스터링과 같다(21, 0, 64). [실측, CPU, 인스턴스 가중 소스 클러스터당 삼각형, 64 기준] forest_thin 20.3(수목 21, 풀 6), forest_card 13.2, waterside 19.7, city 41.4.
    - 원인: 수목 21 = min_triangles다(흩어진 잎마다 flex 빌더가 min에서 끊는다). 풀 6과 카드 2는 성분마다 144칸 방향 부류로 나뉜 것이다.
    - 후보(128 정점, min 64, 폭 0.016 m)는 CPU로 forest_thin 42.2(수목 42, 풀 64), waterside 26.5, city 48.4다. 가는 전선처럼 경계 구가 커지는 메시(1.9 → 7.5 m)가 있어, 기본값은 GPU A/B(컬링, 래스터, 대역 수, 번갈아 중앙값·P95) 뒤에 정한다.
    - 테스트 `cluster_fill_of_scene_meshes`(C 트랙이 있는 빌드)가 채움을 보고한다.
- v1.35 (2026-09-25):
  - **이력 불연속(5.5.2, I 요청 d07bca2 계열, S·R·M 목록)**: `FrameContext::discontinuity`(`kDiscontinuityRestore`, `kDiscontinuityCut`), 메인 뷰 이전 뷰 재설정, `GpuScene::resetMotion`, `kTransformTeleport`(6.3). 전체 렌더러의 결정성은 결정 대기다(R 비용과 함께).
  - **GI 광선 배분 입력(10.3, R·I 합의)**: `FrameContext::gpuSimulation`(`kGpuSimulationSoft/Vfx/Rigid`). 품질 키 `gi.rays_per_frame`은 이름과 뜻(프레임당 평균)을 그대로 둔다. 배분, 무게, 누산기는 R의 GiSystem 안이다. `giRaysThisFrame`(5.5)은 R이 GPU 진단용으로 채운다.
  - **`GpuScene::palette(instance)`(R 요청)**: 스킨 프록시 자세 편차 한계용 CPU 팔레트 접근자.
- v1.84 (2026-10-01, R: VSM 래스터 목록 넘침의 구조 수정, 결함 큐 1, 조정 결정 13:40):
  - **원인**: 기차 라운지의 0프레임 태양 래스터가 가시 클러스터 1.67 M / (클러스터, 타일) 쌍 2.07 M을 요구해 `visibility.max_visible_clusters`(1,048,576)를 넘었고, 캐스터가 빠진 페이지가 캐시에 남았다(f60에 104k 화소가 태양 그림자 없음).
  - **`DepthRaster.h`: `DepthRasterOverflow { frame, bits }`, `DepthRasterOverflows`(TrackState 키 `kDepthRasterOverflowKey`)**: V가 래스터 실행(요청 이름)마다 목록이 넘친 가장 최근 프레임과 넘침 비트를 적는다(통계가 돌아오는 framesInFlight 프레임 뒤). 실행 결과를 보관하는 요청자(S의 페이지 캐시)는 그것을 다시 그린다.
  - **S 오류 비트 `VSM_ERR_RASTER_OVERFLOW = 0x20`**(3.6, CPU에서 세움): 넘친 실행이 있으면 S가 모든 페이지를 다시 그리고 국소광 세대를 올린다(안전장치. 아래 묶기가 켜져 있으면 일어나지 않는다).
  - **요청 묶기 `shadow.vsm.raster_split`(기본 true, false = 이전: 태양 요청 하나, 국소광 6개씩)**: S가 V 래스터 요청을 뷰별 구조적 상한(그 뷰가 닿는 캐스터의 컷 상한 합, 국소광은 큐브 면별 절두체)이 가시 목록 용량에 들도록 나눈다.
  - **`ClusterData::MeshRange::cutBound`**(GpuScene.h): 메시의 어떤 LOD 컷도 넘지 않는 클러스터 수. 빌더가 "모든 그룹의 단순화 결과 ≤ 그룹의 클러스터 수"를 확인하면 잎 클러스터 수, 아니면 전체 클러스터 수. 0 = 모름(clusterCount를 쓴다).
  - **`visibility.raster_amplification`(기본 true, false = 이전: 저장된 쌍 목록)**: 타일 국소 래스터(`DepthRasterRequest::tileLocal`)가 쌍을 저장하지 않는다. 컬이 가시 클러스터마다 항목 하나와 타일 사각형을 쓰고, 증폭 셰이더(`DepthRaster.as.hlsl`)가 행별 접두합으로 (클러스터, 타일 런) 메시 그룹을 띄운다. 쌍 용량이 없으므로 쌍 넘침이 없다. 타일 격자는 256 × 256 이하(V가 확인).
  - **`MeshPipelineDesc::amplificationShader`**(Shaders.h): 커널 이름(as_6_6), 빈 문자열 = 없음.
  - 정확성 [실측, 2026-10-01]: 그림자 층 f0 = f60 = f300 비트 동일(기차 라운지 1080), f0 = f300(욕탕 라운지 1080), 넘침 0, 오류 비트 0x0, VisibilityTests 9/9, VSMTests, LocalShadowTests, 클러스터 빌더 테스트 15/15.
  - 비용 [실측, 게임 실행 중 측정, 패스 최소값 합]: 래스터 요청 수 4 → 14(기차), 14 → 23(욕탕 라운지). 요청 하나 = 컬 체인 19패스, 바닥 약 0.047 ms. 프레임 바닥 +0.8 ms(기차 1080), +0.5 ms(기차 1440), +0.6 ms(라운지 1080), +0.7 ms(라운지 1440). AS 래스터 패스 자체는 요청당 +0.001–0.003 ms. 이 비용은 큐 1b(2단 컬: 뷰별 가시 수 → 접두합 → 쓰기)로 되찾는다.
- v1.82 (2026-09-27, 렌더 A: A3 발광 입자 광원 쓰는 쪽 + 렌더 C의 메시 입자 합류):
  - **`tracks::particleLightCapacity(TrackState&, GpuScene&)`**(프레임 import·frame constants 전)와 **`tracks::particleLights(fc, main)`**(simulation 바로 뒤, V·S 전): 최신 tick의 빛 플래그 행(NV_STREAM_PROGRAM_LIGHT, 발광 스프라이트)마다 점광원 1개. I = Σ α L π(s/2)², 위치 = Y 가중 중심, 색 = I/Y, size.x = Y 가중 RMS 반경 + 평균 반경, range = √(Y·노출/(π 2⁻¹⁰))(창이 버리는 조도 < 현재 노출의 1 표시 코드), 그림자 없음. 청크(≤ 2048 입자) 그룹 트리 합 → 행별 순서 합: 원자 연산 없이 결정적. 용량은 max(F, 2 × 용량, 16)으로만 는다.
  - **`tracks::particleMeshes(fc)`**(렌더 C, simulation 뒤): FrameRenderer가 부른다.
  - **호스트 `UnxVfxMapMeshAsset(UnxRenderer, uint64_t asset, uint32_t mesh)`**: 커밋 메시 색인, 런타임 메시 id(비트 31) 또는 0xFFFFFFFF(해제). 바뀔 때 프레임 패킷으로 가고 렌더 스레드가 런타임 id를 GPU 색인으로 풀어 `fx::meshAssets`에 넣는다(제거된 런타임 메시 = kNone, 그리지 않고 unmapped로 센다). 커밋 때 `RuntimeCapacity::gpuInstances = fx.particles.mesh_instances_max`.
- v1.81 (2026-09-27, 렌더 B 요청: FX 광원 꼬리의 그래프 순서):
  - **`FrameResources::fxLights`**(장면 광원 버퍼 전체, stride 80)와 **`fxLightCount`**(StructuredBuffer<uint>, 요소 0 = F): core(FrameRenderer)가 프레임마다 한 번 import한다(RenderGraph::importBuffer는 같은 자원의 여러 import를 합치지 않는다). 쓰는 쪽(FX)은 Uav, S·R·M은 Srv로 선언한다. 꼬리가 없으면 무효.
  - v1.80 `giIrradiance`의 M 읽기는 **넣지 않았다**. D0 4K GpuLock 2쌍 [실측, Results/M/GiScreenAB]: m.lit.shade.b0 3.33/3.45 → 1.83/1.87 ms, r.gi.screen +1.38/+1.53 ms, 패스 합 9.652 → 9.612, 9.834 → 9.806 ms(변화 없음). M이 읽지 않으니 r.gi.screen은 컬링된다.
- v1.80 (2026-09-27, 렌더 B 요청: M의 GI 캐시 조회를 셰이딩 밖으로):
  - **`ViewResources::giIrradiance`**(RGBA16F, 주 뷰 W×H): R의 `r.gi.screen`이 globalIllumination 끝에서 쓴다. rgb = M과 같은 표면점(카메라 + mPixelRay × 선형 z, 법선은 보는 쪽으로)의 `giCacheIrradianceScreen` × g_exposure, a = 1(캐시 값 있음) / 0(M이 프로브를 씀). 앞면만이고 Foliage 뒷면은 M이 직접 조회한다. 무효면 M이 전처럼 직접 조회한다.
- v1.79 (2026-09-27, 렌더 A: A3 FX 발광 입자 광원 core, 계약 S_STATUS 10):
  - **`GpuScene::setFxLightCapacity(F_max)` / `fxLightRange()`**: 장면 광원 버퍼 뒤 F_max개 꼬리(gpu::Light, UAV 가능, 0으로 시작)와 개수 버퍼(요소 0 = F, 용량이 바뀔 때마다 0). N + F_max > 32,768이면 거절하고 꼬리는 그대로다(`kMaxSceneLights`; S의 16비트 목록 항목은 비트 15가 그림자 플래그라 색인은 0x7FFF까지, 렌더 B 정정). 쓰는 쪽(FX)은 gpuInstanceRange처럼 자기 장벽(SRV → UAV/복사 → SRV)으로 매 프레임 레코드와 F를 쓴다.
  - **`FrameConstants::fxLightCount`**(StructuredBuffer<uint> SRV, kNone = 꼬리 없음)와 **`fxLightCapacity`**(F_max): 이전 framePad0/1 자리(576 B 그대로). HLSL `g_fxLightCount`, `g_fxLightCapacity`. 읽는 쪽 광원 범위는 [0, lightCount + min(F, F_max)).
  - 원점 재설정 뒤 CPU 광원 거울(`GpuScene::lights()`)이 GPU 표와 같아졌다(이전에는 재설정 전 값으로 남아 있었다).
- v1.78 (2026-09-27, 엔진 2 W: W2 닫힌 수체(욕탕·수영장), FEATURES_GAME 1.10):
  - **`render::PoolFrame`, `render::PoolSourceFrame`, `FrameContext::pools` / `poolCount`**: 이 프레임의 수조와 이 프레임의 원천.
    - 수조: id(0이 아니고 프레임 안에서 유일; 물결 상태의 열쇠 — 목록에서 빠지면 해제), 재질(Water 클래스), 안쪽 크기 Lx·Lz, 균일 수심, 표면막 0/1, 고요 수면 중심(이 프레임 좌표, y = 물체가 없을 때 수위), yaw.
    - 원천: {x, z, 반지름 σ, 충격량 N·s(아래 +), 밀어낸 부피 변화 m³(+ 밀어냄)}. 한 원천은 정확히 한 프레임에 간다.
  - **W `waterGeometry`가 `water::poolGeometry`를 먼저 부른다**(PoolTrack.cpp): 어느 뷰(주 뷰, 보조 뷰)에 보이거나 원천이 있는 수조만 진행하고, layer 1 TriangleStream(재질 = 수조 재질)을 넣는다. 보이지 않고 원천도 없는 수조는 다음에 그려질 때 경과 시간만큼 정확히 따라잡는다. 복원(kDiscontinuityRestore)은 모든 수조를 잔잔하게 시작한다(물결은 World 상태가 아니다).
  - **호스트**: `HostRenderer::setPools`(보유, 월드 좌표, 호출 스레드에서 검증: id, 크기, 수심, 막 0/1, 유한) / `addPoolSources`(다음 대기 프레임 하나에; 알 수 없는 수조·수조 밖·반지름 0·유한 아님은 거부). 대기 프레임은 원점 이동을 뺀 자기 좌표로 받는다. 그려지지 않고 버려진 프레임의 원천은 다음 프레임으로 옮겨진다(변환처럼 이동량을 뺌).
  - **ABI 6 선택 내보내기** `UnxFrameSetPools(UnxPoolDesc[64 B])`, `UnxFrameAddPoolSources(UnxPoolSource[32 B])`. C# `UnravelNextRenderer.TrySetPools` / `TryAddPoolSources`(Unity 브리지, 렌더러 축 = z 반전).
  - 시험: `unx_test_water_pooltests`, `unx_test_host_hostpools`(하드웨어 통과: 원점 이동 뒤 좌표, 한 번만 전달, 원천이 있는 프레임 렌더, 거부 9가지, 디버그 층 오류 0).
- v1.77 (2026-09-27, 엔진 1 W: 물 단계 2, 물 아래 band A의 태양광; 렌더 A 합의):
  - **`FrameResources::waterSunDepth`(D32, 태양에 가장 가까움 = 1, 물 없음 = 0), `waterSunNormal`(RG32F 팔면체 법선, +y 극: 2026-09-27에 RG16F·+z에서 바꿈. 물 법선이 fp16 4.9e-4 간격 구간에 있어 코스틱 착지가 4 m에서 텍셀의 1/4씩 흔들렸다. 읽기는 WaterLight.hlsli waterOctDecode만 쓴다), `waterSunMedium`(RGBA16F: 1 m 투과율 RGB, IOR), `waterSunConstants`(raw 80 B, 0번 워드 = 유효)**: W의 모든 삼각형 스트림을 태양 방향 직교 투영으로 래스터한 지도다(WaterSunMap.ms/.ps, waterGeometry에서 V·M 앞).
    - 범위는 스트림 경계 상자 합집합이다. 텍셀은 긴 변 / N이다(N = 2 mm 이하가 되는 2의 거듭제곱, 256~2048).
    - 매질은 스트림 재질에서 온다: baseColor = 1 m 투과율, ior.
  - **`Passes/Water/WaterLight.hlsli` `waterSunLight(depthSrv, normalSrv, mediumSrv, constSrv, X, sunDir, ior, out lightDir, out transmittance)`**: 태양의 곧은 광선이 X 위에서 물을 지나면 true를 낸다. 그때 값은 다음과 같다.
    - lightDir = −(S에서 햇빛의 정확한 스넬 굴절)
    - transmittance = (1 − F(θ_s)) (cos θ_s / cos θ_t) T^d. d는 X에서 S의 접평면까지 lightDir 방향 거리다. cos 비는 수면을 지나며 빔이 좁아지는 몫이다. 호출자는 lightDir로 E × transmittance를 쓰므로, 수평 바닥 조도가 E (1 − F) cos θ_s T^d로 보존된다(시험의 에너지 검사 3.5e-4).
    - 평평한 면에서 정확하다. 수평 램버트 바닥의 조도는 E cos θ_s (1 − F) T^d다. 굽은 면의 오차 조건은 FEATURES_GAME 1.9에 있다.
  - **코스틱(추가, 같은 v1.77)**: `FrameResources::waterSunCaustics`(R32_UINT 배열, 5겹, min(지도 텍셀, 1024)² 격자)와 `waterSunLight(…, constSrv, causticsSrv, X, …)` 오버로드(첫 형태는 causticsSrv = UNX_NONE과 같다).
    - WaterCaustics.hlsl: 지도 텍셀마다 햇빛을 정확한 스넬로 굴절시켜, 입사점 법선 방향 수심 z_k = 0.25·2^k m(k = 0..4)에서 내린 점을 X와 같은 태양 방향 투영 좌표에 쌍선형 고정소수점으로 뿌린다.
    - 받는 쪽도 자기 수면점 법선 방향 수심으로 겹 사이를 log2 보간한다. 평평한 물은 격자가 똑같이 밀리므로 정확히 1이다. 에너지는 보존된다.
    - 적재 +2(겹 두 개). [실측, 하드웨어] 평평한 물 9 864점에서 투과율 불변 8.6e-4.
  - 비용: 지도가 없으면 적재 1회, 있으면 4회. 렌더 A가 ShadeOpaque·폴백에서 부르고(l = lightDir, E ×= transmittance), coverage 조각은 합성 분할 뒤에 부른다.
  - 조건: VSM 그림자는 곧은 태양 방향이다(굴절 그림자는 S). 하늘·GI의 물속 감쇠는 R과 함께. 코스틱은 같은 지도로 뒤에.
  - 시험 `unx_test_water_waterlighttests` [실측, 하드웨어]: 평평한 수조와 20° 기운 판(매질·IOR 다름), 점 16 692개. 경계 822개를 빼고 분류 불일치 0, 방향 2.8e-4, 투과율 상대 9.3e-4.
- v1.76 (2026-09-27, 렌더 A: A9 clearcoat 층, MATERIAL_LAYERS 1.1·1.3; 1단계 데이터·모델, 2단계 GPU 레코드·미러):
  - **`scene::Material`**: `clearcoat`(덮임 c), `clearcoatRoughness`(r_c), `clearcoatIor`(표로 만든 코트 1.5 또는 1.33만, Standard 클래스). 장면 파일 "COAT" 블록.
  - **정의(권위: `scene::model::evaluateCoated`)**: C가 측정한 R1 + A2 + S(`Results/C/MaterialLayers/clearcoat_r1e.md`: 유전체 기저는 3절 기준 통과, 코트 아래 금속 기저는 lobe 모양 실패 = 재설계 대상)를 표 형태로 쓴다. 표는 코트마다 E_c, E_ms, A_x, B_x(μ, r), K_ms, Ā, B̄(r), K_0이고 η 1.33 표는 `unx_study_material_layers tables 1.33`으로 만들었다. 표 형태의 3절 기준 측정은 `clearcoat_model` 연구로 하며, 사용자 게임이 끝난 뒤 CPU에서 돌린다.
  - **GPU**: `gpu::MaterialLayers` 64 B(별도 버퍼 `FrameConstants::materialLayers`), `MaterialLayered` 플래그(classFlags 비트 10)와 레코드 번호(비트 16~31). `FrameConstants::coatTable`(코트마다 4224 float). FrameConstants 560 → 576 B(끝에 materialLayers, coatTable, 예비 2).
  - **HLSL 미러**: `MaterialModel.hlsli` `modelEvaluateCoated`, `modelCoatOf(GpuMaterial)`, `modelFresnelDielectric`.
  - 시험: unit clearcoat_model(정의·표·장면 블록), clearcoat_model_on_the_gpu(4096점, 두 코트, 금속·유전체, 최악 상대 1.39e-6 [실측, WARP]). unit 시험 장치는 `UNX_WARP=1`로 WARP를 쓸 수 있다.
  - 남음: M 셰이딩(층 셰이드 클래스, 태양 원반·면광원·간접), R 적중·C 기준 요청, 호스트 ABI·Unity 대응(URP Complex Lit `_ClearCoatMask`/`_ClearCoatSmoothness`).
- v1.75 (2026-09-27, 렌더 A: 미리 셰이딩된 coverage 기록(v1.73 목록의 M 쪽), W 유체·바다 기록과 Cut·Terrain 대역 B의 합류):
  - **`ViewResources::coverageRecordRadiance`**(M, raw): coverageRecords 원소마다 8 B uint2 { f16 r | f16 g << 16, f16 b }. 단위는 합성의 단위(노출된 선형 복사휘도, 앞쪽 공기 포함: (L × T_air + L_inscatter) × g_exposure). coverageSpecial에 있는 원소만 정의된다.
  - **순서(M shade 안)**: m.lit(대역 A) → `m.coverage.special default`(종류 1·2에 0) → `tracks::water(fc, target)`(W가 종류 2를 덮어씀) → `m.coverage.special material`(Cut, Terrain 각각: 클래스 재질을 목록 칸마다 48 B 임시 버퍼로) → `m.coverage.special`(종류 5 조명) → 합성. 합성과 heavy round는 visId 상위 비트가 00이 아닌 기록을 버퍼에서 읽는다(P[8].y). 면적 × 가려지지 않은 몫으로 가중하고, 뒤의 대역 A는 나머지만 받는다.
  - **`ViewResources::bandARadiance`**(M, RGBA16F): E·coverage 합성이 조각 뒤에 두는 대역 A 복사휘도. W는 내부 물 픽셀을 `target.color`와 이 텍스처 둘 다에 쓴다(물 위 조각이 물 위에 합성되도록). 물이 있는 뷰는 `postActive`가 참이라 float 영상이다.
  - **보조 함수**: `Passes/Shading/CoverageSpecial.hlsli`의 `covRecordPixel`(타일 목록 이분 탐색 + 기록의 타일 내 비트), `covPackRadiance`, `covUnpackRadiance`.
  - **V 목록(CoverageRaster.ms)**: M 선셰이딩 클래스 = Cut, Terrain(렌더 C 정지 중이라 A가 한 줄 반영).
  - DXIL: CoverageComposite 194.4 KB(이전 198.8 KB), CoverageSpecial 재질 45~48 KB, 조명 141/173 KB. 커널 하나에 재질과 조명을 합치면 202~234 KB라 단계를 나눴다.
  - 시험: ShadingTests 13(`--preshade`)은 같은 기록으로 Standard 재질과 그와 같은 1레이어 Terrain(종류 5)을 비교한다(f16 반올림 이내). 하드웨어 실행은 GPU 보류 뒤. WARP는 셰이딩 영상이 전부 0이라 판정에 쓸 수 없다.
- v1.74 (2026-09-27, 렌더 A: Terrain 재질 클래스, C5 쿠킹 지형 타일의 합류; FEATURES_GAME 9 "직접 블렌딩 경로"):
  - **`scene::MaterialClass::Terrain` 7**: `Material::terrainSplat[2]`(Rgba8Linear, 레이어 i의 가중 = 스플랫 ⌊i/4⌋의 채널 i mod 4)·`terrainLayers`(1..8개 { Standard 재질, uv 척도, 오프셋 }). 장면 파일의 "TERR" 블록, `scene::validate`(레이어 재질은 Standard, 4개 넘으면 스플랫 1 필요).
  - **GPU**: `gpu::Material::terrainLayers`(예약 칸 자리: 첫 레이어 | 개수 << 24), `gpu::TerrainLayer` 32 B { 재질, 척도, 오프셋, 스플랫 0·1 크기(폭 | 높이 << 16) }, `FrameConstants::terrainLayers`(framePad0 자리). M 텍스처 세트는 스플랫 SRV를 occlusion·slopeRange 칸에 싣는다(이 클래스에는 없는 슬롯).
  - **해석(M, MaterialTerrain.hlsli)**: 스플랫 텍셀 중심은 지형 모서리(Unity 규칙, uv0 × (N−1)/N + 0.5/N), 가중 = footprint 필터 표본 / 합, 기본색·거칠기·금속 = 가중 합, 법선 = LEAN 혼합(평균 Σw m, 분산 Σw(v + |m|²) − |m̄|²).
  - **호스트**: `UNX_MATERIAL_CUT` 6·`UNX_MATERIAL_TERRAIN` 7, 선택 내보내기 `UnxSceneSetTerrainLayers`(ABI 6 안, 커밋 전; `UnxTerrainLayer` 32 B). Unity 브리지는 쿠킹 타일("UnravelNext Terrain Cook" 자식)의 재질을 지형의 Terrain 재질로 바꾼다(TerrainLayer → Standard, 알파맵 → 스플랫).
  - 아직 없는 곳: 대역 B coverage 조각(coverageSpecial 종류 5로 M 선셰이딩, Cut과 같이), R 적중 셰이딩과 C 기준(RtScene)은 상수 색을 본다 → 각 트랙 요청.
  - 시험: MaterialTests 7(--terrain, WARP): 6 레이어·스플랫 2장, 286,898 px, 기본색 ≤ 0.87 sRGB 코드(한계 2.5), 법선 8.5e-4(한계 5e-3); unit scene_roundtrip_and_validation(TERR 왕복·거부 2건). 하드웨어 실행은 GPU 보류 뒤.
- v1.73 (2026-09-27, 렌더 C: 특수 기록 목록(A 요청), M 선셰이딩 클래스(A 요청), A14 보조 뷰의 V·프레임 부분, 바다 뷰 격자의 V 부분(W 합의)):
  - **`ViewResources::coverageSpecial`**(raw): 머리 { 개수, DispatchIndirect(64/그룹, 1, 1) }, 이어서 uint2 { coverageRecords 원소, 종류 }. 종류 1 머리카락(COV_HAIR_ID), 2 스트림(COV_STREAM_ID: 물 가장자리·유체·바다), 5 M 선셰이딩 클러스터 기록. scatter가 파동당 원자 1회로 채우고 순서는 정해지지 않는다. 용량은 필요량의 1.5배(바닥 `visibility.coverage_special_min`)이고 넘치면 OVERFLOW_COVERAGE_SPECIAL. 소유 패스가 `coverageRecordRadiance[element]`에 미리 셰이딩하고 합성은 값만 읽는다.
  - **`COV_PRESHADE_ID` 0x40000000**(기록 visId 접두 01): CoverageRaster.ms가 M 선셰이딩 클래스(지금 목록: Cut)의 클러스터 기록에 켠다. 해독은 `coverageClusterVisId(id)`. V 설정이 `visibility.max_visible_clusters` ≤ 2^23을 강제한다(클러스터 visId < 2^30).
  - **A14(V·코어)**: `ViewResources::viewId`(주 뷰 0), `ViewKind` RenderTexture 2·Mirror 3·Portal 4·Split 5, `FrameContext::auxViews`(AuxView { id, view, output, reads }; reads 위상 순서, 순환은 직전 프레임), FrameRenderer 순서 = 모든 뷰 visibility → shadowPages 한 번 → 보조 뷰 decals·resolve·shadowVisibility·shading → 주 뷰 resolve 이하(요청 20260926_C_per_view_history 5절). V는 뷰 id별 상태(HiZ, coverage, 이전 위치)를 두고 framesInFlight + 1 프레임 안 보인 뷰를 해제한다. **결함 수정**: 크기가 바뀌어 HiZ를 새로 만들면 history를 버린다(전에는 1단계 가림이 초기화 안 된 HiZ를 읽었다). 보조 뷰의 프록셀·GI·반사·뷰별 S 표시는 트랙 일반화(요청문) 뒤다.
  - **바다(W 뷰 격자)**: `FrameResources::oceanDepth`(R32F 선형 깊이, +inf = 없음)·`waterSurface`(RGBA32F, W 형식)를 W가 waterGeometry에서 채운다. V는 물 층 패스에서 PlanarFill.ms + OceanLayer.ps(SV_Depth, GREATER)로 합쳐 waterVis = `COV_OCEAN_ID`(스트림 슬롯 63 예약, `kMaxTriangleStreams` = 63), waterDepth = oceanDepth(비트 동일)로 둔다. `ViewResources::oceanEdgePixels`(raw: 머리 { 개수, 인자 }, y << 16 | x; 바다 가장자리와 윤곽 바깥쪽, 바닥 `visibility.ocean_edges_min`)를 만든다. **`FrameServices::coverageAppend`**: V가 coverage 래스터 뒤, count 앞에 부르는 갈고리이고 `ViewResources::coverageState / coverageStream / coverageKeys / coverageCapacity / coverageTilesX`와 CoverageLayer.hlsli `coverageAppend(...)`로 기록을 덧붙인다(W의 가장자리 32 부표본 패스).
  - 시험(하드웨어, 디버그 층 오류 0, TDR 없음): unx_test_visibility_auxviewtests(주 뷰·보조 뷰 이력 분리, 크기 변경 프레임의 1단계 지연 0, 보조 뷰 깊이 = 새 주 뷰 깊이 24,000 px 비트 동일), unx_test_visibility_oceanlayertests(합성 바다 평면: 바다 id·깊이 정확, 가장자리 목록 = CPU 규칙), wateredgetests(특수 목록 640개, 불량 0), haircoverage·translucent·waterlayer·streamcoverage·fragment·instancehierarchy·morph·originrebase·runtimepool 통과.
  - 2026-09-26 23:15 TDR(haircoveragetests, 사용자 게임 중) 원인 가르기: 게임 없는 하드웨어에서 세 변경을 켜고 끈 두 실행 모두 통과(결과 동일) → 게임과의 GPU 경합으로 판단. WARP는 이 시험에서 쓸 수 없다(RayScene 가속 구조 빌드 중 d3d10warp.dll 정수 0 나누기, R 경로). visibilitytests coverage_layer_is_exact는 커밋된 main(1a616eb)에서도 같은 자리에서 실패한다(이 변경과 무관, 렌더 A에 알림).
- v1.72 (2026-09-27, 렌더 A: B7 바다 입력, 엔진 1 W 제안):
  - **`FrameContext::ocean`(core)와 `render::OceanFrame`**: 바람 속도·방향(rad)·fetch·spread·seed(unx::water::OceanDesc), 수위(이 프레임 좌표), 가장 거친 바다의 수평·수직 변위 한계 R·A(m), 물 몸체(0 열린 바다, 1 원형 호수: 중심 (x, z)·반경). null이면 바다가 없다. record()가 끝날 때까지 유효하다. 시간은 `time`, 카메라는 mainView다. W의 waterGeometry가 FFT와 뷰 격자로 `FrameResources::oceanDepth`와 `waterSurface`를 채운다(두 칸과 바다 id `COV_STREAM_ID | 63 << 24`, 가장자리 kind 4는 W·V 합의).
  - **호스트**: `HostRenderer::setOcean(const OceanInput*)`(null = 끔)와 선택 export `UnxFrameSetOcean(r, const UnxOceanDesc* 72 B)`. 설정은 상태다. 입력은 월드 좌표이고, 각 프레임은 큐에 넣을 때 그때까지의 원점 이동(1024 m 격자)을 수위와 호수 중심에서 뺀 값을 받는다. 스펙트럼은 바뀌지 않는다. W는 FFT 위상을 위해 `originShift` 누적(1024 m 법)을 스스로 유지한다. 검증은 호출 스레드에서 한다: 유한값, 바람 ≥ 0, fetch > 0, spread ≥ 0, R·A > 0, 호수면 반경 > 0.
  - `HostRendererOptions::standaloneDevice`: 주어진 장치(WARP 등)에서 독립 렌더러를 만든다(디버그 계층 없음). 소프트웨어 어댑터의 독립 프레임 경로는 시스템의 디스플레이 mux 조회(`QueryMuxDListForApplication`)에서 정수 0 나눗셈으로 멈추므로, WARP 시험은 프레임 없는 부분만 돈다.
  - [실측, WARP] `unx_test_host_hostocean --warp`: 원점 이동 (1024, −2048, 3072) 뒤 수위 2.5 → 2050.5, 호수 중심 (100, −40) → (−924, −3112), 스펙트럼 불변. 잘못된 입력 5종 거부. 프레임 렌더 부분은 하드웨어 보류가 풀린 뒤 돈다.
- v1.71 (2026-09-26, 렌더 C, A11 파괴 단면 재질):
  - 8.1 Cut 정의(위). 클러스터 삼각형 워드 비트 24..26 = Cut 삼각형의 경계 변(다른 재질 삼각형은 0). 지금의 판독은 모두 하위 24비트만 가려 읽는다.
  - `Passes/Material/CutFace.hlsli`: `cutFaceProjection`, `cutFaceWhiteout`, `cutFaceEdgeDistance`, `cutFaceDamage`, `cutFaceApplyDamage`. M 해석 합류는 렌더 A(MSurface에 객체 공간 p·도함수·bary·꼭짓점 추가).
  - CPU 기준 추적기(RtScene)가 Cut을 평가한다. GPU 기준(E)은 아직 Cut을 거부한다(엔진 2에 요청).
  - 시험: `unx_test_clusterbuilder cut_face_border_edges`(32 × 32 격자, 절반 Cut: 모든 LOD의 Cut 삼각형 1,980개, 단순화 클러스터 16개, 경계 변 354개 표시, 틀림 0, 외피 비트 0), 빌더 시험 15/15; `unx_test_reference cut`(축 방향 면 1,385 표본 = z 투영 탭, 손상 띠 79 표본 = 0.55 × 탭·거칠기 0.8, 45° 면 = x·y 탭 평균, 평평한 화이트아웃 법선 = 기하 법선), point·rect 기준 불변; CutFace.hlsli는 cs_6_6로 컴파일된다.
- v1.70 (2026-09-26, 렌더 A: B8 GPU 유체 입력, 엔진 1 제안):
  - **`FrameContext::fluids / fluidCount`(core)와 `render::FluidFrame`**: current/start 입자 버퍼(물리의 GPU 입자 FluidGpu.hlsl Particle, 원소 크기 = view의 stride ≥ 48·4의 배수, 현재 80 B, COMMON), 개수, stride, startValid, 이 프레임 좌표의 원점(double, 원점 이동 반영), dx, alpha(틱 안 시각), tick, domainCells, material(표면의 Water 클래스 장면 재질)이다. record()가 끝날 때까지 유효하다. W의 waterGeometry가 읽는다.
  - **호스트**: `HostRenderer::setFluids(FluidInput{NP_FluidGpuView*, alpha, domainCells}, stamp[6])`, 선택 export `UnxFrameSetFluids(r, UnxFluidInput 32 B[], count, stamp[6])`. 설정은 상태다(다음 호출까지 모든 프레임이 읽고, 빈 목록은 없음). 각 프레임은 목록을 실행하기 전 GPU 브리지에서 `prepareGraphics({current, start: READ}, 틱 stamp)`로 허가받는다(그래픽 큐 대기). 실행 뒤에는 그래픽 fence로 `commitGraphics`를 부른다. 예외가 나도 커밋해 브리지 잠금을 푼다.
  - [실측] `unx_test_host_hostfluids`: 물리 도메인에 예약·결합한 버퍼를 넘기면 유체 프레임마다 그래픽 제출이 1회 늘고, 유체가 없는 프레임은 0이다. 잘못된 view·stride·개수·alpha와 Water 클래스가 아닌 재질은 호출 스레드에서 거부한다. 디버그 오류 0.
- v1.69 (2026-09-26, 렌더 A: 머리카락 호스트, GPU 브리지 호스트; v1.67·v1.68은 렌더 C의 A6 투과 층과 coverageSpecial):
  - **머리카락 호스트(B10, E의 hair::HairSystem과 1:1)**: `HostRenderer::hairAddBody / hairTick / hairSetFrameFraction / hairRemoveBody`, 선택 export `UnxHairAddBody(UnxHairBodyDesc 112 B) / UnxHairTick(관절 3×4, UnxHairCapsule 32 B, 바람, dt) / UnxHairSetFrameFraction / UnxHairRemoveBody`.
    - 몸체 id는 호스트 거울이 정하고, 렌더 스레드가 연산을 순서대로 재생해 같은 id를 얻는다(해제된 id는 마지막 해제부터 재사용). 검증은 호출 스레드에서 E의 addBody와 Hair 클래스 재질 검사로 한다.
    - World 단계마다 순서는 tick들 → 분수 → 프레임이다. 건너뛰거나 버린 프레임의 연산은 이월된다.
    - [실측] `unx_test_host_hosthair` 통과(id 일치, 상태 생성, 잘못된 입력 거부, 제거·재사용, 디버그 오류 0).
  - **GPU 브리지 호스트(엔진 1의 84922cc)**: 렌더러 장치마다 GpuBridgeHost 하나. 선택 export `UnxAcquireGpuBridge / UnxReleaseGpuBridge / UnxGpuBridgeStatistics`(NRC_GpuBridge 136 B, NRC_GpuStatistics 240 B, 옛 Tnr* 모양). 소멸자는 quiesce 뒤 장치보다 먼저 해제한다. 프레임 합류(prepareGraphics / commitGraphics)는 B8 유체 표면 때 넣는다.
- v1.67 (2026-09-26, 렌더 C, A6 투과 층 — A 결정):
  - **투과 층**(주 뷰, coverage 층 켜짐, 장면에 유리·물 재질이 있을 때): 대역 A 폭의 유리(`MATERIAL_GLASS`)·물(`MATERIAL_WATER`) 클러스터는 대역 A에서 빠져 `LIST_T_BACK`·`LIST_T_NONE`으로 간다(대역 A는 그 뒤를 그대로 가진다; HiZ도 유리를 가리개로 쓰지 않는다). 두 단계 모든 항목을 band A 깊이 사본 위에 그린다(재질 알파 시험 포함): 개수(깊이 시험만) → 최근접(깊이 시험·쓰기).
  - **`ViewResources::translucentVis`**(R32_UINT, 이 뷰의 visibleClusters 기준 vis id, VIS_NONE = 없음), **`translucentDepth`**(R32F 선형 뷰 깊이, +inf = 없음), **`translucentClass`**(R8_UINT): 0 없음, 1 표본이 픽셀의 유일한 투과 면이고 픽셀 전체를 덮음, 2 픽셀의 투과 면들은 coverage 기록(투과 비트, 정확 면적·마스크·깊이)이고 표본은 쓰지 않음. 2의 조건: 투과 면 개수 ≥ 2, 또는 3 × 3 이웃에 개수가 1이 아니거나 다른 면(인스턴스·재질)이 있거나 깊이가 굽음(2차 차분 > 1e-3 × 깊이), 또는 중심에 투과 면이 없는데 이웃 중심에 있음(윤곽 바깥쪽: 면이 중심 없이 픽셀 일부를 덮을 수 있어 표본은 없음). 0은 3 × 3 어디에도 없음.
  - **M 합성**: 클래스 1은 표본(vis·깊이)을 픽셀 전체 투과 면으로, 클래스 2는 기록으로 앞에서 뒤로 투과를 누적한다. 물 층(v1.63·v1.64)과 대역 B 기록은 그대로 함께 온다.
  - 목록·상태 배치(V 내부): VS_LISTS 8, VS_LIST_PHASE1 48, VA_MESH 48, VA_COV_T_MESH 72. CoverageRaster는 `COV_RASTER_LIST`(P[6].x)·`COV_RASTER_TRANSLUCENT`(P[6].y)·`COV_TRANSLUCENT_CLASS`(P[7].z)를 받는다.
  - 시험 `unx_test_visibility_translucenttests`: 먼 불투명 배경, 유리 판 두 장(판 2가 판 1 뒤에서 일부 겹침), 판 1 모서리 앞의 불투명 판으로 된 장면에서 대역 A 안의 유리 0 픽셀; 윤곽 0.01 px 안과 그 이웃을 뺀 230,346 픽셀에서 클래스·면·깊이 불일치 0(깊이 최악 상대 4.3e-7); 투과 기록 6,818개 전부 투과 비트·클래스 2 픽셀 안; 가려지지 않은 판 2: 클래스 1 픽셀 2,861 + 기록 3,030.374 px² = 5891.374 px² 대 정확 5891.409(반올림 한도 1.578). 첫 실행에서 윤곽 바깥쪽 픽셀(중심 개수 0)이 기록을 못 받아 10.4 px²가 빠진 결함을 찾아 클래스 규칙을 고쳤다. V·S 시험 14개 통과(디버그 층 오류 0). 함께: `GpuScene::addRuntimeMesh`는 풀 대상 뷰가 없으면 거절한다(B 제안). 커밋 f0f1031.
- v1.66 (2026-09-26, 렌더 A·core: Hair·Cut 재질 필드, 엔진 2·렌더 C 합의):
  - **scene::Material**: `hairEumelanin`, `hairPheomelanin`, `hairBetaN`(0.3), `hairTilt`(0.0349 rad), `cutScale`(1), `cutDamageWidth`(0.01 m), `MaterialClass::Cut = 6`(HLSL `MATERIAL_CUT`). validate는 Hair(멜라닌 ≥ 0, β_N ∈ (0, 1], ior > 1)와 Cut(cutScale > 0, 폭 ≥ 0)을 검사한다.
  - **장면 파일**: 확장 블록 "HAIR"와 "CUTS"는 해당 클래스 재질이 있을 때만 쓴다. 그래서 형식 버전과 기존 장면의 바이트·contentHash는 그대로다. 읽기는 확장 블록(MRPH, HAIR, CUTS)을 순서대로 받는다.
  - **gpu::Material 80 → 112 B**(HLSL `GpuMaterial`): `hairAbsorption`(σ_a), `hairBetaN`, `hairTilt`, `cutScale`, `cutDamageWidth`, 예비 1워드(A9)다. `scene::model::hairAbsorption(m)`은 CPU·GPU 기준 추적기와 GPU 레코드가 같은 식을 쓰게 하는 공용 함수다.
  - 셰이더 ABI 도장이 바뀐다(Scene.hlsli). 모든 build 폴더를 다시 빌드해야 한다. [실측] unit 38/38(장면 왕복·흡수 식 포함). material·shading·decal hits·host decal·surface layers·visibility 시험을 통과했다.
- v1.65 (2026-09-26, 렌더 A: B11 사진 모드 호스트, 셰이딩 법선 규칙):
  - **사진 모드 호스트(FEATURES_GAME 17, E의 `GpuPathTracer`를 렌더러 장치 위에서)**: `HostRenderer::photoBegin(camera, PhotoSettings)`, `photoSave(exr, png)`, `photoEnd()`, `photoStatus()`, `photoScene()`. 선택 export는 `UnxPhotoBegin(UnxPhotoDesc 64 B)`, `UnxPhotoSave`, `UnxPhotoEnd`, `UnxPhotoGetStatus(UnxPhotoStatus 312 B)`(ABI 6 안)다.
    - 스냅숏은 호스트가 지금 보여 주는 장면이다: 최신 변환·자세·태양·가시성, C2b 런타임 메시와 인스턴스, C5 지형 패치(패치된 타일은 V의 무게중심 규칙으로 교체 삼각형을 뺀다), 최신 프레임 카메라 × 자세의 A12 뷰 모델. 렌즈는 호스트 렌즈를 쓰고, EV100이 유한하지 않으면 자동 노출의 마지막 값을 쓴다.
    - 켜져 있는 동안 렌더되는 프레임마다 `pass(halfSamplesPerFrame)`를 하고, `currentImageResource()`(COMMON)를 그래프에 가져와 M의 후처리 사슬로 출력한다. 출력 크기가 바뀌면 다시 시작한다. 끝난 뒤 첫 프레임은 이력 컷이다.
    - 저장: EXR은 `current()`의 선형 복사휘도 × 노출이고, PNG는 같은 프레임 사슬의 SDR 인코딩(16비트 RGB, WIC)이다.
    - [실측] `unx_test_host_hostphoto`: 표시 프레임 = EXR의 필름 곡선 + OETF(≤ 1 코드, 평균 +0.001), PNG = 표시 코드와 정확히 같음, 자체 추적기 렌더와 relMSE 2.6e-15, 크기 변경 시 재시작, 디버그 레이어 오류 0이다.
  - **`FrameRenderer::recordImage(graph, frame, image, output, sdrCopy)`(core)와 `tracks::imagePost(fc, view, image)`(M)**: 다른 곳에서 만든 노출 선형 영상을 장면 렌더 대신 후처리 사슬로 보낸다. GPU 장면은 그 프레임의 갱신을 계속 받는다.
  - **셰이딩 법선 규칙(8.1)**: 기준과 같은 두 규칙(기하 법선 쪽 유지, 시선 쪽 1e-4 굽힘)을 실시간 해석·coverage 조각·셰이딩에 넣었다. [실측] D0 고정 장면 1920×1080에서 순검정 점 2,869 → 0(가장자리 검출 끔, coverage 끔 변형도 0). `unx_test_material_materialtests` 5번: 등진 픽셀 73,697개에서 G버퍼 법선이 기준 굽힘과 5.1e-5 안으로 같고, 등지지 않은 픽셀 33,631개는 매핑 법선 그대로다.
- v1.64 (2026-09-26, 렌더 C, 물 층 가장자리 — A 결정; R 접근자 — B 요청):
  - **물 층 가장자리 기록**: layer 1 스트림은 coverage 층이 켜져 있을 때 가장자리 픽셀에만 투과 기록을 낸다(`COV_FLAG_WATER_EDGE`, `coverageWaterEdge`: 3 × 3 안에 이 스트림의 물이 아닌 픽셀·band A 뒤의 물이 있거나 물 깊이 2차 차분 > 1e-3 × 깊이). 그 밖의 물 픽셀은 물 층 표본이 픽셀 전체다. M은 가장자리 픽셀에서 기록을, 나머지에서 waterVis·waterDepth를 쓴다. 시험 `unx_test_visibility_wateredgetests`: 전체 표본 픽셀 13,266 + 가장자리 기록 515.250 px² = 13781.250 대 정확 13781.252.
  - **`GpuScene::runtimeSubmeshes(mesh)`, `runtimeMeshGeneration(mesh)`**(R의 런타임 BLAS): 살아 있는 런타임 메시의 서브메시 기록(메시 기준 인덱스 범위)과 슬롯 세대(그 슬롯에 추가될 때마다 +1). 런타임 인스턴스 = instances()[i ≥ staticInstanceCount()], 제거된 것은 슬롯 재사용 전까지 kInstanceHidden.
- v1.63 (2026-09-26, 렌더 C, 물 층 — A 결정, W 요청 `20260926_W_ocean_patch_stream.md`):
  - **물 층**: `TriangleStream::layer`(0 = coverage 투과 기록: 좁은 표면(B8 유체), 1 = 물 층: 넓은 표면(B7 바다·호수)). V가 주 뷰에서 layer 1 스트림을 band A 깊이의 복사본 위에 1표본 하드웨어 래스터(깊이 검사·쓰기, 양면)로 그려 `FrameResources::waterVis`(R32_UINT, COV_STREAM_ID | 슬롯 | 삼각형, 물 없으면 VIS_NONE)와 `waterDepth`(R32_FLOAT 선형 view 깊이, 물 없으면 +∞)를 낸다. band A(수중)는 그대로다. 프레임에 물 스트림이 없으면 둘 다 invalid. 근거 [실측] (ea6ff3d): 수면을 coverage 기록으로 넣으면 4K에서 약 5.5 ms. 남은 V 몫: W의 정제 함수(`oceanRefine`, 픽셀 셰이더에서 정확 깊이 또는 discard, SV_DepthLessEqual)와 가장자리 픽셀만의 coverage 기록(A 결정) — W의 include가 들어오면.
- v1.62 (2026-09-26, 코어·M, 렌더 A):
  - **루트 상수 32 → 48 DWORD(3절)**: `Device::kRootConstantCount = 48`, `Bindless.hlsli`의 `cbuffer PassConstants { uint4 P[12]; }`. P[8..11]이 새로 생겼다. 루트 시그니처는 48 + 루트 CBV 2 = 50 DWORD(한도 64)다. P를 32개 이하로 쓰는 커널과 `computeConstants(k, 32)` 호출은 그대로 유효하다. 필요한 곳만 48까지 쓴다. 이유: coverage 조각 커널(풀·잎)이 표면 층·날씨·광원 함수·머리카락 같은 프레임 전역 SRV를 받을 자리가 없었다. [실측] 전 트랙 빌드 뒤 단위 36/36, visibility 8/8, material·shading·post·lights·decalhits·photo·FX 층·water 3종·호스트 5종 모두 통과, 디버그 층 오류 0.
  - **coverage 조각의 광원 함수(A8)**: P[8].x = `FrameResources::lightFunctions`(없으면 UNX_NONE). CoverageShade의 점·스폿 조도에 ShadeOpaque와 같은 lightFunction을 곱한다.
- v1.53 (2026-09-26, E A12 1인칭 뷰 모델):
  - **`gpu::kInstanceViewModel`(1 << 29, HLSL `INSTANCE_VIEW_MODEL`, GPU 장면 전용)과 `GpuScene::setInstanceViewModel`**: E의 `Passes/ViewModel`이 카메라에 붙은 인스턴스를 매 렌더 프레임 그 프레임의 카메라 × 카메라 공간 자세로 둔다(`tracks::viewModelPrepare`, GPU 장면 flush 앞; 불연속은 순간이동). 그래서 카메라 기준 운동이 0이고(움직임 벡터·회전 블러), 그림자·반사·GI는 화면과 같은 기하를 본다. M은 이 플래그로 회전 블러에서 뷰 모델 픽셀을 뺀다.
  - **`FrameConstants::viewModelScale`(옛 `spare1`, HLSL `g_viewModelScale`)**: 주 뷰에서 뷰 모델 인스턴스의 투영 재매핑 k(clip.xy × k, 깊이 불변; 1 = 없음이 기본). `viewmodel.fov_override_degrees`가 정하고, 다른 뷰는 늘 1이다. V는 뷰 모델 정점의 현재·이전 clip에 `ViewModel.hlsli`의 `viewModelClip`을 부른다.
- v1.54 (2026-09-26, A12 호스트 경로, 엔진 2 요청):
  - **1인칭 뷰 모델 호스트 입력(E `viewmodel::ViewModels`)**: 선택 export `UnxViewModelAdd(r, instance, cameraLocal12, &id)` / `UnxViewModelSetPose(r, id, cameraLocal12)` / `UnxViewModelRemove(r, id)`. 자세는 객체 → 뷰 공간(x 오른쪽, y 위, −z를 봄)의 3×4 행 우선 행렬이고, 렌더러가 렌더 프레임마다 그 프레임의 카메라와 합성한다. 호스트가 거울로 id를 즉시 돌려주고 연산을 다음 큐 프레임과 함께 보낸다. 렌더 스레드가 순서대로 재생해 같은 id가 나온다(건너뛴·버려진 프레임의 연산은 이월). [실측] `HostViewModel`: 돌고 움직이는 카메라의 12프레임 모두 중앙을 덮었고, 0.5 m 자세 이동이 투영 예측과 0.08 px 안에서 맞았으며, 제거 뒤에는 장면 자리로 돌아갔다.
- v1.55 (2026-09-26, E A8 광원 함수):
  - **`FrameResources::lightFunctions`(raw, 무효 = 광원 함수 없음)와 `tracks::lightFunctions`(prepareScene 직후)**: E의 `Passes/Lights`가 광원 인덱스(`GpuScene::setLights` 순서)마다 쿠키·IES·고보·회전·세기/색 키·깜빡임을 담은 표와 이미지(RGBA16F, 상자 필터 밉)를 올린다. 광원 방출을 읽는 모든 곳(M 셰이딩, S 프록셀 산란, R 적중 셰이딩·GI)은 광원 기여에 `LightFunction.hlsli`의 `lightFunction(srv, light, forward, right, dir, footprint, g_time)`(rgb)을 곱한다. 무효면 `LIGHT_FUNCTION_NONE`을 넘기고 1을 받는다. footprint는 광원에서 본 수신면 발자국 각(라디안)이고, 0이면 가장 선명한 밉을 쓴다. 그림자(VSM)는 f와 무관하다. 표는 바뀔 때만 새 버퍼로 올리고, 이미지는 바뀐 광원만 다시 올린다.
  - **IES 가져오기 `lights::parseIes`**: LM-63, TILT=NONE, C형. 배수 × 안정기 계수를 곱하고 최댓값으로 나누며(광원 세기 = 최대 칸델라), 대칭 0, 0..90, 0..180, 0..360, 90..270을 받는다. [실측] `unx_test_lights_lightfunctiontests`: GPU와 CPU 기준의 차이는 4096 질의에서 IES 4.8e-7, 시간 함수 2.4e-7, 쿠키·고보 7.7e-4(반정밀 텍셀과 필터 분수)였고 불연속 질의는 0이었다. 1텍셀 줄무늬를 8텍셀 발자국으로 보면 평균 0.5가 나왔다.
- v1.61 (2026-09-26, E B11 사진·시네마틱 모드):
  - **GPU 기준 추적기 사진 모드 API(`unx/reference/GpuPathTracer.h`; Reference/GpuTracer/README_KO.md 6)**: 호스트(I·core)는 게임을 멈춘 스냅숏 `scene::Scene`과 카메라로 `GpuPathTracer(scene, "", what)`를 만든다. 저장소 루트를 비우면 측정 잠금 조각이 없다. 흐름은 `setMotion(ShutterMotion)`(선택) → `start(camera, settings)` → 프레임마다 `pass(n)` → `current()`다.
    - `current()`의 영상은 복사휘도 × 노출(HDR 선형)이다. 수렴 표시는 `halvesRelMse / 4`다.
    - 렌즈는 `ResolvedCamera::lensAperture / lensFocus`로, 실시간 `FrameContext::lensAperture / lensFocus`와 같은 정의다.
    - 셔터 `ShutterMotion`은 close 시각의 인스턴스 변환, 골격 jointToModel, 카메라를 받는다. 보간은 tick 규칙과 같다.
    - 저장: `metrics::writeExr`(선형 HDR). PNG는 M의 후처리 사슬(6절 곡선)을 거친다.
  - **추적기 안전장치**:
    - 디스패치 slow start: 경로 수 상한이 4096부터 시작해, 25 ms 목표의 절반 아래일 때만 두 배로 는다. 보정 전에는 배치당 디스패치 하나다.
    - GpuLock.ps1 래퍼 안에서 실행하면 부모의 잠금을 물려받는다(교착 없음).
    - 광선 원점 오프셋에 삼각형 크기 항을 더한다.
- v1.60 (2026-09-26, 렌더 C, W 요청 `20260926_W_gpu_triangle_stream.md` — W 동의):
  - **GPU 삼각형 스트림**: `render::TriangleStream`{vertices(raw, 32 B: 월드 (xyz,1), (법선,0), 색인 없음, CCW), drawArgs(D3D12_DRAW_ARGUMENTS, GPU가 씀), velocities(선택, float4 월드 m/s: M의 움직임 prev = pos − v·Δt), material, instance, maxTriangles(≤ 2^24, V dispatch 상한), boundsMin/Max}, `FrameResources::triangleStreams`(≤ `kMaxTriangleStreams` = 64; W가 V 앞 `tracks::waterGeometry`에서 채움 — 자리는 A). V의 `StreamRaster.ms`가 용량의 32개 묶음마다 메시 그룹 하나(살아 있는 수 = 인자 정점 수 / 3)로 삼각형을 대역 B 픽셀 커널에 보내 **투과 기록**(COV_DEPTH_SEE_THROUGH)을 만든다: 정확 면적·마스크·중심 깊이·보간 법선, 양면. 대역 A(수중)는 그대로다. vis id: 최상위 두 비트 11 = 스트림, 비트 24–29 슬롯, 0–23 삼각형(`coverageFragmentIsStream/StreamSlot/StreamTriangle`); 머리카락은 10(`coverageFragmentIsHair`가 두 비트로 판정). coverage_layer가 켜져 있어야 그려진다.
- v1.59 (2026-09-26, 렌더 C, B10 머리카락 V 몫; 요청 `20260926_E_hair_strands.md`):
  - **coverage 층의 가닥 머리카락**: V의 `HairRaster.ms`가 `FrameResources::hairSegments`(E)의 세그먼트마다 카메라를 향한 띠(폭 2r, 끝 캡 없음)를 삼각형 둘로 대역 B 픽셀 커널에 보낸다(픽셀 ∩ 띠의 정확한 면적, 32 부표본 마스크, 덮인 영역 중심의 깊이). 기록: vis id = `COV_HAIR_ID`(0x80000000) | 세그먼트 번호, 법선 비트 = u(뿌리 0 → 끝 1, unorm16). 판독 도우미 `coverageFragmentIsHair/HairSegment/HairU`(CoverageTiles.hlsli). M은 접선을 p1 − p0에서 다시 얻는다. 품질 키 `visibility.coverage_hair`(기본 false: M이 머리카락 기록을 읽기 전까지 기록을 내지 않음; coverage_layer와 함께). 조건: 굽는 곳의 바깥 쐐기 틈·안쪽 겹침 ≈ w² × 각도 / 2, 카메라를 향하는 세그먼트는 빠짐. 대역 C 가닥 브릭은 아직 없다(E의 LOD가 픽셀당 2가닥을 넘지 않게 함).
- v1.58 (2026-09-26, 렌더 C, A3 메시 입자 경계 — A와 합의):
  - **GPU가 쓰는 인스턴스 범위**: `RuntimeCapacity::gpuInstances`(품질 키 `fx.mesh_instances_max`, 기본 65,536 [예상]), `GpuScene::gpuInstanceRange()` = {first, capacity, instanceUav(인스턴스 버퍼 raw), countUav, countByteOffset, instanceBuffer, countBuffer}. 인스턴스 id는 올린 인스턴스와 C2b 런타임 인스턴스 뒤에 이어진다(장면 편집으로 올린 수가 바뀌면 옮겨지므로 매 프레임 조회). 쓰는 쪽(FX)은 V 컬링 전 패스에서 gpu::Instance 기록(prev 변환 포함, SKINNED·WIND 금지, bonePalette·morph·patch = kNone)과 살아 있는 개수(uint, `patchData`의 `kGpuInstanceCountElement` 원소 .x)를 쓴다. 개수는 매 프레임 장면 갱신이 0으로 되돌린다. V는 CullInstances SOURCE0을 용량만큼 띄우고 개수 밖 스레드는 아무것도 하지 않는다(`CullView.gpuFirst/gpuCapacity`, 368 B). 나머지는 동적 인스턴스와 같다(두 단계 HiZ, LOD, S 그림자는 CAST_SHADOW 비트). R TLAS는 CPU 목록이라 포함하지 않는다.
- v1.57 (2026-09-26, 렌더 C, C5):
  - **지형 변형 패치(FEATURES_GAME 9, PHYSICS 9.7)**: `gpu::Instance::patch`(옛 morphPad[0]; kNone = 없음), `FrameConstants::patchData`(v1.56의 끝 블록; `patchData` = StructuredBuffer<uint4>, 슬롯 `kPatchSlots` = 64개 × `kPatchSlotElements` = 35: 블록 격자 원점·크기, 교체 블록 사각형, 한 변 블록 수, 4096비트 마스크), SceneUpdate 흩뿌리기 타깃 15(P[4].y). `GpuScene::setPatchRegion(instance, PatchRegion)`(빈 목록 = 해제, 슬롯은 framesInFlight + 1 뒤 재사용). V는 패치 인스턴스의 교체 사각형에 닿는 노드를 끝까지 순회하고 그 사각형에 자기 LOD 구가 닿는 단순화 클러스터를 그리지 않는다(잎 노드와 같은 구·같은 판정이라 컷이 일관, 부모가 자식을 감싸 단조). 원본 삼각형 중 무게중심이 교체 블록에 있는 것은 VisRaster·DepthRaster·CoverageRaster 메시 셰이더가 뺀다(SV_CullPrimitive). 패치 메시는 `scene::buildTerrainPatch`(Native/Scene/TerrainPatch.h)가 C2b 런타임 메시로 만든다.
  - **호스트 ABI**: `UnxTerrainDeformation`, `UnxFrameSetTerrainDeformation`(창 전체를 tick마다; 바뀐 4 × 4 셀 블록만 다시 만든다, 호출 스레드; 목록에서 빠진 타일은 패치를 잃는다). 조건: 텍셀이 지형 격자 위(셀 크기/spacing 정수), 창 가장자리 D = 0, 타일은 평행 이동 배치. 광선(R)은 D 없는 타일을 본다(설계 조건 ≤ 10 cm).
- v1.56 (2026-09-26, 렌더 C, C4 — 처음 v1.46으로 잡았으나 그 번호는 A4가 썼다; 요청 `20260926_C_morph_rebase_host_abi.md`):
  - **블렌드 셰이프·정점 애니메이션(6.4 변형)**: `scene::Mesh::blendShapes`/`vertexAnimation`, `scene::Instance::blendWeights`/`vertexAnimationTime`(Native/Scene, 기준 평가 `scene::evaluateMorph`, 컬링 한계 `scene::morphBound`). `gpu::Instance` 144 → **160 B**(`morph` 기록 행, `morphRadius`, pad 2; v1.45의 `breakCentre` 유지). `FrameConstants` 끝 16 B 블록(544 → **560 B**, spare0은 v1.50의 debugDraw)에 `morphRecords`(float4 행: {메시 블록, 가중치 행, 시간, 이전 시간} + 가중치 현재·이전), `morphData`(uint 단어: 메시 블록). `Deformation.hlsli morphVertex`가 스킨 전에 적용(CPU 기준과 같은 순서의 덧셈, `precise`). `GpuScene::setMorph(frame, instance, weights, time)`: 이전 = 직전 렌더 프레임, deformRevision += 1, 정착 규칙은 팔레트와 같음, SceneUpdate 타깃 3. V는 worldSphere에 `morphRadius`를 더하고 모프 인스턴스의 법선 원뿔·시트 판정을 끈다. 모프 메시는 클러스터 단순화 없이 원본 클러스터만(바인드 자세 오차가 모프 뒤에 성립하지 않으므로). 모프 인스턴스는 업로드 때만 추가된다(setInstances 거부). R은 모프 인스턴스를 변형 BLAS(refit) 대상에 넣어야 한다(B 할 일).
- v1.53 (2026-09-26, A15·A7 호스트 경로, 엔진 2 요청):
  - **디버그 드로우 호스트 입력(E `debug::DrawList`)**: 선택 export `UnxDebugPrimitives(r, lines, n, triangles, m)`(DebugDraw.h 기록 그대로 Line 32 B·Triangle 48 B), `UnxDebugText(r, anchor3, utf8, rgba, sizePx, flags, offsetX, offsetY)`(E의 글리프 배치). 다음 큐 프레임에만 들어간다(즉시 모드: 렌더되지 않는 프레임의 원시형은 버린다).
  - **투영 데칼 호스트 입력(E `decal::DecalSet`)**: `UnxDecalDesc`(80 B: 상자 3×4 행 우선, 재질, 인스턴스 또는 0xFFFFFFFF, 우선순위, 불투명도, 페이드 시작·끝 도, 가장자리, 0), `UnxDecalAdd(r, desc, &id)` / `UnxDecalUpdate(r, id, desc)` / `UnxDecalRemove(r, id)`. 호스트가 거울 집합을 가지며, 바뀌면 다음 큐 프레임과 함께 스냅숏을 보낸다(늦은 스냅숏이 이긴다). 재질과 인스턴스는 호스트의 현재 개수로 검사한다. [실측] `HostDecal` 4·5: 제거와 갱신이 G-buffer에 정확히 반영됐고, 디버그 통계는 선 3·글리프 2였다.
- v1.52 (2026-09-26, A7 호스트 경로, 엔진 2 요청):
  - **표면 상태 장 호스트 입력(E `surface::SurfaceField`)**: 선택 export `UnxSurfaceDelta(r, changed, changedCount, removedKeys, removedCount)`(NativeVfx `nv_surface_delta`의 `NV_SurfaceBrickV2` 1,560 B 기록과 제거 키 int32×3), `UnxSurfaceSetHalfLives(r, double[6])`(젖음·그을음·서리·먼지·핏자국·눈, s, 0 = 감쇠 없음), `UnxSurfaceSetTime(r, seconds)`(이후 큐 프레임의 VFX 시각). `HostRenderer::surfaceDelta/setSurfaceHalfLives/setSurfaceTime`이 다음 큐 프레임과 함께 보내고, 렌더 스레드가 기록 전에 호출 순서대로 `apply`한다. 건너뛴 티켓과 버려진 패킷의 묶음은 다음 렌더 프레임으로 옮긴다. [실측] `HostSurface`: 추가·제거·교체 순서, 건너뛴 두 프레임 뒤의 마지막 값, 버려진 패킷의 묶음, 반감기·시각이 모두 맞았다(디버그 층 오류 0).
- v1.51 (2026-09-26, A5·A3):
  - **물리 카메라 렌즈(5.5, A5, COVERAGE 14.12 (2c))**: `FrameContext::lensAperture`(조리개 지름 m, 0 = 핀홀: 피사계 심도 없음, 게이트 카메라)와 `lensFocus`(초점 거리 m, 시선 축). 깊이 z의 착란원은 f_px A |1/z − 1/z_focus| 픽셀이다(f_px = (H/2) proj[1][1]). 호스트: `HostRenderer::setLens`, 선택 export `UnxFrameSetLens(r, apertureMetres, focusMetres)`(ABI 6 안). 이후 큐에 들어가는 프레임이 그 렌즈를 쓴다. 조리개 적분(DOF) 커널은 A5가 넣는다. 그 전까지 값은 전달만 된다.
  - **`FrameContext::timing` 채움(I, v1.50)**: 호스트가 `GpuProfiler::lastCompleted()`를 넣는다.
  - **VFX 실행기 version 2(`NV_STREAM_EXECUTOR_HEIGHTFIELDS`, 엔진 1 P12 높이 타일, 엔진 2 요청)**: FX가 높이장 절(`NV_StreamHeightField` 80 B, `NV_StreamHeightTile` 1,232 B)을 표면 표와 함께 받아 보관한다. 틱마다 정적 몸체 프레임으로 앵커 공간 프레임을 만든다(CPU 참조 `load_inputs`와 같은 식, double에서 float로). `Particles.hlsli`가 `NV_HEIGHTFIELD*` 훅을 채운다. `UnxVfxStreamExecutor`는 version 2를 등록한다.
- v1.50 (2026-09-26, E A15; v1.47–1.49 are other tracks' open changes):
  - **디버그 드로우(FEATURES_GAME 7.1, E `Passes/Debug`)**: `FrameConstants::debugDraw`(옛 `spare0`, HLSL `g_debugDraw`) = 이번 프레임 디버그 원시형 버퍼의 UAV, 0xFFFFFFFF = 꺼짐(`GpuScene::fill`이 꺼짐으로 두고 `FrameRenderer`가 `tracks::debugBegin`의 값을 넣는다). 어느 커널이든 `Passes/Debug/DebugDraw.hlsli`의 `debugLine/debugPoint/debugTriangle/debugBox/debugNumber`로 덧붙인다(그래픽 큐 패스는 선언 없이 그려진다: 그리기 패스가 전역 배리어로 시작; 비동기 계산 큐 패스는 `debug::buffer(fc)`를 UAV로 선언). `Tracks.h`: `debugBegin`(프레임 처음, 프레임 상수보다 먼저), `debugOverlay`(프레임 끝: `debug.view` 버퍼 시각화 → 원시형 → HUD를 view.color 위에).
  - **`MeshPipelineDesc::premultipliedBlend`(core)**: 렌더 타깃 0에 미리 곱한 알파 혼합(src + dst (1 − src.a)).
  - **`FrameContext::timing`(I가 채움)**: 직전 완료 프레임의 `GpuProfiler::lastCompleted()`, 품질 키 `debug.hud`가 패스별 ms를 그린다. null이면 HUD가 없다고 적는다.
- v1.49 (2026-09-26, 렌더 C, C2b):
  - **런타임 지오메트리 풀(6.3)**: `render::RuntimeCapacity`(메시·서브메시·정점·인덱스·클러스터·클러스터 정점 인덱스·삼각형·노드·인스턴스 수), `GpuScene::reserveRuntime`(upload 전), `addRuntimeMesh(mesh, clusters)`(한 메시 장면의 clusterbuilder 결과, 깊이 ≤ `kRuntimeMaxDepth` = 6, 단순화 없음), `removeRuntimeMesh`, `addRuntimeInstance`, `removeRuntimeInstance`, `staticInstanceCount`, `staticMeshCount`. 장면 버퍼마다 올린 내용 뒤에 꼬리 영역이 있고 범위 할당기로 나눈다. 쓰기는 SceneUpdate 흩뿌리기 타깃 4–14(16 B 원소). 해제는 framesInFlight + 1 flush 뒤 재사용. 장면 revision은 바뀌지 않는다(트랙 재구축 없음). V는 `CullView.runtimeFirst/runtimeCount`로 런타임 인스턴스를 평면 목록에서 컬링하고, 풀이 있으면 노드 단계를 최소 `kRuntimeMaxDepth`번 돈다. 런타임 인스턴스는 강체만(스킨·모프 거부). 광선 추적(R)은 런타임 지오메트리를 아직 보지 않는다(이벤트 BLAS, FEATURES_GAME 2.1).
  - **호스트 ABI**: `UnxRuntimeCapacity`, `UnxSceneReserveRuntime`(commit 전), `UnxFrameAddRuntimeMesh(desc, &id)`(id 비트 31 = 런타임 메시; 호출 스레드에서 클러스터 생성), `UnxFrameRemoveRuntimeMesh`(쓰는 인스턴스가 남아 있으면 거부), `UnxFrameAddRuntimeInstance(mesh, objectToWorld, flags, &id)`(mesh = 커밋된 메시 번호 또는 런타임 메시 id), `UnxFrameRemoveRuntimeInstance`, `UnxFrameSetRuntimeTransforms(count, ids, transforms)`. 다음 큐 프레임에 호출 순서대로 적용되고, 버려진 프레임의 연산은 다음 프레임으로 넘어가며 원점 이동을 따른다. 런타임 내용은 저장하지 않는다(게임 상태에서 다시 만든다). 풀이 차면 로그를 남기고 그 요소를 그리지 않는다.
- v1.48 (2026-09-26, 렌더 C, C3):
  - **인스턴스 공간 계층·스킨 컬링(V 내부, ARCHITECTURE 2.1)**: 정적 인스턴스(동적·스킨 아님)를 64 m 셀 × ≤ 256개 청크로 묶어 청크를 먼저 컬링(CullChunks, ChunkBounds: worldSphere + 바람 항 + 모프 반경), 동적·스킨은 평면 목록. 스킨 인스턴스는 관절 경계구의 팔레트 상(SkinBounds)으로 인스턴스 단계 컬링(노드·클러스터 단계는 그대로 보수적). 정적 인스턴스는 움직이지 않는다는 계약(R 정적 TLAS·S 페이지 캐시와 같음)에 기댄다. `visibility::Stats::chunkItems`, `deferredChunks`.
- v1.47 (2026-09-26, 렌더 C, C9; 요청 `20260926_C_origin_rebase.md`):
  - **원점 재설정**: `FrameContext::originShift`(축마다 1024 m 정수배, `kOriginGrid`), `GpuScene::rebase(shift)`(CPU 거울·GPU 표를 SceneShift 커널로 제자리 이동, 광원 재업로드, 움직임 없음), FrameRenderer가 메인 뷰의 이전 viewProj를 새 좌표로 옮김. V는 이전 카메라 위치와 청크 경계를 옮긴다. S(클립맵 페이지 정수 이동)·R(GI 셀 재색인, TLAS)·호스트 ABI(`UnxFrameSetOriginShift`)는 요청 문서대로 각 트랙.
- v1.46 (2026-09-26):
  - **HDR 디스플레이 출력(5.5, A4)**: `FrameContext::displayPeak`는 0이면 SDR이고(주 뷰 색은 R10G10B10A2, 톤 커브 뒤 sRGB), 1 이상이면 디스플레이 최고 휘도 ÷ 종이 흰색이다. 이때 주 뷰 색은 R16G16B16A16 FLOAT이고, 봉우리에 맞춰 일반화한 커브(M `shPbrNeutralPeak`, 1에서 SDR 커브와 같다)를 거친 디스플레이 기준 선형 Rec.709 빛(1 = 종이 흰색)을 담는다. 인코딩(scRGB, PQ)은 호스트가 한다. 호스트 ABI는 `UnxFrameDesc::displayPeak`로, 옛 `reserved` 자리(0)라 옛 호스트는 SDR을 받는다. 선택 export `UnxRendererQualityOverride`(commit 전 품질 덮어쓰기)도 이때 더했다.
- v1.45 (2026-09-26):
  - **움직임 끊김 플래그(6.3, A0 복원 검사)**: `gpu::kInstanceMotionBreak`와 `Instance::breakCentre`. S의 `VsmMoved`는 끊긴 인스턴스의 모든 단계를 무효화하고(기준점은 마지막으로 그린 자리에 남는다), `VsmLocalInvalidate`는 옛 구를 `breakCentre`에서 잡는다.
  - **안정 면광원 마스크(B2, COVERAGE 12.4 구조 2)**: `FrameResources::areaLightStable`(M `prepareScene`, raw SRV, 광원당 1비트 = revision이 8프레임 이상 그대로). `raytracing.emitters`가 켜졌을 때만 게시하고, 꺼지면 UINT32_MAX다. M은 비트가 선 면광원의 LTC 스펙큘러를 R의 반사 경로(K/G/M)에 넘기고, R은 비트가 없는 광원을 직접 발광체 hit에서 뺀다. `GpuScene::lights()`(광원 레코드 CPU 사본).
- v1.44 (2026-09-26):
  - **commit 뒤 장면 편집(6.3, I 요청, D0)**: `GpuScene::setInstances`, `setMaterials`.
  - **Unity 콘텐츠 매핑(8.5, D0, core 결정)**: 좌표·UV·재질(URP Lit, Standard)·광원·메시 규칙. 8.1로 정확히 옮길 수 없는 입력은 콘텐츠 보고로 남긴다.
- v1.43 (2026-09-26):
  - **`FrameResources::vsmAtlas`(S 요청 `20260926_S_vsm_one_path.md`)**: VSM 한 경로의 페이지 아틀라스(D32_FLOAT, SRV R32_FLOAT, 페이지 p는 ((p % 128), (p / 128)) × 128 px, 0 = 캐스터 없음). `ShadowSrvs.pool`이 그 SRV를 담는다. 전환 순서: 필드를 먼저 더했다(`vsmPool`과 함께). M·R·FX가 `vsmAtlas`를 읽도록 옮긴 뒤 S가 아틀라스를 게시하고 `vsmPool`을 무효로 둔다. 그다음 코어가 `vsmPool`을 지운다. 버퍼 서술자를 텍스처로 읽는 창은 생기지 않는다.
- v1.42 (2026-09-26):
  - **`GpuLockSlice`(3.3, C 요청)**: 프로세스 안 조각 잠금 C++ API. GpuLock.ps1과 같은 규약이다.
  - **공용 헤더 분리(5절, 인프라 요청 `20260926_Infra_header_split.md`)**: 내용 변경 없이 위치만 옮겼다. `Frame.h`는 호환 include를 유지한다. 코어 파일 가운데 `src/GpuLock.cpp`는 `TrackPending.h`만 include한다. 인프라가 적용 뒤 증분 빌드 시간을 다시 잰다.
- v1.41 (2026-09-26):
  - **coverage 층 타일 구간(7.1, V; 설계 개정 채택, 1절 최악 dispatch 조건)**: 스트림 append → 카운트 → 스캔 → 오프셋 → 흩뿌리기로 목록 타일마다 픽셀 순서 연속 구간을 만든다. M 요청대로 픽셀 순서로 두었고, 목록 정보 `{ tile, records, record base, block base }`는 M의 A 단계와 같은 형식이다. 블록 패스와 여러 블록 타일 마무리 패스가 opaqueCovered와 S의 `coverageDepthRange`를 만든다. 청크 표·확장 트리·CAS는 없다.
    - `ViewResources`: `coverageRecords`, `coverageTilePixels`, `coverageDepthRange` 추가. `coverageChunkTable`(늘 무효)과 `coverageChunks`(= records)는 M 전환 뒤 지운다.
    - V 통계: `coverageBlocks`, `coverageHeavyTiles`(여러 블록 타일), `coveragePoolRecords`, `coverageMeasured`(측정 단계의 fragment). `coverageChunks`, `coverageChunksLost`, `coveragePoolChunks`는 없앴다. 측정 단계 2는 이제 "append 원자까지"다.
    - 용량 판독을 프레임의 V 패스 앞으로 당겼다(필요량이 늘면 한 프레임 일찍 커진다).
    - [실측] 첫 하드웨어 실행(잠금 안 correctness, `UNX_FENCE_TIMEOUT_S=10`): visibility 8/8, D3D12 디버그 층 오류 0. `coverage_layer_is_exact` 두 설정 × 4프레임:
      - 기본: 프레임당 65,998~80,513 레코드가 전부 정확 클리핑과 일치했다(면적 최대 오차 5.3e-4 px², 법선 0.78° 이하). 가장 깊은 타일은 51,641~65,916 레코드(블록 51~65개)로 여러 블록 경로를 지났다. 목록·기저·픽셀 구간·깊이 구간이 레코드와 맞았고, 지난 프레임 타일과 깊이 구간은 모두 비워졌다.
      - 용량 하한 0.05/px(65,536 레코드): 프레임 0~2가 가득 찼고(`Stats::overflow` 0x100), 저장된 65,536 레코드는 모두 일관됐다. 프레임 3은 131,072로 커져 빠짐이 없었다.
  - **S·FX 필드(7.3, 표)**: `shadowFragmentVisibility`, `shadowFragmentSun`(S), `particleLayer`, `particleDepthRange`, `particleEdges`, `distortionLayer`(FX). FX 진입점 `tracks::particles`와 호출 위치는 FX 모듈의 정의가 들어온 뒤 넣는다.
  - **GpuLock(3.3)**: 하드웨어 어댑터 LUID만, 우리 프로세스만 경합(그 밖은 `background:`), `cpu-contended`, 대기자 파일·correctness 양보·HOLD, 짧은 끝 구간은 판정 제외, 프로세스 안 조각 규약.
- v1.40 (2026-09-26):
  - **대역을 삼각형 단위로 판정(설계 개정 14.9 교정 3, 요청 16)**: 판 모양(sheet) 클러스터 중 최소 폭이 히스테리시스 폭(`visibility.band_a_hysteresis_px`, 2.0 px) 아래인 것(대역 C·스킨·`BAND_MODE_A` 제외)은 "혼합"이다. 혼합 클러스터는 대역 A 목록과 coverage 목록(LIST_B) 둘 다에 들어가고, 목록 항목 bit 31(`LIST_ENTRY_MIXED`)이 선다. 두 메시 커널이 같은 변형 정점으로 `sheetTriangleBandB`(최소 높이 × lodScale / 무게중심 거리 × |cos|)를 계산해 삼각형 하나를 정확히 한 쪽에서만 그린다: VisRaster는 B 삼각형을, CoverageRaster는 A 삼각형을 `SV_CullPrimitive`로 버린다. 히스테리시스 (a): 지금 폭이 A 최소와 히스테리시스 폭 사이이면 이전 카메라 위치(`CullView::prevPosition`, 컷 뒤에는 현재 위치)에서 B였을 때 B로 둔다. 판이 아닌 클러스터는 클러스터 규칙에 거리 비례 히스테리시스를 더한다. `CullView` 352 B.
    - 통계: `visibility::Stats::mixedClusters`, `mixedTriangles`(상태 워드 40, 41; `VS_WORDS` 48).
  - **coverage 래스터 측정 단계 3, 4**: `visibility.coverage_debug_stage` 3 = 픽셀 커널이 바로 반환(메시 + 래스터 + 호출), 4 = 면적·깊이·대역 A 판정 뒤 반환. 측정 변형은 커널 호출 수를 센다(`Stats::coverageInvocations`, 상태 워드 31).
  - **TDR 재현 금지 규칙(3.6, 조율 결정)**과 10.1 동적 강체 메모(C 195fda3).
  - **RPP 트랙 등록(요청 `20260926_RPP_track_registration.md`)**: 1절 표의 RPP 행, `cmake/Tracks.cmake`의 `RPP`와 `RppBuild → RPP`, `Build.ps1 -Track RPP` → `build/RPP`, 트랙 `C;RPP`(2.5). `Content/`는 빌드 대상이 아니다(데이터). 큰 생성물은 `Cache/RPP1/`(무시 목록).
  - V 게이트: `--scene deep_tile --deep-tile-cards N`(한 타일에 카드 N장을 겹친 합성 장면; 성장 곡선용), 거리대(62·187·374 m)별 픽셀·레코드·나무 레코드, 픽셀당 레코드 P50/P99/최대.
  - [실측] 삼각형 단위 판정의 첫 하드웨어 실행(잠금 안, `UNX_FENCE_TIMEOUT_S=10`): visibility 8/8, D3D12 디버그 층 오류 0.
  - [실측, deep_tile 4K, 한 타일에 fragment 10 k / 30 k / 100 k / 300 k, 단계 0] 타일 패스(타일당 그룹 1개) 0.057 / 0.188 / 0.670 / 2.052 ms, 래스터 2.98 / 2.29 / 2.00 / 1.93 ns/f, CAS에 진 청크 81 %. 선형이지만 한 그룹이 타일 전체를 걷는 형태라 3.6의 dispatch 상한 규칙에 맞지 않는다 → v1.41에서 배치를 바꾼다.
- v1.39 (2026-09-26):
  - **coverage 확장 트리(7.1)와 셰이더 루프 상한 규칙(3.6)**: forest_combat edge V 게이트의 FENCE_TIMEOUT(조율 신고)이 원인이다. 확장 사슬 걷기가 깊은 타일에서 제곱 비용이 됐다. 트리로 바꿔 조회를 노드 4개 이하로 했고, M 합성의 `coverageChunkOf` 호출은 그대로 맞다(4변형 컴파일 확인). V의 웨이브 루프·타일 패스 루프에 상한 + 0x400을 넣었다. 새 overflow 비트: 0x200 트리 초과, 0x400 반복 상한.
    - [실측] `coverage_layer_is_exact`(두 설정 × 3프레임, 바뀐 커널의 첫 하드웨어 실행은 잠금 안에서 `UNX_FENCE_TIMEOUT_S=10`)가 통과했다. 트리 노드는 기본 설정에서 5개, 칸 1 설정에서 69~81개였고, 카드 더미 타일은 2단 하위 트리까지 갔다. unit 34/34, visibility 8/8, D3D12 디버그 층 오류 0. 래스터 단가 분해용 측정 키 `visibility.coverage_debug_stage`(1 = fragment 계산까지, 2 = + 타일 카운터, 0 = 층): waterside 4K 12.0 M fragment, A B C 두 번 교대. 1 = 7.63~7.69 ms(0.64 ns/f), 2 = 9.39~9.66(+0.16 ns/f), 0 = 11.95~12.03(+0.20 ns/f). 경합(카운터·청크)은 1/3이고, 가장 큰 몫은 픽셀 커널 계산과 메시·래스터 앞단이다(설계 목표 ≤ 0.3 ns/f).
  - **GpuLock 경합 검출(3.3, 조율 요청)**, **하네스 JSON `gpu_contention`·`queues`, 프로파일러 목록 마크(11, I 요청)**. 설계 개정은 2.13에 "호스트 통합 틈" 행을 더했다(목표 ≤ 0.10 ms; I Player 4K 실측 0.27~0.44, 1024+256 부하에서 0.59 ms로 부하와 함께 커진다). 이 마크로 tail(끝 배리어)인지 목록 사이 틈인지 가른다.
  - [실측] unit 34/34, visibility 8/8, D3D12 디버그 층 오류 0.
- v1.38 (2026-09-26):
  - **opaqueCovered를 합집합 규칙으로(7.1, 설계 개정 판정: COVERAGE_REDESIGN 4.6)**: v1.37의 "fragment 하나가 픽셀 전체" 규칙은 얇은 기하에서 거의 서지 않았다(관찰 2). 이제는 래스터 뒤 타일 패스가 레코드에서 픽셀별 불투명 마스크 합집합과 가장 먼 불투명 깊이를 만들어 비트를 세운다. `coverageBDepth`는 없앴다(래스터의 뒤 fragment 컷도 없다). 레코드 depth 워드의 부호 비트가 투과 재질 표시다.
    - 픽셀별 원자 판(기여자 D min → 펜스 → U or, 64 bit CAS 판은 선택 기능이라 제외)을 먼저 구현하고 정확성까지 확인했다. 그러나 설계 개정 DesignBench에서 fragment당 +0.11~+0.17 ns(펜스 없이)로 재어져 버렸다.
  - **레코드 풀을 `StructuredBuffer<uint4>`로 바꿨다**: raw 뷰의 원소 상한 2^27 워드(33.5 M fragment)는 forest_thin(브릭 전 25.2 M)에 너무 가까웠다. 16 B 원소 뷰는 134 M까지 된다. 확장 표 원자는 원소 성분 원자로 한다.
  - [실측] `coverage_layer_is_exact`(두 설정 × 3프레임, 바뀐 커널의 첫 하드웨어 실행은 잠금 안): opaqueCovered는 모든 픽셀에서 "불투명 레코드 마스크 합집합이 가득 & 대역 A 깊이 < 그중 가장 먼 깊이"와 같았다. 불투명 합집합이 가득 찬 픽셀은 프레임당 38~60개였다. v1.37 규칙에서는 5~12개였으니 5~7배다(1.3 px 기둥 6개, 사각형 하나 = 삼각형 2개). 유리 카드 더미의 레코드 51,641~56,000개는 모두 투과 표시가 있었고, 다른 레코드에는 없었다. 누락 0, 남은 타일 0. visibility 8/8, unit 34/34, D3D12 디버그 층 오류 0.
  - V 게이트: `--camera NAME|INDEX`(forest_combat의 eye·up·edge). coverage 출력에 타일 패스 시간(`v.coverage.tiles`)을 따로 내고, 한 프레임 되읽기에서 "채워진 불투명 합집합 뒤의 fragment 비율"(합성에서 가중치가 0인 몫)과 합집합이 가득 찬 픽셀 수를 낸다.
- v1.37 (2026-09-26):
  - **coverage 층 v2(7.1, V 요청 `20260926_V_coverage_layer_v2.md`, 설계 채택 d90f9db)**: 8×8 타일 청크, 16 B 레코드(보간 법선 포함), 타일 머리(수·깊이 범위·opaqueCovered·확장 표), bDepth, 타일 목록과 무거운 타일 목록. 정렬 없음(M이 groupshared에서 정렬). `ViewResources`의 coverage 필드 5개가 v1.25의 3개를 대신한다.
    - 구현: 픽셀 커널이 웨이브의 fragment를 타일별로 묶어 타일마다 원자 1회로 번호를 받는다. 청크는 풀에서 받아 CAS로 표에 넣고, 진 쪽은 이긴 쪽 청크를 쓴다(기다림 없음). 확장 표는 0으로 채운 뒤 메모리 펜스를 두고 게시하며, 표 읽기는 원자 연산이다. 지난 프레임의 타일만 비운다(화면 전체 지우기 없음). 무거운 타일 분류는 목록 위 패스 하나다.
    - 품질 키: `coverage_table_slots`(32), `coverage_pool_min_fragments_per_pixel`(1.0), `coverage_heavy_tile_fragments`(1,024). `max_coverage_fragments`는 읽지 않는다(이전 바이너리용으로 설정에만 남김).
    - V 통계: `coverageTiles`, `coverageChunks`(필요량, 풀 크기 근거), `coverageChunksLost`(CAS에 진 청크), `coverageHeavyTiles`, `coveragePoolChunks`. `coveragePixels`는 없앴다.
    - `RenderGraph::desc(BufferRef)`(코어): 기록 시점의 버퍼 설명.
    - [실측] `coverage_layer_is_exact`(640×360, 두 설정 × 3프레임, RTX 4080, 새 커널 첫 실행은 GPU 잠금 안): 프레임당 65,998~70,538 레코드 전부가 정확 클리핑과 일치했다. 면적 최대 오차 5.25e-4 px²(10 bit 반 걸음 4.9e-4 + float), 깊이 1.25e-5 상대, 법선 최대 0.77°(8+8 bit 팔면체 반올림 상한 0.95°, 40만 방향 실측). 누락 0, 남은 타일 0, 절단 조각 중복 162~195개(같은 값). 카드 4,000장 더미의 타일(51,641~56,000 fragment)은 확장 표 사슬에 모두 담겼다(표 칸 32: 표 4개, 칸 1: 68~80개). 무거운 타일 목록은 문턱 1,024와 64에서 정확했고, bDepth·opaqueCovered는 픽셀 전체를 덮은 불투명 레코드와 일치했다. D3D12 디버그 층 오류 0.
    - 관찰 1(실제 장면 값은 V 게이트에서 잰다): CAS에 진 청크. 더미 타일처럼 웨이브 수천 개가 한 타일에 몰리면 청크 5,073개 중 3,916개가 버려졌다(풀 16,384 안이라 손실은 없다).
    - 관찰 2(설계 개정·M 판정 필요): opaqueCovered·bDepth는 fragment 하나가 픽셀 전체를 덮을 때만 선다. 대각선으로 나뉜 사각형(1.3 px 기둥)은 두 삼각형이 픽셀을 나눠 덮으므로 거의 서지 않았다(프레임당 5~12 픽셀). 설계 4.6의 −0.70 ms가 이 조건에 달려 있다.
- v1.36 (2026-09-25):
  - **결정성(5.5.2, 사용자 결정)**: 결정 모드 스위치(R `gi.deterministic` 등)로 두고, 합계표 여유가 확인되면 늘 켬으로 올린다.
  - **`ViewResources::screenProbeBlocks`(R 요청, 설계 개정 12.3)**: GI 화면 프로브 블록, 프로브당 640 B 연속, 메인 뷰 전용. R이 쓰고 M이 읽는다.
  - **Build.ps1 헤더 의존성 검사(VFX 신고)**: 빌드 뒤 프로젝트 헤더를 include하는데 Ninja가 헤더를 하나도 기록하지 않은 오브젝트가 있으면 실패한다. 다른 코드페이지의 셸에서 `cmake --build`를 돌리면 /showIncludes 접두어가 설정 때 것과 달라 의존성이 기록되지 않고, 그러면 헤더만 바꾼 변경이 다시 빌드되지 않는다. Build.ps1은 설정과 빌드를 한 cmd에서 하므로 일치한다. **빌드는 Build.ps1로만 한다.**
    - `Build.ps1 -Jobs N -LowPriority`: 병렬 수와 BelowNormal 우선순위를 준다(사용자 게임 중의 가벼운 모드). 20:40대의 기계 정지 때는 -j4 BelowNormal 빌드로도 부담이 컸다. 조율이 멈추라고 하면 빌드도 멈춘다.
    - [실측, 모든 build 폴더의 `ninja -t deps`] Build.ps1로 만든 폴더(core, S, M*, R*, FX, I, all, gate)에는 없다. `build/C`의 오브젝트 3개(ReferenceTests, PathTracer, ReferenceTool)에 기록이 없다. 그 셋은 C가 오브젝트를 지우고 Build.ps1로 다시 빌드해야 한다. 그 바이너리로 잰 결과는 헤더 변경 뒤의 것인지 C가 확인한다.
- v1.35 (2026-09-25):
  - **FrameConstants 544 B(5.5)**: `coverageMaskLut`, `giRaysThisFrame`, 예비 2칸.
  - **coverage 마스크 LUT(5.5.1, 설계 개정 11 b)**: `coverageTriangleMaskLut`, `coverageMaskTable()`. 게이트 결과는 5.5.1에 있다.
  - **기준 비교 한도 형식(3.4, C 합의)**.
  - **서비스 컬링의 단 수 척도(5.3)**: V 게이트 `--service-levels`, `--service-spacing`. 아틀라스 행 128 슬롯(16,384 페이지까지).
  - **`visibility::Stats::bandClusters[3]`**: 대역별 가시 클러스터 수(V 게이트 출력). [실측, forest_thin 4K, 브릭 없는 지금 빌드, 순회 용량 초과 상태의 하한] A/B/C = 4,890 / 1,850,897 / 3,926,947. 설계 개정 14.2 ③의 visId 24 bit 상한(131 k)을 넘으므로 ③은 (b) 타일 청크 클러스터 표로 간다(설계 개정 판정). forest_card 4K(용량을 올림): 65,557 / 78,942 / 0.
- v1.33 (2026-09-25):
  - **서비스 컬링의 타일 마스크 존재 판정(5.3, V)**: 인스턴스·노드 단계는 `tileAnySet`(조기 종료). [실측] city 4K 링 부하 12단: 컬링 0.790 → 0.282 ms, 출력 동일.
  - **V 게이트**: `--service`가 쉼표 목록(whole, local, atlas16, atlas32)을 받는다. `--service-pages camera|ring`. 아틀라스 실측은 5.3 표에 있다.
  - **V 테스트**: `raster_service`의 두 래스터 id 비교가 순서 무관이 됐다(`TestDepthPixel.KEY1`: 픽셀마다 depth << 32 | id의 64비트 원자 max). 이전에는 깊이 max 뒤의 비원자적 id 저장이라, 커널 타이밍이 바뀌면 같은 깊이의 id가 4픽셀 달라졌다.
- v1.32 (2026-09-25):
  - **깊이 래스터 서비스 타일 아틀라스(5.3, S 요청 04a8f70)**: `DepthRasterRequest::atlasSlots/atlasTilesPerRow`. 깊이 대상은 D32 또는 D16이다. 컬링은 아틀라스 모드에서 켜진 타일마다 쌍 하나를 내고(`CULL_VIEW_TILE_SINGLE`), 메시 셰이더 변형 `DepthRaster.ms.TILE2`가 타일을 슬롯으로 옮긴다. `RenderGraph::desc(TextureRef)`를 기록 시점에 쓸 수 있다(코어). [실측] `raster_service_tile_atlas` 통과, 시각화 테스트 7/7. 새 커널의 첫 하드웨어 실행은 GPU 잠금 안(correctness)에서 했다.
  - 바람 메타: S가 설계 개정 14.2 ④(한 경로: 매 프레임 전부 다시 그리기, 바람 메타 삭제)를 정하기 전까지 V는 따로 만들지 않는다. 필요하면 `[earlydepthstencil]` 픽셀 커널로 지금 쓸 수 있다.
- v1.31 (2026-09-25):
  - **조명 그룹과 밴드 도구(4절, M·S 요청)**: `tracks::shadowVisibilityPasses/shadingPasses/shadingComposite` 선언(`Tracks.h`)과 코어 스텁, `PassBand::lagged(rows)`, `passBand(height, count, index)`. FrameRenderer 전환은 S·M 구현 뒤에 한다. [실측] 34/34.
  - **`output.band_pixels` 기본 0 = 한 밴드(M 실측 순손실 6.725 → 6.800 ms)**. 틀과 도구는 그대로 두고, 셰이딩이 바닥에 가까워지면 다시 잰다.
- v1.30 (2026-09-25):
  - **GpuLock 보유 상한(3.3, 조율 요청)**: Job 객체로 명령 트리를 묶는다. `-TimeoutMinutes`(기본 45)를 넘으면 트리를 끝내고 `TIMEOUT`(124)으로 기록한다. 래퍼가 죽으면 트리도 같이 끝난다. 명령 뒤에 남은 자손을 정리하고, 죽은 보유자는 `stale release`로 남긴다. `current.json` 쓰기 경쟁을 고쳤다. 잠금 대기 상한은 `-WaitMinutes`로 이름을 바꿨다. [실측] 자체 시험(별도 뮤텍스 이름의 사본): 3초 상한에서 exit 124, 잠자던 자식 0개 남음. 떠난 자손 1개를 이름과 함께 정리. 래퍼 강제 종료 2초 뒤 자식(PING, conhost) 0개. 기다리는 쪽이 있을 때와 없을 때 모두 `stale release` 기록. 대기자 4개가 동시에 `current.json`을 읽어도 모두 exit 0.
  - **CPU 펜스 대기 상한(3.3, 조율 요청)**: `waitFenceCpu`, `fenceTimeoutSeconds`, `kFenceTimeoutExitCode = 88`. [실측] 단위 테스트 `fence_wait_limit_exit_policy`(자식 프로세스, 상한 2초: 2.8초에 exit 88, 마지막 줄 `UNX_FENCE_TIMEOUT Queue::waitCpu(graphics) value 1001 completed 1 after 2 s`), `fence_wait_limit_throw_policy`(대기가 돌아오고 `deviceWasRemoved()` = true). 33/33.
  - **GPU 잠금 규칙(3.3, 사용자 결정)**: 잠금은 성능 측정, 새 커널의 첫 하드웨어 실행, 물리 GPU 실행만이다. 그 밖의 정확성 실행은 다시 잠금 없이 동시에 해도 된다.
  - **Build.ps1 `[CmdletBinding()]`(조율 요청)**: 모르는 매개변수는 오류다. S가 준 `-BuildDir`가 무시되어 기본 `-Track core`로 다른 폴더를 다시 구성했었다. 공유 트리의 `build\core`는 `-Tracks "V;C"`로 다시 구성했다.
- v1.29 (2026-09-25):
  - **밴드 패스 그룹(4절, 설계 개정 1 요청 7절)**: `RenderGraph::addBandedGroup`, `PassContext::band`, `passBandCount`, 품질 키 `output.band_pixels`(코어). M·S가 픽셀 패스를 옮긴다.
  - **Coverage.hlsli 면적·무게중심을 Green 정리 스트리밍으로 바꿨다(V, 요청 11절 9.4, 4354627)**: 시그니처는 그대로다. 면적은 이전과 같은 정확도(최대 5.7e-5 px²)다. 무게중심 깊이는 면적 1e-2 px² 이상 fragment에서 1.3e-5 상대 이내이고, 더 작은 조각은 그 삼각형의 깊이 범위 안이다(순서 오차 ≤ 그 면적 < 1/32). M은 가장자리 합성 테스트를 다시 돌린다.
