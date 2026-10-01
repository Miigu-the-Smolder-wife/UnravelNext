$ErrorActionPreference = "Continue"
Set-Location "C:\Users\USER\UnravelNext-gate"
& "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\baseline\bin\unx_test_shading_shadingtests.exe" *> "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\base_unx_test_shading_shadingtests.log"
"baseline shadingtests exit $LASTEXITCODE"
