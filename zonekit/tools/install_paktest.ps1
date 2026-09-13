# Install the Zone Kit OverrideContent build as a test pak and disable the UE4SS DLL for a clean A/B.
# Run only while the game is closed. Revert with revert_paktest.ps1.
$kit  = "G:\Epic Games\STALKER2ZoneKit"
$game = "C:\Program Files (x86)\Steam\steamapps\common\S.T.A.L.K.E.R. 2 Heart of Chornobyl\Stalker2"
$src  = "$kit\Stalker2\SavedMods\Staged\ImmersiveDialogue\Windows\OverrideContent\Windows\Stalker2\Mods\ImmersiveDialogue\Content\Paks\Windows"
$dst  = "$game\Content\Paks\~mods\zzz_ImmersiveDialogue_PakTest"
if (Get-Process -Name "Stalker2-Win64-Shipping" -ErrorAction SilentlyContinue) { throw "game is running" }
New-Item -ItemType Directory -Force $dst | Out-Null
Get-ChildItem "$src\ImmersiveDialogueStalker2-Windows-OverrideContent.*" | Copy-Item -Destination $dst -Force
$modsTxt = "$game\Binaries\Win64\ue4ss\Mods\mods.txt"
(Get-Content $modsTxt) -replace '^ImmersiveDialogueCpp : 1', 'ImmersiveDialogueCpp : 0' | Set-Content $modsTxt -Encoding utf8
Get-ChildItem $dst | Select-Object Name, Length, LastWriteTime
Select-String -Path $modsTxt -Pattern ImmersiveDialogueCpp
