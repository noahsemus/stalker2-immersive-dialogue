-- ImmDlgProbe: read-only diagnostic for the pak port. Logs once per second while in static dialog.
local function S(x) local ok, r = pcall(function() return tostring(x) end); return ok and r or "?" end
local function safe(f, ...) local ok, r = pcall(f, ...); if ok then return r end; return "ERR:" .. S(r) end
local last = ""
LoopAsync(1000, function()
  ExecuteInGameThread(function()
    local ok, err = pcall(function()
      local pawn = FindFirstOf("BP_Stalker2Character_C")
      if not pawn or not pawn:IsValid() then return end
      local inDlg = S(safe(function() return pawn:IsInStaticDialog() end))
      if inDlg ~= "true" then return end
      local mesh = pawn.Mesh
      if not mesh or not mesh:IsValid() then print("[Probe] no mesh\n"); return end
      local inst = mesh.AnimScriptInstance
      local line = "[Probe] inDlg=" .. inDlg
      if inst and inst:IsValid() then
        line = line .. " animClass=" .. S(inst:GetClass():GetFullName())
        line = line .. " DlgMoving=" .. S(safe(function() return inst.DlgMoving end))
        line = line .. " DlgFwd=" .. S(safe(function() return inst.DlgFwd end))
        if inDlg == "true" then print(line .. "\n") end
        local states = {}
        for i = 0, 5 do
          local n = S(safe(function() return inst:GetCurrentStateName(i):ToString() end))
          states[#states + 1] = i .. ":" .. n
        end
        line = line .. " machines=" .. S(safe(function() return inst:GetStateMachineIndex(FName("Moving")) end))
        line = line .. " states=[" .. table.concat(states, " ") .. "]"
        line = line .. " anyMontage=" .. S(safe(function() return inst:IsAnyMontagePlaying() end))
        if inDlg == "true" then print(line .. "\n") end
      else
        line = line .. " animInst=nil"
      end
      local linked = {}
      local lerr = safe(function()
        local arr = mesh.LinkedInstances
        linked[#linked + 1] = "n=" .. S(arr:GetArrayNum())
        arr:ForEach(function(i, e)
          local o = e:get()
          if o and o:IsValid() then linked[#linked + 1] = S(o:GetClass():GetName()) end
        end)
        return "ok"
      end)
      line = line .. " linked=[" .. table.concat(linked, ",") .. "] lerr=" .. S(lerr)
      line = line .. " layerByTag=" .. S(safe(function() local o = inst:GetLinkedAnimLayerInstanceByGroup(FName("WeaponLayer")); return o and o:IsValid() and o:GetClass():GetName() or "nil" end))
      if inDlg == "true" or line ~= last then print(line .. "\n") end
      last = line
    end)
    if not ok then print("[Probe] error: " .. S(err) .. "\n") end
  end)
  return false
end)
print("[Probe] loaded\n")
