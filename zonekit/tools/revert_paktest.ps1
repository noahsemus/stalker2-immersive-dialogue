# Remove the dev test pak(s) and re-enable the 1.x UE4SS DLL entry.
$game = "C:\Program Files (x86)\Steam\steamapps\common\S.T.A.L.K.E.R. 2 Heart of Chornobyl\Stalker2"
if (Get-Process -Name "Stalker2-Win64-Shipping" -ErrorAction SilentlyContinue) { throw "game is running" }
Get-ChildItem "$game\Content\Paks\~mods" -Directory -Filter "zzz_ImmersiveDialogue*_PakTest" | Remove-Item -Recurse -Force
$modsTxt = "$game\Binaries\Win64\ue4ss\Mods\mods.txt"
(Get-Content $modsTxt) -replace '^ImmersiveDialogueCpp : 0', 'ImmersiveDialogueCpp : 1' | Set-Content $modsTxt -Encoding ascii
Select-String -Path $modsTxt -Pattern ImmersiveDialogueCpp
