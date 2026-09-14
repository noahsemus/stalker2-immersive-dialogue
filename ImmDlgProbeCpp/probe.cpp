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
