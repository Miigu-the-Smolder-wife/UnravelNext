$ErrorActionPreference = "Continue"
Set-Location "C:\Users\USER\UnravelNext-fix"
& "build\all\bin\unx_test_shading_shadingtests.exe" *> "C:\Users\USER\UnravelNext-fix\Results\Local\Fix-11\unx_test_shading_shadingtests.log"
"shadingtests exit $LASTEXITCODE"
