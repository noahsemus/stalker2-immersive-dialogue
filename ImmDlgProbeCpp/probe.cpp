// ImmDlgProbeCpp - read-only diagnostic for the pak port. Logs the player anim
// instance's state machines, linked layers and the anim data structs while in
// static dialogue (and a baseline every few seconds outside it). No writes.
#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Windows.h>
#include <cstring>
#include <cmath>
#include <map>
#include <string>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

class ImmDlgProbe : public CppUserModBase {
public:
    ImmDlgProbe() {
        ModName = STR("ImmDlgProbeCpp"); ModVersion = STR("0.1"); ModAuthors = STR("Noah");
        ModDescription = STR("Read-only anim state probe for the pak port.");
    }
    uint64_t m_lastMs = 0;
    uint64_t m_lastHeartbeatMs = 0;
    uint64_t m_burstUntilMs = 0, m_burstStartMs = 0;
    bool m_prevMoving = false;
    bool m_prevDlg = false;
    uint64_t m_lastOrientMs = 0; std::wstring m_lastOrientLine;
    uint64_t m_lastArmsMs = 0;

    static UFunction* Fn(UObject* o, const wchar_t* n) { return o->GetFunctionByNameInChain(FName(n)); }
    static UObject* ObjProp(UObject* o, const wchar_t* n) {
        FProperty* p = o->GetPropertyByNameInChain(n); if (!p) return nullptr;
        UObject** s = p->ContainerPtrToValuePtr<UObject*>(o); return s ? *s : nullptr;
    }
    static StringType ClassName(UObject* o) {
        if (!o) return STR("null");
        UClass* c = o->GetClassPrivate(); return c ? c->GetName() : StringType(STR("?"));
    }
    // Build name->offset map for a struct property (walks parent structs).
    static void StructOffsets(FProperty* sp, std::map<StringType, int32_t>& out) {
        FStructProperty* sfp = CastField<FStructProperty>(sp); if (!sfp) return;
        for (UStruct* w = sfp->GetStruct(); w; w = w->GetSuperStruct())
            for (FProperty* p : TFieldRange<FProperty>(w, EFieldIterationFlags::None))
                if (p) out[p->GetName()] = p->GetOffset_ForInternal();
    }
    // SEH guards: a fault inside becomes a false return instead of a game crash.
    static bool GuardedProcessEvent(UObject* o, UFunction* fn, void* parms) {
        __try { o->ProcessEvent(fn, parms); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static bool GuardedReadPtrArray(uint8_t* hdr, UObject** out, int32_t cap, int32_t* num) {
        __try {
            UObject** data = *reinterpret_cast<UObject***>(hdr);
            int32_t n = *reinterpret_cast<int32_t*>(hdr + 8);
            if (n < 0 || n > 64 || (n > 0 && !data)) return false;
            *num = n < cap ? n : cap;
            for (int32_t i = 0; i < *num; ++i) out[i] = data[i];
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    // FString = { TCHAR* Data; int32 Num; int32 Max; } — copy out under SEH.
    static bool GuardedReadFString(uint8_t* raw, wchar_t* out, int cap) {
        __try {
            const wchar_t* data = *reinterpret_cast<const wchar_t**>(raw);
            int32_t num = *reinterpret_cast<int32_t*>(raw + 8);
            if (!data || num <= 0 || num > 4096) { out[0] = 0; return true; }
            int n = num - 1; if (n >= cap) n = cap - 1;
            for (int i = 0; i < n; ++i) out[i] = data[i];
            out[n] = 0; return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    // Call fn(FName Name) -> float/bool on inst; returns false on fault/layout problem.
    bool CallNameFn(UObject* inst, const wchar_t* fnName, const wchar_t* arg, float* outF, bool* outB) {
        UFunction* fn = Fn(inst, fnName); if (!fn) return false;
        uint8_t buf[128]; std::memset(buf, 0, sizeof buf);
        int32_t offArg = -1, offRet = -1; bool retIsBool = false;
        for (FProperty* p : TFieldRange<FProperty>(fn, EFieldIterationFlags::None)) {
            if (!p) continue;
            StringType n = p->GetName();
            if (n == STR("ReturnValue")) { offRet = p->GetOffset_ForInternal(); retIsBool = p->GetClass().GetName() == STR("BoolProperty"); }
            else if (offArg < 0) offArg = p->GetOffset_ForInternal();
        }
        if (offArg < 0 || offRet < 0 || offRet + 8 > (int)sizeof buf) return false;
        *reinterpret_cast<FName*>(buf + offArg) = FName(arg, FNAME_Add);
        if (!GuardedProcessEvent(inst, fn, buf)) return false;
        if (retIsBool) { if (outB) *outB = *reinterpret_cast<bool*>(buf + offRet); }
        else { if (outF) *outF = *reinterpret_cast<float*>(buf + offRet); }
        return true;
    }
    StringType GestureSignals(UObject* inst) {
        float cv = -9; bool slot = false, mont = false;
        bool okC = CallNameFn(inst, STR("GetCurveValue"), STR("AdditiveMovingUpperBody"), &cv, nullptr);
        bool okS = CallNameFn(inst, STR("IsSlotActive"), STR("UpperBody"), nullptr, &slot);
        if (UFunction* f = Fn(inst, STR("IsAnyMontagePlaying"))) { struct { bool R = false; } p; if (GuardedProcessEvent(inst, f, &p)) mont = p.R; }
        wchar_t b[96]; swprintf(b, 96, L"curve=%s%.2f slot=%s%d montage=%d", okC ? L"" : L"?", cv, okS ? L"" : L"?", slot ? 1 : 0, mont ? 1 : 0);
        return b;
    }
    // Call UAnimInstance::GetCurrentStateName(int32) using the UFunction's real param layout.
    StringType StateName(UObject* inst, UFunction* fn, int32_t machine) {
        uint8_t buf[128]; std::memset(buf, 0, sizeof buf);
        int32_t offIdx = -1, offRet = -1;
        for (FProperty* p : TFieldRange<FProperty>(fn, EFieldIterationFlags::None)) {
            if (!p) continue;
            StringType n = p->GetName();
            if (n == STR("MachineIndex")) offIdx = p->GetOffset_ForInternal();
            else if (n == STR("ReturnValue")) offRet = p->GetOffset_ForInternal();
        }
        if (offIdx < 0 || offRet < 0 || offRet + 8 > (int)sizeof buf) return STR("(layout?)");
        *reinterpret_cast<int32_t*>(buf + offIdx) = machine;
        if (!GuardedProcessEvent(inst, fn, buf)) return STR("(fault)");
        return reinterpret_cast<FName*>(buf + offRet)->ToString();
    }

    auto on_update() -> void override {
        uint64_t hb = GetTickCount64();
        // Same lookup the shipped DLL uses; it must load before UObjectCacheMod in mods.txt.
        UObject* pawn = UObjectGlobals::FindFirstOf(STR("PC"));
        const wchar_t* how = pawn ? STR("FindFirstOf(PC)") : STR("none");
        std::vector<UObject*> pcs; std::vector<UObject*> pcClass;
        if (pawn && pawn->IsUnreachable()) { pawn = nullptr; how = STR("unreachable"); }
        if (hb - m_lastHeartbeatMs > 5000) {
            m_lastHeartbeatMs = hb;
            Output::send<LogLevel::Verbose>(STR("[Probe] heartbeat via={} controllers={} pcObjs={} pawn={}\n"), how, pcs.size(), pcClass.size(), pawn ? pawn->GetFullName() : StringType(STR("null")));
        }
        // Post-process instance status on every heartbeat (build A needs it outside dialogue too).
        if (pawn && hb == m_lastHeartbeatMs) {
            UObject* meshH = ObjProp(pawn, STR("Mesh"));
            UObject* ppH = meshH ? ObjProp(meshH, STR("PostProcessAnimInstance")) : nullptr;
            UObject* mainH = meshH ? ObjProp(meshH, STR("AnimScriptInstance")) : nullptr;
            Output::send<LogLevel::Verbose>(STR("[Probe] hb postproc class={} main={}\n"), ClassName(ppH), ClassName(mainH));
            // Hidden-bone state of the first-person body (a mesh swap resets it to all-visible).
            if (meshH) {
                StringType hidden;
                for (const wchar_t* bn : { STR("jnt_head"), STR("jnt_neck"), STR("jnt_spine_03"), STR("jnt_spine_02"), STR("jnt_l_clavicle"), STR("jnt_r_clavicle") }) {
                    if (UFunction* f = Fn(meshH, STR("IsBoneHiddenByName"))) {
                        struct { FName BoneName; bool R; } p{}; p.BoneName = FName(bn);
                        if (GuardedProcessEvent(meshH, f, &p)) { hidden += StringType(bn) + (p.R ? STR("=H ") : STR("=v ")); }
                    }
                }
                Output::send<LogLevel::Verbose>(STR("[Probe] hb bones {}\n"), hidden);
            }
        }
        // Body-turning check, in and out of dialogue: orient-to-movement flag, actor yaw vs control yaw, mesh relative yaw. Logged twice a second when something changes.
        if (pawn && hb - m_lastOrientMs > 500) {
            m_lastOrientMs = hb;
            auto readBit = [&](UObject* o, const wchar_t* n) -> int {
                if (!o) return -1; FProperty* p = o->GetPropertyByNameInChain(n); if (!p) return -1;
                FBoolProperty* bp = CastField<FBoolProperty>(p); if (!bp) return -1; uint8_t* raw = p->ContainerPtrToValuePtr<uint8_t>(o); return raw ? (bp->GetPropertyValue(raw) ? 1 : 0) : -1; };
            struct FR { double P, Y, R; };
            auto rotFn = [&](UObject* o, const wchar_t* fn, FR* out) -> bool {
                if (!o) return false; UFunction* f = Fn(o, fn); if (!f) return false; struct { FR R; } p{}; if (!GuardedProcessEvent(o, f, &p)) return false; *out = p.R; return true; };
            UObject* cmc = ObjProp(pawn, STR("CharacterMovement")); UObject* meshO = ObjProp(pawn, STR("Mesh"));
            int orient = readBit(cmc, STR("bOrientRotationToMovement")); int useYaw = readBit(pawn, STR("bUseControllerRotationYaw"));
            FR ar{}, cr{}, mr{}; rotFn(pawn, STR("K2_GetActorRotation"), &ar); rotFn(pawn, STR("GetControlRotation"), &cr); rotFn(meshO, STR("K2_GetComponentRotation"), &mr);
            wchar_t line[256]; swprintf_s(line, 256, L"orient=%d useCtrlYaw=%d actorYaw=%.0f ctrlYaw=%.0f meshYaw=%.0f", orient, useYaw, ar.Y, cr.Y, mr.Y);
            if (m_lastOrientLine != line) { m_lastOrientLine = line; Output::send<LogLevel::Verbose>(STR("[Probe] turn {}\n"), StringType(line)); }
        }
        // Arms check, in and out of dialogue, once a second: where the hands are relative to the camera and the shoulders
        // (collapsed = scaled away, far below = posed out of view), plus whether the arm bones are hidden.
        if (pawn && hb - m_lastArmsMs > 1000) {
            m_lastArmsMs = hb;
            struct FV { double X, Y, Z; };
            UObject* meshA = ObjProp(pawn, STR("Mesh")); UObject* camA = ObjProp(pawn, STR("Camera"));
            auto sock = [&](const wchar_t* sn, FV* out) -> bool {
                if (!meshA) return false; UFunction* f = Fn(meshA, STR("GetSocketLocation")); if (!f) return false;
                struct { FName InSocketName; FV R; } p{}; p.InSocketName = FName(sn); if (!GuardedProcessEvent(meshA, f, &p)) return false; *out = p.R; return true; };
            auto hid = [&](const wchar_t* bn) -> int {
                if (!meshA) return -1; UFunction* f = Fn(meshA, STR("IsBoneHiddenByName")); if (!f) return -1;
                struct { FName BoneName; bool R; } p{}; p.BoneName = FName(bn); return GuardedProcessEvent(meshA, f, &p) ? (p.R ? 1 : 0) : -1; };
            FV cam{}; if (camA) { if (UFunction* f = Fn(camA, STR("K2_GetComponentLocation"))) { struct { FV R; } p{}; if (GuardedProcessEvent(camA, f, &p)) cam = p.R; } }
            FV lh{}, rh{}, ls{}, rs{}; sock(STR("jnt_l_hand"), &lh); sock(STR("jnt_r_hand"), &rh); sock(STR("jnt_l_shoulder"), &ls); sock(STR("jnt_r_shoulder"), &rs);
            auto dist = [](FV a, FV b) { double dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z; return std::sqrt(dx*dx + dy*dy + dz*dz); };
            bool inDlgA = false; if (UFunction* f = Fn(pawn, STR("IsInStaticDialog"))) { struct { bool R = false; } p; if (GuardedProcessEvent(pawn, f, &p)) inDlgA = p.R; }
            int vis = -1; if (meshA) if (FProperty* p = meshA->GetPropertyByNameInChain(STR("bVisible"))) if (FBoolProperty* bp = CastField<FBoolProperty>(p)) { uint8_t* raw = p->ContainerPtrToValuePtr<uint8_t>(meshA); if (raw) vis = bp->GetPropertyValue(raw) ? 1 : 0; }
            Output::send<LogLevel::Verbose>(STR("[Probe] arms dlg={} Lhand-cam dz={:.0f} d={:.0f} Lhand-shoulder={:.0f} | Rhand-cam dz={:.0f} d={:.0f} Rhand-shoulder={:.0f} | hidden arm L={} R={} hand L={} R={} meshVisible={}\n"),
                inDlgA ? 1 : 0, lh.Z - cam.Z, dist(lh, cam), dist(lh, ls), rh.Z - cam.Z, dist(rh, cam), dist(rh, rs), hid(STR("jnt_l_arm")), hid(STR("jnt_r_arm")), hid(STR("jnt_l_hand")), hid(STR("jnt_r_hand")), vis);
        }
        if (!pawn) return;
        void* dlgPtr = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(pawn) + 0x650);
        if (!dlgPtr) return;
        bool inDlg = false;
        if (UFunction* f = Fn(pawn, STR("IsInStaticDialog"))) { struct { bool R = false; } p; if (GuardedProcessEvent(pawn, f, &p)) inDlg = p.R; }
        if (!inDlg) { m_prevDlg = false; return; }   // read anim data only inside dialogue (the window the DLL used safely)
        uint64_t now = GetTickCount64();
        // Burst mode: once DlgMoving rises, log every frame for ~1.5 s so the start-of-move timeline is visible.
        bool dlgMovingNow = false;
        {
            UObject* mesh0 = ObjProp(pawn, STR("Mesh")); UObject* inst0 = mesh0 ? ObjProp(mesh0, STR("AnimScriptInstance")) : nullptr;
            if (inst0) if (FProperty* p = inst0->GetPropertyByNameInChain(STR("DlgMoving"))) { bool* b = p->ContainerPtrToValuePtr<bool>(inst0); if (b) dlgMovingNow = *b; }
        }
        if (dlgMovingNow && !m_prevMoving) { m_burstUntilMs = now + 1500; m_burstStartMs = now; }
        m_prevMoving = dlgMovingNow;
        bool burst = now < m_burstUntilMs;
        if (!burst && now - m_lastMs < 500 && m_prevDlg) return;
        m_lastMs = now; m_prevDlg = true;

        UObject* mesh = ObjProp(pawn, STR("Mesh"));
        if (!mesh) { Output::send<LogLevel::Verbose>(STR("[Probe] no Mesh\n")); return; }
        UObject* inst = ObjProp(mesh, STR("AnimScriptInstance"));
        if (!inst) { Output::send<LogLevel::Verbose>(STR("[Probe] no AnimScriptInstance\n")); return; }

        // Post-process anim instance on the player mesh (build A of the no-override plan sets it at runtime).
        {
            UObject* pp = ObjProp(mesh, STR("PostProcessAnimInstance"));
            int ppDisabled = -1;
            if (FProperty* p = mesh->GetPropertyByNameInChain(STR("bDisablePostProcessBlueprint"))) {
                if (FBoolProperty* bp = CastField<FBoolProperty>(p)) { uint8_t* raw = p->ContainerPtrToValuePtr<uint8_t>(mesh); if (raw) ppDisabled = bp->GetPropertyValue(raw) ? 1 : 0; }
            }
            Output::send<LogLevel::Verbose>(STR("[Probe] postproc class={} disabled={} main={}\n"), ClassName(pp), ppDisabled, ClassName(inst));
            // Walk layer variables live on the post-process instance from the no-override build.
            if (pp) {
                int ppMoving = -1; float ppFwd = 0, ppRight = 0, ppWalk = 0, ppGest = 0;
                if (FProperty* p = pp->GetPropertyByNameInChain(STR("DlgMoving")))   { bool* b = p->ContainerPtrToValuePtr<bool>(pp); if (b) ppMoving = *b ? 1 : 0; }
                if (FProperty* p = pp->GetPropertyByNameInChain(STR("DlgFwd")))      { double* f = p->ContainerPtrToValuePtr<double>(pp); if (f) ppFwd = (float)*f; }
                if (FProperty* p = pp->GetPropertyByNameInChain(STR("DlgRight")))    { double* f = p->ContainerPtrToValuePtr<double>(pp); if (f) ppRight = (float)*f; }
                if (FProperty* p = pp->GetPropertyByNameInChain(STR("WalkAlpha")))   { double* f = p->ContainerPtrToValuePtr<double>(pp); if (f) ppWalk = (float)*f; }
                if (FProperty* p = pp->GetPropertyByNameInChain(STR("GestureAlpha"))){ double* f = p->ContainerPtrToValuePtr<double>(pp); if (f) ppGest = (float)*f; }
                Output::send<LogLevel::Verbose>(STR("[Probe] pp vars moving={} fwd={:.2f} right={:.2f} walkAlpha={:.2f} gestureAlpha={:.2f}\n"), ppMoving, ppFwd, ppRight, ppWalk, ppGest);
            }
        }
        // Camera vs jnt_camera socket, world space: tells whether the camera still rides on the head bone.
        {
            struct FV { double X, Y, Z; };
            auto compLoc = [&](UObject* c, FV* out) -> bool {
                if (!c) return false; UFunction* f = Fn(c, STR("K2_GetComponentLocation")); if (!f) return false;
                struct { FV R; } p{}; if (!GuardedProcessEvent(c, f, &p)) return false; *out = p.R; return true; };
            auto sockLoc = [&](UObject* c, const wchar_t* sock, FV* out) -> bool {
                if (!c) return false; UFunction* f = Fn(c, STR("GetSocketLocation")); if (!f) return false;
                struct { FName InSocketName; FV R; } p{}; p.InSocketName = FName(sock); if (!GuardedProcessEvent(c, f, &p)) return false; *out = p.R; return true; };
            UObject* cam = ObjProp(pawn, STR("Camera"));
            FV c{}, s{}, h{}; bool okC = compLoc(cam, &c); bool okS = sockLoc(mesh, STR("jnt_camera"), &s); bool okH = sockLoc(mesh, STR("jnt_head"), &h);
            Output::send<LogLevel::Verbose>(STR("[Probe] camsock cam-jnt_camera=({:.1f},{:.1f},{:.1f}) cam-jnt_head=({:.1f},{:.1f},{:.1f}) ok={}{}{}\n"),
                c.X - s.X, c.Y - s.Y, c.Z - s.Z, c.X - h.X, c.Y - h.Y, c.Z - h.Z, okC ? 1 : 0, okS ? 1 : 0, okH ? 1 : 0);
        }
        // Camera/mesh placement, to compare builds when "the body looks different" (read only in dialogue).
        {
            auto vec3 = [&](UObject* o, const wchar_t* n, double* out) -> bool {
                if (!o) return false; FProperty* p = o->GetPropertyByNameInChain(n); if (!p) return false;
                double* v = p->ContainerPtrToValuePtr<double>(o); if (!v) return false; out[0] = v[0]; out[1] = v[1]; out[2] = v[2]; return true; };
            UObject* cam = ObjProp(pawn, STR("Camera"));
            double cl[3] = {0,0,0}, ml[3] = {0,0,0}; float fov = -1;
            bool okC = vec3(cam, STR("RelativeLocation"), cl); bool okM = vec3(mesh, STR("RelativeLocation"), ml);
            if (cam) if (FProperty* p = cam->GetPropertyByNameInChain(STR("FieldOfView"))) { float* f = p->ContainerPtrToValuePtr<float>(cam); if (f) fov = *f; }
            Output::send<LogLevel::Verbose>(STR("[Probe] placement cam=({:.1f},{:.1f},{:.1f}){} mesh=({:.1f},{:.1f},{:.1f}){} fov={:.1f} camParent={}\n"),
                cl[0], cl[1], cl[2], okC ? STR("") : STR("?"), ml[0], ml[1], ml[2], okM ? STR("") : STR("?"), fov, ClassName(cam ? ObjProp(cam, STR("AttachParent")) : nullptr));
        }

        // Our override variables (only present on the pak build's class).
        int dlgMoving = -1; float dlgFwd = 0, dlgRight = 0;
        if (FProperty* p = inst->GetPropertyByNameInChain(STR("DlgMoving"))) { bool* b = p->ContainerPtrToValuePtr<bool>(inst); if (b) dlgMoving = *b ? 1 : 0; }
        if (FProperty* p = inst->GetPropertyByNameInChain(STR("DlgFwd")))    { double* f = p->ContainerPtrToValuePtr<double>(inst); if (f) dlgFwd = (float)*f; }
        if (FProperty* p = inst->GetPropertyByNameInChain(STR("DlgRight")))  { double* f = p->ContainerPtrToValuePtr<double>(inst); if (f) dlgRight = (float)*f; }
        StringType dbgState = STR("(no DbgState)");
        if (FProperty* p = inst->GetPropertyByNameInChain(STR("DbgState"))) {
            uint8_t* raw = p->ContainerPtrToValuePtr<uint8_t>(inst);
            wchar_t tmp[128]; tmp[0] = 0;
            if (raw && GuardedReadFString(raw, tmp, 128)) dbgState = tmp; else dbgState = STR("(read fault)");
        }

        // State machines.
        StringType states = STR("Moving=") + dbgState;
        int gestureVar = -1, speedLim = -1;
        if (FProperty* p = inst->GetPropertyByNameInChain(STR("GestureActive"))) { bool* b = p->ContainerPtrToValuePtr<bool>(inst); if (b) gestureVar = *b ? 1 : 0; }
        if (FProperty* p = inst->GetPropertyByNameInChain(STR("SpeedLimited")))  { bool* b = p->ContainerPtrToValuePtr<bool>(inst); if (b) speedLim = *b ? 1 : 0; }
        states += STR(" gestureVar=") + std::to_wstring(gestureVar) + STR(" speedLim=") + std::to_wstring(speedLim);
        states += STR(" MAIN{") + GestureSignals(inst) + STR("}");

        // Linked layer instances on the mesh.
        StringType linked;
        if (FProperty* p = mesh->GetPropertyByNameInChain(STR("LinkedInstances"))) {
            uint8_t* hdr = p->ContainerPtrToValuePtr<uint8_t>(mesh);
            UObject* arr[16]; int32_t num = 0;
            if (hdr && GuardedReadPtrArray(hdr, arr, 16, &num)) { for (int32_t i = 0; i < num; ++i) { if (!arr[i]) continue; linked += ClassName(arr[i]) + STR("{") + GestureSignals(arr[i]) + STR("},"); } }
            else linked = STR("(read fault)");
        } else linked = STR("(no LinkedInstances prop)");

        // state_data / locomotion_data / dialog_data on the main instance.
        auto readStruct = [&](const wchar_t* a, const wchar_t* b, FProperty*& prop, std::map<StringType, int32_t>& offs) {
            prop = inst->GetPropertyByNameInChain(a); if (!prop) prop = inst->GetPropertyByNameInChain(b);
            if (prop) StructOffsets(prop, offs);
        };
        FProperty* sdP = nullptr; std::map<StringType, int32_t> sd; readStruct(STR("state_data"), STR("StateData"), sdP, sd);
        FProperty* ldP = nullptr; std::map<StringType, int32_t> ld; readStruct(STR("locomotion_data"), STR("LocomotionData"), ldP, ld);
        FProperty* ddP = inst->GetPropertyByNameInChain(STR("dialog_data")); if (!ddP) ddP = inst->GetPropertyByNameInChain(STR("DialogData"));
        auto rb = [&](FProperty* p, std::map<StringType, int32_t>& m, const wchar_t* n) -> int {
            if (!p) return -1; auto it = m.find(n); if (it == m.end()) return -1;
            uint8_t* base = p->ContainerPtrToValuePtr<uint8_t>(inst); return base ? (*(bool*)(base + it->second) ? 1 : 0) : -1; };
        auto rf = [&](FProperty* p, std::map<StringType, int32_t>& m, const wchar_t* n) -> float {
            if (!p) return -9.0f; auto it = m.find(n); if (it == m.end()) return -9.0f;
            uint8_t* base = p->ContainerPtrToValuePtr<uint8_t>(inst); return base ? *(float*)(base + it->second) : -9.0f; };
        auto ru8 = [&](FProperty* p, std::map<StringType, int32_t>& m, const wchar_t* n) -> int {
            if (!p) return -1; auto it = m.find(n); if (it == m.end()) return -1;
            uint8_t* base = p->ContainerPtrToValuePtr<uint8_t>(inst); return base ? (int)*(base + it->second) : -1; };
        // MovementPlayRate is a nested struct: resolve its sub-offsets.
        float mprR = -9, mprF = -9, mprP = -9;
        if (ldP) {
            FStructProperty* sfp = CastField<FStructProperty>(ldP);
            for (UStruct* w = sfp ? sfp->GetStruct() : nullptr; w; w = w->GetSuperStruct())
                for (FProperty* p : TFieldRange<FProperty>(w, EFieldIterationFlags::None)) {
                    if (!p || p->GetName() != STR("MovementPlayRate")) continue;
                    std::map<StringType, int32_t> sub; StructOffsets(p, sub);
                    uint8_t* base = ldP->ContainerPtrToValuePtr<uint8_t>(inst) + p->GetOffset_ForInternal();
                    if (sub.count(STR("RightValue")))   mprR = *(float*)(base + sub[STR("RightValue")]);
                    if (sub.count(STR("ForwardValue"))) mprF = *(float*)(base + sub[STR("ForwardValue")]);
                    if (sub.count(STR("PlayRate")))     mprP = *(float*)(base + sub[STR("PlayRate")]);
                }
        }
        int dialogBit = -1;
        if (ddP) { uint8_t* dm = ddP->ContainerPtrToValuePtr<uint8_t>(inst); if (dm) dialogBit = *dm ? 1 : 0; }
        bool anyMontage = false;
        if (UFunction* f = Fn(inst, STR("IsAnyMontagePlaying"))) { struct { bool R = false; } p; if (GuardedProcessEvent(inst, f, &p)) anyMontage = p.R; }

        Output::send<LogLevel::Verbose>(
            STR("[Probe] t+{}ms inDlg={} class={} Dlg(moving={} fwd={:.2f} right={:.2f}) states=[{}] linked=[{}] montage={} | sd: moving={} walking={} running={} sprint={} walkOvr={} dynGait={:.1f} curveGait={:.1f} enumGait={} cutscene={} actionSlot={} fullBody={} | ld: vel={:.0f} MPR(R={:.2f} F={:.2f} P={:.2f}) angle={:.0f} | dialog={}\n"),
            burst ? (now - m_burstStartMs) : 0, inDlg ? 1 : 0, ClassName(inst), dlgMoving, dlgFwd, dlgRight, states, linked, anyMontage ? 1 : 0,
            rb(sdP, sd, STR("bMoving")), rb(sdP, sd, STR("bWalking")), rb(sdP, sd, STR("bRunning")), rb(sdP, sd, STR("bSprinting")), rb(sdP, sd, STR("bWalkingOverride")),
            rf(sdP, sd, STR("DynamicGaitValue")), rf(sdP, sd, STR("CurveGaitValue")), ru8(sdP, sd, STR("EnumGaitState")),
            rb(sdP, sd, STR("bCutscene")), rb(sdP, sd, STR("bActionSlotActive")), rb(sdP, sd, STR("bFullBodySlotActive")),
            rf(ldP, ld, STR("Velocity")), mprR, mprF, mprP, rf(ldP, ld, STR("AngleDirection")), dialogBit);
    }
};

#define PROBE_API __declspec(dllexport)
extern "C" {
    PROBE_API CppUserModBase* start_mod() { return new ImmDlgProbe(); }
    PROBE_API void uninstall_mod(CppUserModBase* mod) { delete mod; }
}
