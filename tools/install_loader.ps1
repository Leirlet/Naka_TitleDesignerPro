# Copies the loader .prm into MediaCore (needs administrator rights).
# The implementation DLL and the designer go to %ProgramData%\NAKA\TitleDesignerPro (no admin needed).
$src = Join-Path $PSScriptRoot "..\build\bin\Release\NAKAfx_TitleDesignerPro.prm"
$dst = "C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\"
Copy-Item -LiteralPath $src -Destination $dst -Force
Get-Item -LiteralPath (Join-Path $dst "NAKAfx_TitleDesignerPro.prm") | Select-Object FullName, Length, LastWriteTime
