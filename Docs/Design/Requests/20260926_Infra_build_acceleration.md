# 알림: 빌드 가속으로 코어 파일을 고친다 (인프라 → 코어, 2026-09-26)

사용자 결정(REBUILD_PLAN 14.3 ⑭)으로 인프라 세션이 빌드 가속을 맡는다. `Tools/CI`와 `cmake` 파일은 코어 소유지만, 이번 작업에 한해 코어가 위임했다. 바꾸기 전에 이 파일로 알리고, 커밋 뒤 결과를 이 파일 아래에 적는다.

## 고칠 파일과 내용

| 파일 | 변경 | 기본값 |
|---|---|---|
| `CMakeLists.txt` 또는 `cmake/Toolchain.cmake` | `CMAKE_CXX_SCAN_FOR_MODULES OFF`. 저장소에 C++20 모듈이 없다(`import`/`module` 선언 0개). CMake 3.28은 C++20 대상의 모든 번역 단위를 한 번 더 전처리해 `.ddi`를 만들고 dyndep으로 모은다. `build/all`의 `.ninja_log`에서 `.ddi` 단계가 번역 단위마다 약 3 s였다 [실측, 32병렬 경합 중]. Build.ps1이 트랙 집합이 바뀐 폴더를 지우게 만든 dyndep 단언(3.5)도 이 경로에서 나온다. | 켬(동작 영향 없음) |
| `cmake/Toolchain.cmake` | 옵션 `UNX_PCH`: 대상별 `target_precompile_headers`(표준 라이브러리, `windows.h`, `d3d12.h` 등 자주 쓰는 외부 헤더만. 프로젝트 헤더는 넣지 않는다: 넣으면 그 헤더를 고칠 때 전부 다시 컴파일된다) | 측정 뒤 결정 |
| `cmake/Toolchain.cmake`, `cmake/Modules.cmake` | 옵션 `UNX_UNITY_BUILD`: `CMAKE_UNITY_BUILD`, 합치면 깨지는 파일은 제외 목록(`SKIP_UNITY_BUILD_INCLUSION`) | 측정 뒤 결정 |
| `cmake/Toolchain.cmake` | 디버그 정보 `/Zi` → `/Z7`(선택). `/Zi`는 대상의 모든 번역 단위가 PDB 하나에 mspdbsrv로 쓰고, PCH 재사용과 컴파일러 캐시를 막는다. 링크 시간이 늘 수 있어 실측으로 정한다. 실행 파일 PDB는 그대로 링커가 만든다. | 측정 뒤 결정 |
| `Tools/CI/Build.ps1` | `-Pch`, `-Unity`, `-Z7` 스위치(캐시 변수로 전달), 빌드 폴더 이름 `-BuildDir`(측정용 별도 폴더) | 없음 |
| `Tools/CI/GpuLock.ps1` | 명령이 끝난 뒤 남은 자손이 Unity 계열(Unity, AssetImportWorker, Unity.Licensing.Client, UnityShaderCompiler, Bee 등)이면 5초 뒤 강제 종료하지 않고 스스로 끝날 때까지 기다린다(상한 있음). 2026-09-26 05:09:56 VFX 실행에서 스크립트가 돌아온 뒤 살아 있던 `Unity (67868)`을 끝냈다(`history.log`). 편집기 종료 중 Library 쓰기를 끊으면 Library 손상 위험이 있다. | 켬 |
| `Tools/CI/HeaderCost.ps1` (새) | `ninja -t deps`로 헤더마다 다시 컴파일되는 번역 단위 수와 그 컴파일 시간 합을 낸다. 결과로 쪼갤 헤더 제안을 따로 요청 파일로 낸다(코어 소유 헤더는 코어가 적용). | — |

## 지키는 것

- 결과 바이너리 동작이 같아야 한다: 켜기 전후로 `unx_unit_tests`와 트랙 정확성 테스트를 같은 결과로 확인하고, 셰이더(DXIL) 바이트가 같은지 본다.
- 기본값을 바꾸는 것은 실측으로 이득이 확인되고 결과가 같을 때만이다. 이 파일에 전후 수치를 [실측]으로 적는다.
- 측정 빌드는 별도 폴더(`build/infra-*`)에서 BelowNormal, 병렬 수를 고정해 교대로(A/B/A/B) 한다. 다른 세션의 빌드 폴더는 건드리지 않는다.

## 결과

(커밋 뒤 적는다)
