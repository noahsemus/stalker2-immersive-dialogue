# Remove the test pak and re-enable the UE4SS DLL.
$game = "C:\Program Files (x86)\Steam\steamapps\common\S.T.A.L.K.E.R. 2 Heart of Chornobyl\Stalker2"
if (Get-Process -Name "Stalker2-Win64-Shipping" -ErrorAction SilentlyContinue) { throw "game is running" }
Remove-Item -Recurse -Force "$game\Content\Paks\~mods\zzz_ImmersiveDialogue_PakTest" -ErrorAction SilentlyContinue
$modsTxt = "$game\Binaries\Win64\ue4ss\Mods\mods.txt"
(Get-Content $modsTxt) -replace '^ImmersiveDialogueCpp : 0', 'ImmersiveDialogueCpp : 1' | Set-Content $modsTxt -Encoding utf8
Select-String -Path $modsTxt -Pattern ImmersiveDialogueCpp
