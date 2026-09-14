# Cook the ImmersiveDialogue Zone Kit mod and install the override pak into the game (as a _20_P patch pak).
# Usage: save your assets in the Zone Kit editor, then run this from PowerShell. Takes 5-6 minutes.
$kit = "G:\Epic Games\STALKER2ZoneKit"
$log = "$env:TEMP\ImmersiveDialogue_cook.log"
Write-Host "Cooking (log: $log) ..."
& "$kit\Engine\Build\BatchFiles\RunUAT.bat" GSCCookMod "-Project=$kit\Stalker2\Stalker2.uproject" "-PluginPath=$kit\Stalker2\Mods\ImmersiveDialogue\ImmersiveDialogue.uplugin" "-PackageClassifierOutputDir=$kit\Stalker2\SavedMods\PackageClassifier\ImmersiveDialogue" "-UnrealExe=$kit\Stalker2\Binaries\Win64\Stalker2ModEditor-Win64-Shipping-Cmd.exe" -TargetPlatform=Win64 -nocompile -nocompileuat *> $log
if ($LASTEXITCODE -ne 0) { Write-Host "COOK FAILED (exit $LASTEXITCODE). See $log"; exit 1 }
Write-Host "Cook OK."
while (Get-Process -Name "Stalker2-Win64-Shipping" -ErrorAction SilentlyContinue) { Write-Host "Game is running - close it to install..."; Start-Sleep -Seconds 10 }
& "$PSScriptRoot\install_paktest.ps1" | Out-Null
Write-Host "Installed:"
Get-ChildItem "C:\Program Files (x86)\Steam\steamapps\common\S.T.A.L.K.E.R. 2 Heart of Chornobyl\Stalker2\Content\Paks\~mods\zzz_ImmersiveDialogue_PakTest" | Select-Object Name, Length, LastWriteTime
