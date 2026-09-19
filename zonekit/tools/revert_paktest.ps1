# Remove the dev test pak(s). Does not touch ue4ss\Mods\mods.txt: re-enabling the 1.x DLL here would run it
# alongside a Vortex-installed 2.x pak.
$game = "C:\Program Files (x86)\Steam\steamapps\common\S.T.A.L.K.E.R. 2 Heart of Chornobyl\Stalker2"
if (Get-Process -Name "Stalker2-Win64-Shipping" -ErrorAction SilentlyContinue) { throw "game is running" }
Get-ChildItem "$game\Content\Paks\~mods" -Directory -Filter "zzz_ImmersiveDialogue*_PakTest" | Remove-Item -Recurse -Force
