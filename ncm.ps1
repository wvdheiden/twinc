# twinc shell integration for PowerShell: start twinc with `ncm`; after F10 the
# shell is in the directory you were in. Add to your profile (notepad $PROFILE):
#   . "C:\path\to\ncm.ps1"
function ncm {
  $f = [System.IO.Path]::GetTempFileName()
  try {
    & twinc.exe --lastdir $f @args
    $d = Get-Content -LiteralPath $f -Encoding UTF8 -TotalCount 1 -ErrorAction SilentlyContinue
    if ($d -and (Test-Path -LiteralPath $d -PathType Container) -and ((Resolve-Path -LiteralPath $d).Path -ne $PWD.Path)) {
      Set-Location -LiteralPath $d
    }
  } finally {
    Remove-Item -LiteralPath $f -ErrorAction SilentlyContinue
  }
}
