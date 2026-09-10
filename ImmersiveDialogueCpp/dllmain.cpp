// ImmersiveDialogue (C++ UE4SS mod) — free movement + mouse/pad look during STALKER 2 dialogue.
//
// While PC::IsInStaticDialog() is true:
//   - WASD walks the character (camera-relative, walk speed). W/A/S/D are swallowed at the
//     WndProc layer so the dialogue widget's option list doesn't also scroll on those keys.
//   - Raw mouse looks (release-fraction smoothed). Multiplier = BASE_MOUSE_SENS *
//     MouseSensitivityCoef from AppliedSettingsWin64.cfg. InvertMouseYAxis honored.
//   - Xbox-style controller works (via XInput). Left stick moves, right stick looks. The
//     game's polling of XInputGetState is IAT-patched so the game sees zeroed left stick
//     values (D-pad still navigates dialogue options); our own polling reads the real state.
//     GamepadSensitivityCoef + GamepadInvert{X,Y}Axis honored.
//   - Escape and gamepad B pass through untouched — game handles them natively (which in
//     STALKER 2 dialogue usually means: B exits, Esc does nothing).
//
// What this mod does NOT do (proven not reachable through UE4SS's UFunction reflection):
//   - Open a working pause menu during dialogue (game gates it below the reflection layer).
//   - Force footstep audio during dialogue movement (anim state manipulated via native C++).
//   - Make Esc close the dialogue (game doesn't handle Esc-in-dialogue).

#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Constructs/Loop.hpp>

#include <vector>
#include <map>

#include <Windows.h>
#include <Xinput.h>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <string>

using namespace RC;
using namespace RC::Unreal;

// UE5 math types (STALKER 2 is UE5.5 — LWC = doubles).
struct FVectorD  { double X, Y, Z; };
struct FRotatorD { double Pitch, Yaw, Roll; };

// ================= Shared state (WndProc thread <-> game thread) =================
static std::atomic<long>  g_dx{0};
static std::atomic<long>  g_dy{0};
static std::atomic<bool>  g_inDialogue{false};

// Settings loaded from AppliedSettingsWin64.cfg (refreshed on each dialogue entry).
static std::atomic<double> g_mouseSensCoef{1.0};
static std::atomic<double> g_padSensCoef  {1.0};
static std::atomic<bool>   g_invertMouseX {false};
static std::atomic<bool>   g_invertMouseY {false};
static std::atomic<bool>   g_invertPadX   {false};
static std::atomic<bool>   g_invertPadY   {false};

static WNDPROC g_origWndProc = nullptr;
static bool    g_rawReady    = false;

// Thread-local: set true right before OUR own IsInStaticDialog polls; the post-hook uses
// this to distinguish our polling (return truth) from every other caller (return lie).
static thread_local bool tl_selfDialogueQuery = false;

// ================= Controller (XInput) with IAT-patched hook =================
// Patch the game's IAT for XInputGetState so its polls see zeroed left stick during
// dialogue (dialogue option list stops scrolling; D-pad still navigates). Our own
// polling uses this DLL's IAT (separate), so we always get real values.
using XInputGetStateFn = DWORD (WINAPI*)(DWORD, XINPUT_STATE*);
static XInputGetStateFn g_realXInputGetState = nullptr;
static bool g_xinputHooked = false;

static DWORD WINAPI HookedXInputGetState(DWORD userIndex, XINPUT_STATE* state) {
    DWORD r = g_realXInputGetState ? g_realXInputGetState(userIndex, state)
                                   : XInputGetState(userIndex, state);
    if (r == ERROR_SUCCESS && state && g_inDialogue.load(std::memory_order_relaxed)) {
        state->Gamepad.sThumbLX = 0;
        state->Gamepad.sThumbLY = 0;
    }
    return r;
}

static bool PatchIATEntry(HMODULE hModule, const char* dllName, const char* funcName,
                          void* newFunc, void** outOrig) {
    if (!hModule) return false;
    auto dosHdr = reinterpret_cast<PIMAGE_DOS_HEADER>(hModule);
    if (dosHdr->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto ntHdr = reinterpret_cast<PIMAGE_NT_HEADERS>(
        reinterpret_cast<BYTE*>(hModule) + dosHdr->e_lfanew);
    if (ntHdr->Signature != IMAGE_NT_SIGNATURE) return false;
    auto& impDir = ntHdr->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (impDir.Size == 0) return false;
    auto imp = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(
        reinterpret_cast<BYTE*>(hModule) + impDir.VirtualAddress);
    for (; imp->Name != 0; ++imp) {
        auto name = reinterpret_cast<const char*>(reinterpret_cast<BYTE*>(hModule) + imp->Name);
        if (_stricmp(name, dllName) != 0) continue;
        auto thunk = reinterpret_cast<PIMAGE_THUNK_DATA>(reinterpret_cast<BYTE*>(hModule) + imp->FirstThunk);
        auto origThunk = reinterpret_cast<PIMAGE_THUNK_DATA>(
            reinterpret_cast<BYTE*>(hModule) + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        for (; origThunk->u1.AddressOfData != 0; ++origThunk, ++thunk) {
            if (origThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            auto ibn = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(reinterpret_cast<BYTE*>(hModule) + origThunk->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(ibn->Name), funcName) != 0) continue;
            DWORD oldProt = 0;
            if (!VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProt)) return false;
            if (outOrig) *outOrig = reinterpret_cast<void*>(thunk->u1.Function);
            thunk->u1.Function = reinterpret_cast<uintptr_t>(newFunc);
            VirtualProtect(&thunk->u1.Function, sizeof(void*), oldProt, &oldProt);
            return true;
        }
    }
    return false;
}

static void InstallXInputHook() {
    HMODULE exe = GetModuleHandleW(nullptr);
    void* orig = nullptr;
    const char* dlls[] = { "xinput1_4.dll", "XINPUT1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    for (auto* d : dlls) {
        if (PatchIATEntry(exe, d, "XInputGetState", (void*)&HookedXInputGetState, &orig)) {
            if (!g_realXInputGetState && orig) g_realXInputGetState = reinterpret_cast<XInputGetStateFn>(orig);
            g_xinputHooked = true;
        }
    }
}

// Read controller sticks + buttons. Left/right sticks normalized to [-1,+1] with deadzone.
static bool ReadPadSticks(double& outMoveX, double& outMoveY,
                          double& outLookX, double& outLookY) {
    outMoveX = outMoveY = outLookX = outLookY = 0.0;
    XINPUT_STATE st{};
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        if (XInputGetState(i, &st) == ERROR_SUCCESS) {
            auto apply = [](SHORT raw, SHORT dz) -> double {
                double v = (double)raw;
                double d = (double)dz;
                if (v > d)  return (v - d) / (32767.0 - d);
                if (v < -d) return (v + d) / (32767.0 - d);
                return 0.0;
            };
            outMoveX = apply(st.Gamepad.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
            outMoveY = apply(st.Gamepad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
            outLookX = apply(st.Gamepad.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
            outLookY = apply(st.Gamepad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
            return (outMoveX != 0.0 || outMoveY != 0.0 ||
                    outLookX != 0.0 || outLookY != 0.0);
        }
    }
    return false;
}

// ================= Settings reader (AppliedSettingsWin64.cfg) =================
static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, b - a + 1);
}
static void LoadStalker2Settings() {
    const wchar_t* la = _wgetenv(L"LOCALAPPDATA");
    if (!la) return;
    std::wstring path = std::wstring(la) + L"\\Stalker2\\Saved\\GameSettings\\AppliedSettingsWin64.cfg";
    std::ifstream f(path.c_str());
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq));
        std::string v = trim(line.substr(eq + 1));
        if      (k == "MouseSensitivityCoef")   { try { g_mouseSensCoef.store(std::stod(v)); } catch (...) {} }
        else if (k == "GamepadSensitivityCoef") { try { g_padSensCoef.store(std::stod(v));   } catch (...) {} }
        else if (k == "InvertMouseXAxis")       { g_invertMouseX.store(v == "true"); }
        else if (k == "InvertMouseYAxis")       { g_invertMouseY.store(v == "true"); }
        else if (k == "GamepadInvertXAxis")     { g_invertPadX.store(v == "true"); }
        else if (k == "GamepadInvertYAxis")     { g_invertPadY.store(v == "true"); }
    }
}

// ================= Raw mouse + keyboard WndProc =================
static LRESULT CALLBACK HookedWndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_INPUT) {
        UINT sz = 0;
        GetRawInputData((HRAWINPUT)l, RID_INPUT, nullptr, &sz, sizeof(RAWINPUTHEADER));
        if (sz > 0 && sz <= 1024) {
            BYTE buf[1024];
            if (GetRawInputData((HRAWINPUT)l, RID_INPUT, buf, &sz, sizeof(RAWINPUTHEADER)) == sz) {
                RAWINPUT* ri = reinterpret_cast<RAWINPUT*>(buf);
                if (ri->header.dwType == RIM_TYPEMOUSE &&
                    (ri->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
                    g_dx.fetch_add(ri->data.mouse.lLastX, std::memory_order_relaxed);
                    g_dy.fetch_add(ri->data.mouse.lLastY, std::memory_order_relaxed);
                }
            }
        }
        return CallWindowProc(g_origWndProc, h, msg, w, l);
    }

    // Only swallow W/A/S/D during dialogue so option list doesn't scroll on movement keys.
    // GetAsyncKeyState still sees them so our movement code still works.
    if ((msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP)
        && g_inDialogue.load(std::memory_order_relaxed)) {
        if (w == 'W' || w == 'A' || w == 'S' || w == 'D') return 0;
    }

    // Everything else — including Escape — passes through to the game's native handling.
    return CallWindowProc(g_origWndProc, h, msg, w, l);
}

static BOOL CALLBACK FindGameWindow(HWND h, LPARAM out) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && GetWindow(h, GW_OWNER) == nullptr && IsWindowVisible(h)) {
        *reinterpret_cast<HWND*>(out) = h;
        return FALSE;
    }
    return TRUE;
}

static void SetupInputHook() {
    if (g_rawReady) return;
    HWND h = nullptr;
    EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&h));
    if (!h) return;
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01; rid.usUsage = 0x02; rid.dwFlags = RIDEV_INPUTSINK; rid.hwndTarget = h;
    if (RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        g_origWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtr(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookedWndProc)));
        g_rawReady = true;
    }
}

// ================= The mod =================
class ImmersiveDialogue : public CppUserModBase {
public:
    UObject* m_pawn = nullptr;

    // Tuning constants
    static constexpr double WALK_SCALE      = 0.15;   // fraction of MaxWalkSpeed
    static constexpr double BASE_MOUSE_SENS = 0.10;   // multiplied by in-game MouseSensitivityCoef
    static constexpr double MOUSE_SMOOTH    = 0.5;    // release fraction per frame (1=raw, lower=smoother)
    static constexpr double PAD_LOOK_SCALE  = 0.4;    // multiplied by in-game GamepadSensitivityCoef

    // Mouse smoothing state
    double m_pending_dx = 0.0;
    double m_pending_dy = 0.0;
    bool   m_prevInDialog = false;

    // Footstep audio (best-effort: fire PostEvent on cadence while character moves in dialogue).
    // We enumerate at first dialogue entry — game's anim graph doesn't play walk cycles during
    // dialogue (native C++ path we can't hook via reflection), so foot notifies never fire, so
    // no footstep audio ever plays. We manually trigger Wwise events instead.
    UObject*  m_ftFootstepEvent = nullptr; // UAkAudioEvent* for footstep (all surface + gait variants)
    UObject*  m_ftAkComponent   = nullptr; // UAkComponent* on the pawn
    UFunction* m_ftPostEventFn  = nullptr; // PostAkEvent-family function on the component
    UFunction* m_ftSetSwitchFn  = nullptr; // UAkComponent::SetSwitch(UAkSwitchValue*)
    // Wwise switch value UObjects, keyed by state short-name.
    // Wwise groups: SW_Cutscenes_MovementType (Walk/Run/Sprint), SW_Cutscenes_SurfaceMaterial
    // (Dirt/Grass/Asphalt/...), SW_Cutscenes_ArmorType (Light/Medium/Heavy),
    // SW_Cutscenes_SurfaceWetness (True/False).
    UObject*  m_swWalk    = nullptr;
    UObject*  m_swMedium  = nullptr;
    UObject*  m_swDirt    = nullptr;
    UObject*  m_swDry     = nullptr; // SurfaceWetness "False"
    bool      m_ftProbed = false;
    uint64_t  m_ftNextStepAtMs = 0;
    static constexpr uint64_t FT_STEP_INTERVAL_MS = 450; // ~2.2 steps/sec at walk

    // Attempt to force walk animation via PlayAnimMontage — game locks character into
    // "stand-to-relax-idle" pose during dialogue and native-C++-driven anim graph doesn't
    // pick up walk cycle from AddMovementInput alone.
    UObject*   m_walkMontage      = nullptr;  // UAnimMontage* for walking
    UFunction* m_playMontageFn    = nullptr;  // ACharacter::PlayAnimMontage(UAnimMontage*, float PlayRate, FName StartSection)

    // Camera-centering toggle (F5 flips; persisted to <mod>/config.ini).
    // STALKER 2 uses /Script/Stalker2.CameraModifier_LookAt (a UCameraModifier subclass) to
    // pull the camera back to the NPC and clamp view angles in dialogue. If we call the
    // base class's DisableModifier(true) on every instance each frame, the modifier stops
    // applying its yaw/pitch bias and clamp — freeing the camera.
    // Default: camera-centering OFF (free camera) — the natural fit for immersive dialogue.
    // Config file overrides this on load; if user sets DisableCameraCentering=false in the
    // ini or via MCM, the game default (centering ON) is restored.
    bool       m_camCenteringDisabled = true;       // toggled by F6

    // FOV control (config-only, no hotkey):
    //   DisableDialogueFovChange=true  -> force FOV back to non-dialogue value in dialogue
    //   DialogueFov=<float>            -> if > 0, force FOV to this exact value in dialogue
    //                                     (takes precedence over DisableDialogueFovChange)
    bool       m_disableDialogueFov = false;
    float      m_dialogueFovOverride = 0.0f;
    float      m_cachedNonDialogueFov = 0.0f;       // last FOV we saw outside dialogue
    UObject*   m_pawnCamera = nullptr;              // UCameraComponent on the pawn
    FProperty* m_fovProp = nullptr;                 // FloatProperty "FieldOfView"
    bool       m_configLoaded         = false;
    bool       m_f5Prev               = false; // (name lingering; actually tracks F6)
    bool       m_f7Prev               = false;
    std::vector<UObject*> m_lookAtModifiers;
    UFunction* m_disableModifierFn = nullptr;
    UFunction* m_enableModifierFn  = nullptr;
    uint64_t   m_lastLookAtRescanMs = 0;

    std::wstring ConfigPath() {
        // Config lives next to the mod DLL for MCM-mod parity with other UE4SS mods.
        wchar_t buf[MAX_PATH]; GetModuleFileNameW((HMODULE)GetModuleHandleW(L"main.dll"), buf, MAX_PATH);
        std::wstring p(buf);
        size_t slash = p.find_last_of(L'\\');
        if (slash != std::wstring::npos) p.resize(slash + 1);
        p += L"config.ini";
        return p;
    }
    void LoadConfig() {
        if (m_configLoaded) return;
        m_configLoaded = true;
        std::wstring path = ConfigPath();
        std::ifstream f(path.c_str());
        if (!f) return;
        std::string line;
        while (std::getline(f, line)) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = trim(line.substr(0, eq));
            std::string v = trim(line.substr(eq + 1));
            if      (k == "DisableCameraCentering")     m_camCenteringDisabled = (v == "true" || v == "1");
            else if (k == "DisableDialogueFovChange")   m_disableDialogueFov   = (v == "true" || v == "1");
            else if (k == "DialogueFov")                { try { m_dialogueFovOverride = std::stof(v); } catch(...) {} }
        }
    }
    void SaveConfig() {
        std::wstring path = ConfigPath();
        std::ofstream f(path.c_str(), std::ios::trunc);
        if (!f) return;
        f << "; ImmersiveDialogue config — key/value ini format\n";
        f << "DisableCameraCentering=" << (m_camCenteringDisabled ? "true" : "false") << "\n";
        f << "DisableDialogueFovChange=" << (m_disableDialogueFov ? "true" : "false") << "\n";
        f << "; DialogueFov: 0 = no override, non-zero = force this FOV (deg) in dialogue\n";
        f << "DialogueFov=" << m_dialogueFovOverride << "\n";
    }

    ImmersiveDialogue() {
        ModName        = STR("ImmersiveDialogue");
        ModVersion     = STR("1.0");
        ModAuthors     = STR("Noah");
        ModDescription = STR("Free movement + mouse/pad look during NPC dialogue.");
    }

    std::pair<int,int> m_hookIsInDialog{-1,-1};
    void InstallIsInDialogLieHook() {
        // Post-hook on PC::IsInStaticDialog. Any caller that goes through the reflection
        // system (anim graph, camera modifier, dialogue widget checks) sees FALSE while
        // g_inDialogue is true — so those systems stop applying their in-dialogue behavior.
        // Our own polling (marked via thread_local tl_selfDialogueQuery) gets the truth.
        UnrealScriptFunctionCallable postLie =
            [](UnrealScriptFunctionCallableContext& ctx, void*) {
                if (tl_selfDialogueQuery) return; // our own probe: pass through real return
                if (g_inDialogue.load(std::memory_order_relaxed)) {
                    ctx.SetReturnValue<bool>(false);
                }
            };
        m_hookIsInDialog = UObjectGlobals::RegisterHook(
            StringType(STR("/Script/Stalker2.PC:IsInStaticDialog")),
            UnrealScriptFunctionCallable{}, postLie, nullptr);
    }

    auto on_unreal_init() -> void override {
        SetupInputHook();
        InstallXInputHook();
        InstallIsInDialogLieHook();
        LoadStalker2Settings();
        LoadConfig();
        Output::send<LogLevel::Verbose>(
            STR("[ImmDlg] unreal init v{} (mouse={}, xinput={}, mouseSens={}, padSens={}, invertY={})\n"),
            ModVersion,
            g_rawReady     ? STR("ok") : STR("FAILED"),
            g_xinputHooked ? STR("ok") : STR("SKIPPED"),
            g_mouseSensCoef.load(),
            g_padSensCoef.load(),
            g_invertMouseY.load() ? STR("true") : STR("false"));
    }

    UObject* GetPawn() {
        if (m_pawn && m_pawn->IsUnreachable() == false) return m_pawn;
        m_pawn = UObjectGlobals::FindFirstOf(STR("PC"));
        return m_pawn;
    }

    static UFunction* Fn(UObject* o, const wchar_t* name) {
        return o->GetFunctionByNameInChain(FName(name));
    }
    bool CallBool(UObject* o, const wchar_t* name) {
        UFunction* fn = Fn(o, name); if (!fn) return false;
        struct { bool ReturnValue = false; } p;
        o->ProcessEvent(fn, &p);
        return p.ReturnValue;
    }
    FRotatorD ControlRotation(UObject* o) {
        FRotatorD out{0,0,0};
        UFunction* fn = Fn(o, STR("GetControlRotation")); if (!fn) return out;
        struct { FRotatorD ReturnValue; } p{{0,0,0}};
        o->ProcessEvent(fn, &p);
        return p.ReturnValue;
    }
    void ResetIgnore(UObject* o) {
        if (UFunction* f = Fn(o, STR("ResetIgnoreMoveInput"))) { char none[1]; o->ProcessEvent(f, none); }
        if (UFunction* f = Fn(o, STR("ResetIgnoreLookInput"))) { char none[1]; o->ProcessEvent(f, none); }
    }
    void AddMovement(UObject* o, double x, double y, float scale) {
        UFunction* fn = Fn(o, STR("AddMovementInput")); if (!fn) return;
        struct { FVectorD WorldDirection; float ScaleValue; bool bForce; } p{{x,y,0.0}, scale, false};
        o->ProcessEvent(fn, &p);
    }
    void AddYaw(UObject* o, float v) {
        UFunction* fn = Fn(o, STR("AddControllerYawInput")); if (!fn) return;
        struct { float Val; } p{v};
        o->ProcessEvent(fn, &p);
    }
    void AddPitch(UObject* o, float v) {
        UFunction* fn = Fn(o, STR("AddControllerPitchInput")); if (!fn) return;
        struct { float Val; } p{v};
        o->ProcessEvent(fn, &p);
    }
    // PC::set_move_vector(FVector) — the UFUNCTION the game's own input pipeline calls
    // to feed WASD/stick into the movement + animation system. In dialogue, this pipeline
    // is gated, so strafe animations receive zero input even though we're calling
    // AddMovementInput directly. Calling set_move_vector ourselves restores the natural
    // anim/body-rotation behavior (body orients to movement, forward-walk anim plays).
    void SetMoveVector(UObject* o, double x, double y, double z) {
        UFunction* fn = Fn(o, STR("SetMoveVector"));
        if (!fn) fn = Fn(o, STR("set_move_vector"));
        if (!fn) return;
        struct { FVectorD MoveVector; } p{{x, y, z}};
        o->ProcessEvent(fn, &p);
    }

    // PC::set_allowed_movement_types(PlayerMovementType) — lifts any dialogue-imposed
    // movement lock (in-dialogue the game sets this to a restricted subset). Passing ALL
    // (highest enum value; if it's a bitmask, 0xFF covers everything).
    void SetAllowedMovementTypes(UObject* pc, uint8_t types) {
        UFunction* fn = Fn(pc, STR("SetAllowedMovementTypes"));
        if (!fn) fn = Fn(pc, STR("set_allowed_movement_types"));
        if (!fn) return;
        struct { uint8_t T; } p{types};
        pc->ProcessEvent(fn, &p);
    }
    void SetForbiddenMovementTypes(UObject* pc, uint8_t types) {
        UFunction* fn = Fn(pc, STR("SetForbiddenMovementTypes"));
        if (!fn) fn = Fn(pc, STR("set_forbidden_movement_types"));
        if (!fn) return;
        struct { uint8_t T; } p{types};
        pc->ProcessEvent(fn, &p);
    }
    void DisableCinematicMode(UObject* pc) {
        UFunction* fn = Fn(pc, STR("DisableCinematicMode"));
        if (!fn) fn = Fn(pc, STR("disable_cinematic_mode"));
        if (!fn) return;
        char none[1]; pc->ProcessEvent(fn, none);
    }

    // PC::stop_dialog_gesture — kills the "dialog gesture" the game plays when talking.
    // If the gesture is a montage that overrides locomotion, this should let strafe
    // blends surface through. Zero-arg UFUNCTION.
    void StopDialogGesture(UObject* pc) {
        UFunction* fn = Fn(pc, STR("stop_dialog_gesture"));
        if (!fn) fn = Fn(pc, STR("StopDialogGesture"));
        if (!fn) return;
        char none[1]; pc->ProcessEvent(fn, none);
    }

    // UAnimInstance::Montage_Stop(float InBlendOutTime, UAnimMontage* Montage=nullptr).
    // Passing null Montage stops the currently-active montage. Called blindly every
    // frame in dialogue to test whether a masked montage is what's overriding strafe.
    void MontageStopAll(UObject* animInstance) {
        if (!animInstance) return;
        UFunction* fn = Fn(animInstance, STR("Montage_Stop"));
        if (!fn) return;
        struct { float InBlendOutTime; UObject* Montage; } p{0.0f, nullptr};
        animInstance->ProcessEvent(fn, &p);
    }

    // USceneComponent::K2_SetRelativeRotation(FRotator, bool bSweep, FHitResult, ETeleportType) → bool.
    // We only need to write the rotation for a mesh subcomponent. Layout for the params:
    //   FRotatorD NewRotation (24 bytes)
    //   bool bSweep (1 byte, padded)
    //   FHitResult SweepHitResult (~100 bytes) — we don't care, pass zeroed buffer
    //   ETeleportType Teleport (1 byte)
    //   bool ReturnValue
    // Rather than model FHitResult exactly, we pass a big zero buffer sized generously.
    void SetMeshRelativeYaw(UObject* meshComp, double yawDeg) {
        if (!meshComp) return;
        UFunction* fn = Fn(meshComp, STR("K2_SetRelativeRotation"));
        if (!fn) return;
        struct { FRotatorD NewRotation; bool bSweep; uint8_t pad[7]; uint8_t hit[200]; uint8_t teleport; uint8_t pad2[7]; bool ReturnValue; } p{};
        p.NewRotation = {0.0, yawDeg, 0.0};
        p.bSweep = false;
        p.teleport = 0;
        meshComp->ProcessEvent(fn, &p);
    }

    // Scan every loaded UObject once to find: a footstep AkAudioEvent + an AkComponent on
    // our pawn's actor hierarchy + a PostAkEvent / PostEvent UFunction to call. Log what's
    // available so we can iterate if the first combination doesn't fire audio.
    void ProbeFootstepAudio(UObject* pawn) {
        if (m_ftProbed) return;
        m_ftProbed = true;

        // Get pawn's full path prefix so we can identify components owned by it.
        StringType pawnFull = pawn->GetFullName();
        // Full name format: "ClassName /Path/To/Pawn". Strip class prefix for outer-match.
        size_t sp = pawnFull.find(STR(' '));
        StringType pawnPath = (sp != StringType::npos) ? pawnFull.substr(sp + 1) : pawnFull;

        int scanned = 0;
        int akEventsLogged = 0;
        int akComponentsLogged = 0;
        UObject* firstFootstepEvent = nullptr;
        UObject* pawnAkComponent    = nullptr;
        UObject* walkMontage        = nullptr;

        UObjectGlobals::ForEachUObject([&](UObject* obj, int32_t, int32_t) -> LoopAction {
            scanned++;
            if (!obj) return LoopAction::Continue;
            StringType full = obj->GetFullName();
            StringType name = obj->GetName();

            // AkAudioEvent instances with "Footstep" in the name — candidates for the sound.
            if (full.find(STR("AkAudioEvent ")) == 0 && full.find(STR("Footstep")) != StringType::npos) {
                if (akEventsLogged < 20) {
                    Output::send<LogLevel::Verbose>(STR("[ImmDlg]   footstep evt: {}\n"), full);
                    akEventsLogged++;
                }
                if (!firstFootstepEvent) firstFootstepEvent = obj;
            }

            // AkComponent instances whose full path starts with our pawn's path — pawn's audio component.
            if (full.find(STR("AkComponent ")) == 0 || full.find(STR("AkSubmixInputComponent ")) == 0) {
                if (akComponentsLogged < 20 && full.find(pawnPath) != StringType::npos) {
                    Output::send<LogLevel::Verbose>(STR("[ImmDlg]   pawn ak component: {}\n"), full);
                    akComponentsLogged++;
                    if (!pawnAkComponent) pawnAkComponent = obj;
                }
            }

            // Wwise switch values (UAkSwitchValue) — need pointers for SetSwitch(UAkSwitchValue*).
            // The class is generated per-switch by Wwise and shows as "AkSwitchValue".
            if (full.find(STR("AkSwitchValue ")) == 0) {
                if (!m_swWalk   && name == StringType(STR("Walk")))   m_swWalk   = obj;
                if (!m_swMedium && name == StringType(STR("Medium"))) m_swMedium = obj;
                if (!m_swDirt   && name == StringType(STR("Dirt")))   m_swDirt   = obj;
                if (!m_swDry    && name == StringType(STR("False")))  m_swDry    = obj;
            }

            // AnimMontage for the player walk cycle. STALKER 2 tends to name these
            // AM_Player_Walk or similar. First hit wins.
            if (!walkMontage && full.find(STR("AnimMontage ")) == 0) {
                bool matchesWalkPattern =
                    (name.find(STR("Walk")) != StringType::npos ||
                     name.find(STR("walk")) != StringType::npos) &&
                    (full.find(STR("Skif"))   != StringType::npos ||
                     full.find(STR("Player")) != StringType::npos ||
                     full.find(STR("BP_PC"))  != StringType::npos);
                if (matchesWalkPattern) {
                    walkMontage = obj;
                    Output::send<LogLevel::Verbose>(STR("[ImmDlg]   walk montage candidate: {}\n"), full);
                }
            }

            return LoopAction::Continue;
        });

        m_ftFootstepEvent = firstFootstepEvent;
        m_ftAkComponent   = pawnAkComponent;
        m_walkMontage     = walkMontage;

        // ACharacter::PlayAnimMontage UFUNCTION (K2_PlayAnimMontage in some builds).
        if (m_walkMontage) {
            const wchar_t* montageFnNames[] = { STR("PlayAnimMontage"), STR("K2_PlayAnimMontage") };
            for (auto* fn_name : montageFnNames) {
                if (UFunction* fn = pawn->GetFunctionByNameInChain(FName(fn_name))) {
                    m_playMontageFn = fn;
                    Output::send<LogLevel::Verbose>(STR("[ImmDlg] walk-montage play fn: {}\n"), fn_name);
                    break;
                }
            }
        }
        Output::send<LogLevel::Verbose>(
            STR("[ImmDlg] switch values: Walk={}, Medium={}, Dirt={}, Dry={}\n"),
            m_swWalk   ? STR("ok") : STR("MISSING"),
            m_swMedium ? STR("ok") : STR("MISSING"),
            m_swDirt   ? STR("ok") : STR("MISSING"),
            m_swDry    ? STR("ok") : STR("MISSING"));

        // Resolve a PostEvent function on the pawn's AkComponent (name varies by Wwise plugin version).
        if (m_ftAkComponent) {
            const wchar_t* names[] = {
                STR("PostAkEvent"),
                STR("PostAssociatedAkEvent"),
                STR("PostEvent"),
                STR("PostEventByName"),
            };
            for (auto* n : names) {
                if (UFunction* fn = m_ftAkComponent->GetFunctionByNameInChain(FName(n))) {
                    m_ftPostEventFn = fn;
                    Output::send<LogLevel::Verbose>(STR("[ImmDlg] footstep post-fn on ak component: {}\n"), n);
                    break;
                }
            }
            // SetSwitch UFUNCTION for setting Wwise switches (MovementType, SurfaceMaterial).
            m_ftSetSwitchFn = m_ftAkComponent->GetFunctionByNameInChain(FName(STR("SetSwitch")));
            Output::send<LogLevel::Verbose>(STR("[ImmDlg] footstep set-switch fn: {}\n"),
                                             m_ftSetSwitchFn ? STR("found") : STR("MISSING"));
        }

        Output::send<LogLevel::Verbose>(
            STR("[ImmDlg] footstep probe: scanned={}, events(Footstep)={}, pawnComps={}, event={}, comp={}, postFn={}\n"),
            scanned, akEventsLogged, akComponentsLogged,
            m_ftFootstepEvent ? STR("FOUND") : STR("MISSING"),
            m_ftAkComponent   ? STR("FOUND") : STR("MISSING"),
            m_ftPostEventFn   ? STR("FOUND") : STR("MISSING"));
    }

    // Call UAkComponent::SetSwitch(UAkSwitchValue* SwitchValue) via reflection.
    void SetAkSwitch(UObject* switchValue) {
        if (!m_ftSetSwitchFn || !m_ftAkComponent || !switchValue) return;
        struct { UObject* SwitchValue; } p{ switchValue };
        m_ftAkComponent->ProcessEvent(m_ftSetSwitchFn, &p);
    }

    // Diagnostic: enumerate every UObject whose class inherits from UAnimInstance AND
    // whose full name path contains our pawn's path. Reveals sub/linked/post-process
    // anim instances beyond the main + shadow we already track. Prints each hit's class
    // name + full name so we can find the one actually driving the visible dialogue pose.
    void ProbeAllAnimInstances(UObject* pawn) {
        if (!pawn) return;
        StringType pawnFull = pawn->GetFullName();
        size_t sp = pawnFull.find(STR(' '));
        StringType pawnPath = (sp != StringType::npos) ? pawnFull.substr(sp + 1) : pawnFull;
        int hits = 0;
        UObjectGlobals::ForEachUObject([&](UObject* obj, int32_t, int32_t) -> LoopAction {
            if (!obj) return LoopAction::Continue;
            UClass* cls = obj->GetClassPrivate();
            if (!cls) return LoopAction::Continue;
            // Walk class chain looking for AnimInstance.
            bool isAnimInst = false;
            for (UStruct* w = cls; w; w = w->GetSuperStruct()) {
                if (w->GetName() == StringType(STR("AnimInstance"))) { isAnimInst = true; break; }
            }
            if (!isAnimInst) return LoopAction::Continue;
            StringType full = obj->GetFullName();
            // Only interested in ones tied to our pawn.
            if (full.find(pawnPath) == StringType::npos) return LoopAction::Continue;
            Output::send<LogLevel::Verbose>(
                STR("[ImmDlg] ANIM-INST hit: class={} full={}\n"),
                cls->GetName(), full);
            hits++;
            return LoopAction::Continue;
        });
        Output::send<LogLevel::Verbose>(STR("[ImmDlg] ANIM-INST probe: {} anim instances tied to pawn\n"), hits);
    }

    // One-shot: dump every UPROPERTY on the main AnimInstance's own class (top level,
    // not inside the sub-structs we already know about). Any bool/enum/float here could
    // be the "in dialogue" gate that switches the anim graph off strafe blends.
    bool m_mainAnimPropsDumped = false;
    void DumpMainAnimInstanceProps() {
        if (m_mainAnimPropsDumped) return;
        if (!m_animInstance) return;
        m_mainAnimPropsDumped = true;
        UClass* cls = m_animInstance->GetClassPrivate();
        if (!cls) return;
        Output::send<LogLevel::Verbose>(STR("[ImmDlg] MAIN-AI class props (class={}):\n"), cls->GetName());
        int n = 0;
        for (UStruct* w = cls; w && n < 250; w = w->GetSuperStruct()) {
            for (FProperty* p : TFieldRange<FProperty>(w, EFieldIterationFlags::None)) {
                if (!p || n >= 250) break;
                Output::send<LogLevel::Verbose>(STR("[ImmDlg]   MAIN prop: {}\n"), p->GetName());
                n++;
            }
        }
    }


    // Footstep timer is now DISABLED — with the walk animation actually playing
    // (state_data.bWalkingOverride writes finally lit it up), the game's own foot-plant
    // anim notifies fire native footsteps at correct cadence + volume. Our fixed 450ms
    // timer would only double them and cause the "sound doesn't line up" symptom.
    // The probe + Ak component lookup stays (harmless), just the fire path skipped.
    void MaybeFireFootstep(bool /*moving*/) {
        // no-op
    }

    // Walking anim in dialogue: attack via direct AnimInstance UPROPERTY writes.
    // Path: pawn -> GetMesh (USkeletalMeshComponent) -> GetAnimInstance (UAnimInstance).
    // We probe for bool UPROPERTYs whose name hints at dialogue-idle state and either
    // force them false (idle-lock) or true (walking) each frame in dialogue.
    UObject* m_pawnMesh         = nullptr;
    UObject* m_animInstance     = nullptr;
    bool     m_animProbed       = false;
    // Candidate UPROPERTYs found — we write in bulk each tick while moving in dialogue.
    struct AnimBoolProp {
        FProperty* prop = nullptr;
        bool valueToForce = false; // false to override "in dialogue" flags, true to override "walking" flags
        const wchar_t* name = nullptr; // for logging
    };
    std::vector<AnimBoolProp> m_animBoolProps;

    void ResolvePawnAnimInstance(UObject* pawn) {
        if (m_animProbed) return;
        m_animProbed = true;

        // Prefer direct UPROPERTY read (ACharacter::Mesh) — GetMesh UFUNCTION isn't always
        // exposed. Fall back to a couple UFUNCTION name variants.
        {
            const wchar_t* meshPropNames[] = { STR("Mesh"), STR("SkeletalMesh"), STR("BodyMesh") };
            for (auto* n : meshPropNames) {
                if (FProperty* p = pawn->GetPropertyByNameInChain(n)) {
                    UObject** slot = p->ContainerPtrToValuePtr<UObject*>(pawn);
                    if (slot && *slot) { m_pawnMesh = *slot; break; }
                }
            }
        }
        if (!m_pawnMesh) {
            const wchar_t* meshFnNames[] = { STR("GetMesh"), STR("K2_GetMesh"), STR("GetSkeletalMeshComponent") };
            for (auto* n : meshFnNames) {
                if (UFunction* fn = pawn->GetFunctionByNameInChain(FName(n))) {
                    struct { UObject* Ret; } p{nullptr};
                    pawn->ProcessEvent(fn, &p);
                    if (p.Ret) { m_pawnMesh = p.Ret; break; }
                }
            }
        }
        if (!m_pawnMesh) {
            Output::send<LogLevel::Verbose>(STR("[ImmDlg] anim probe: pawn mesh not found (tried Mesh/SkeletalMesh/BodyMesh props + GetMesh/K2_GetMesh/GetSkeletalMeshComponent fns)\n"));
            return;
        }
        Output::send<LogLevel::Verbose>(STR("[ImmDlg] anim probe: mesh found -> {}\n"), m_pawnMesh->GetFullName());
        UFunction* getAnimFn = m_pawnMesh->GetFunctionByNameInChain(FName(STR("GetAnimInstance")));
        if (!getAnimFn) getAnimFn = m_pawnMesh->GetFunctionByNameInChain(FName(STR("K2_GetAnimInstance")));
        if (getAnimFn) {
            struct { UObject* Ret; } p{nullptr};
            m_pawnMesh->ProcessEvent(getAnimFn, &p);
            m_animInstance = p.Ret;
        }
        if (!m_animInstance) {
            // Fall back to AnimScriptInstance property on the mesh component.
            if (FProperty* p = m_pawnMesh->GetPropertyByNameInChain(STR("AnimScriptInstance"))) {
                UObject** slot = p->ContainerPtrToValuePtr<UObject*>(m_pawnMesh);
                if (slot) m_animInstance = *slot;
            }
        }
        if (!m_animInstance) {
            Output::send<LogLevel::Verbose>(STR("[ImmDlg] anim probe: AnimInstance not found\n"));
            return;
        }
        Output::send<LogLevel::Verbose>(STR("[ImmDlg] anim probe: AnimInstance found -> {}\n"), m_animInstance->GetFullName());

        // Candidate names + intended force value:
        //   force FALSE for anything that says "in dialogue" / "relax idle"
        //   force TRUE  for anything that says "walking" / "moving"
        struct Cand { const wchar_t* name; bool val; };
        Cand candidates[] = {
            // Dialogue / relax-idle overrides (force false)
            { STR("bInDialogue"),          false },
            { STR("bIsInDialogue"),        false },
            { STR("bIsInStaticDialog"),    false },
            { STR("bIsInStaticDialogue"),  false },
            { STR("bDialogueMode"),        false },
            { STR("bStandToRelaxIdle"),    false },
            { STR("bIsStandToRelaxIdle"),  false },
            { STR("bIsRelaxIdle"),         false },
            { STR("bInRelaxIdle"),         false },
            { STR("bRelaxIdle"),           false },
            { STR("StandToRelaxIdle"),     false },
            { STR("IsInStaticDialog"),     false },
            // Walking flags (force true when moving)
            { STR("bIsWalking"),           true  },
            { STR("bIsMoving"),            true  },
            { STR("bHasMovementInput"),    true  },
            { STR("bWalking"),             true  },
            { STR("bMoving"),              true  },
        };
        for (auto& c : candidates) {
            if (FProperty* p = m_animInstance->GetPropertyByNameInChain(c.name)) {
                m_animBoolProps.push_back({p, c.val, c.name});
                Output::send<LogLevel::Verbose>(STR("[ImmDlg]   anim prop: {} -> force {}\n"),
                                                 c.name, c.val ? STR("true") : STR("false"));
            }
        }
        // Look up the STRUCT properties directly — these are named `dialog_data`,
        // `state_data`, `locomotion_data` on AnimInstancePlayer. Inside dialog_data is a
        // single `dialog` bool (offset 0). If we set that to false in dialogue, the anim
        // graph sees "not in dialogue" and walks normal locomotion.
        m_dialogDataProp = m_animInstance->GetPropertyByNameInChain(STR("dialog_data"));
        if (!m_dialogDataProp) m_dialogDataProp = m_animInstance->GetPropertyByNameInChain(STR("DialogData"));
        Output::send<LogLevel::Verbose>(STR("[ImmDlg]   dialog_data struct prop: {}\n"),
                                         m_dialogDataProp ? STR("found") : STR("MISSING"));

        // Walk state_data struct for override bool offsets.
        ResolveStateDataOffsets();

        Output::send<LogLevel::Verbose>(
            STR("[ImmDlg] anim probe: mesh={}, animInst={}, boolProps={}, dialogStruct={}, stateStruct={}\n"),
            m_pawnMesh ? STR("ok") : STR("null"),
            m_animInstance ? STR("ok") : STR("null"),
            (int)m_animBoolProps.size(),
            m_dialogDataProp ? STR("ok") : STR("null"),
            m_stateDataProp ? STR("ok") : STR("null"));
    }

    // Character rotation control. On ACharacter/APawn:
    //   - bUseControllerRotationYaw (on pawn root): if true, pawn yaw is force-synced to
    //     controller yaw every tick — fights any manual rotation.
    //   - CharacterMovement.bOrientRotationToMovement: if true, CMC rotates pawn yaw toward
    //     velocity direction each tick (this is what makes STALKER 2's strafe/diagonal
    //     walk look natural outside dialogue).
    // In dialogue the game likely sets bUseControllerRotationYaw=true + orient-to-movement=false
    // (body locked to camera) — we override to the opposite so the game's natural strafe
    // behavior kicks in.
    UObject*   m_charMoveComp = nullptr;
    FProperty* m_propUseCtrlYaw = nullptr;   // on pawn — bool
    FProperty* m_propOrientToMove = nullptr; // on CMC   — bool
    bool m_rotationCtrlResolved = false;

    void ResolveRotationControl(UObject* pawn) {
        if (m_rotationCtrlResolved) return;
        m_rotationCtrlResolved = true;
        // bUseControllerRotationYaw is on APawn directly.
        m_propUseCtrlYaw = pawn->GetPropertyByNameInChain(STR("bUseControllerRotationYaw"));
        // CharacterMovement UPROPERTY on ACharacter → UCharacterMovementComponent.
        const wchar_t* cmcNames[] = { STR("CharacterMovement"), STR("character_movement"), STR("MovementComponent") };
        for (auto* n : cmcNames) {
            if (FProperty* p = pawn->GetPropertyByNameInChain(n)) {
                UObject** slot = p->ContainerPtrToValuePtr<UObject*>(pawn);
                if (slot && *slot) { m_charMoveComp = *slot; break; }
            }
        }
        if (m_charMoveComp) {
            m_propOrientToMove = m_charMoveComp->GetPropertyByNameInChain(STR("bOrientRotationToMovement"));
        }
        Output::send<LogLevel::Verbose>(
            STR("[ImmDlg] rotation control resolved: bUseCtrlYaw prop={}, CMC={}, bOrientToMove prop={}\n"),
            m_propUseCtrlYaw ? STR("ok") : STR("null"),
            m_charMoveComp   ? STR("ok") : STR("null"),
            m_propOrientToMove ? STR("ok") : STR("null"));
    }

    // In dialogue: flip pawn's rotation control so body follows movement direction (like
    // it does outside dialogue) instead of being locked to camera yaw.
    void ApplyDialogueRotationControl(UObject* pawn) {
        if (m_propUseCtrlYaw) {
            bool* slot = m_propUseCtrlYaw->ContainerPtrToValuePtr<bool>(pawn);
            if (slot) *slot = false;
        }
        if (m_propOrientToMove && m_charMoveComp) {
            bool* slot = m_propOrientToMove->ContainerPtrToValuePtr<bool>(m_charMoveComp);
            if (slot) *slot = true;
        }
    }
    void RestoreOutsideDialogueRotationControl(UObject* pawn) {
        // Restore the defaults the game expects outside dialogue: body follows camera,
        // no orient-to-movement (STALKER 2 is FPS, camera IS the body yaw).
        if (m_propUseCtrlYaw) {
            bool* slot = m_propUseCtrlYaw->ContainerPtrToValuePtr<bool>(pawn);
            if (slot) *slot = true;
        }
        if (m_propOrientToMove && m_charMoveComp) {
            bool* slot = m_propOrientToMove->ContainerPtrToValuePtr<bool>(m_charMoveComp);
            if (slot) *slot = false;
        }
    }

    // Once-per-500ms diagnostic: log the dialogue-suspicion levers we can read on the pawn,
    // so we can see what the game changes when entering dialogue.
    uint64_t m_lastPcDiagMs = 0;
    void LogPcState(UObject* pawn, bool inDlg) {
        if (!pawn) return;
        uint64_t now = GetTickCount64();
        if (now - m_lastPcDiagMs < 500) return;
        m_lastPcDiagMs = now;
        auto readU8  = [&](const wchar_t* n) -> int {
            FProperty* p = pawn->GetPropertyByNameInChain(n); if (!p) return -1;
            uint8_t* s = p->ContainerPtrToValuePtr<uint8_t>(pawn); return s ? (int)*s : -1;
        };
        auto readI32 = [&](const wchar_t* n) -> int {
            FProperty* p = pawn->GetPropertyByNameInChain(n); if (!p) return -1;
            int32_t* s = p->ContainerPtrToValuePtr<int32_t>(pawn); return s ? *s : -1;
        };
        auto readBool = [&](const wchar_t* n) -> int {
            FProperty* p = pawn->GetPropertyByNameInChain(n); if (!p) return -1;
            bool* s = p->ContainerPtrToValuePtr<bool>(pawn); return s ? (*s ? 1 : 0) : -1;
        };
        int amt = readU8(STR("allowed_movement_actions"));
        int cmc = readI32(STR("cinematic_mode_counter"));
        int cs  = readBool(STR("cinematic_sequence"));
        int useCtrlYaw = readBool(STR("bUseControllerRotationYaw"));
        int immob = readBool(STR("immobilized"));
        int ctxAct = readBool(STR("contextual_action"));
        int orientMove = -1;
        if (m_charMoveComp && m_propOrientToMove) {
            bool* s = m_propOrientToMove->ContainerPtrToValuePtr<bool>(m_charMoveComp);
            if (s) orientMove = *s ? 1 : 0;
        }
        Output::send<LogLevel::Verbose>(
            STR("[ImmDlg] PC inDlg={} | allowedMove={} cineCntr={} cineSeq={} useCtrlYaw={} orientToMove={} immob={} ctxAct={}\n"),
            inDlg ? 1 : 0, amt, cmc, cs, useCtrlYaw, orientMove, immob, ctxAct);

        // Log which AnimInstance is currently sitting on CharacterMesh0.AnimScriptInstance
        // in AND out of dialogue. If it swaps (e.g. Player_C -> dummy_C on dialogue entry),
        // we've found how strafe gets disabled.
        if (m_pawnMesh) {
            FProperty* aip = m_pawnMesh->GetPropertyByNameInChain(STR("AnimScriptInstance"));
            if (aip) {
                UObject** slot = aip->ContainerPtrToValuePtr<UObject*>(m_pawnMesh);
                UObject* curAI = (slot ? *slot : nullptr);
                UClass* aicls = curAI ? curAI->GetClassPrivate() : nullptr;
                Output::send<LogLevel::Verbose>(
                    STR("[ImmDlg] MESH-AI inDlg={} AnimScriptInstance class={}\n"),
                    inDlg ? 1 : 0,
                    aicls ? aicls->GetName() : StringType(STR("(null)")));
            }
        }

        // Log dummy_animation / dummy_blueprint values on the main AnimInstance. These are
        // Read-Write props on AnimInstanceBase. If dummy_animation is a walk-forward asset
        // that displays in dialogue, we've found the source of forward-only visibility.
        if (m_animInstance) {
            FProperty* da = m_animInstance->GetPropertyByNameInChain(STR("dummy_animation"));
            FProperty* db = m_animInstance->GetPropertyByNameInChain(STR("dummy_blueprint"));
            UObject* daVal = nullptr;
            UObject* dbVal = nullptr;
            if (da) { UObject** s = da->ContainerPtrToValuePtr<UObject*>(m_animInstance); if (s) daVal = *s; }
            if (db) { UObject** s = db->ContainerPtrToValuePtr<UObject*>(m_animInstance); if (s) dbVal = *s; }
            Output::send<LogLevel::Verbose>(
                STR("[ImmDlg] MAIN-AI inDlg={} dummy_animation={} dummy_blueprint={}\n"),
                inDlg ? 1 : 0,
                daVal ? daVal->GetFullName() : StringType(STR("(null)")),
                dbVal ? dbVal->GetFullName() : StringType(STR("(null)")));
        }
    }

    // Shadow chain: PC has `shadow_mesh_component` (a SkeletalMeshComponent) with its OWN
    // AnimInstance (AnimInstancePlayerShadow), which has its own `state_data` struct. Writes
    // to the main mesh's AnimInstance state_data don't reach the shadow's, so we need to
    // resolve + drive it separately for the shadow to visibly walk.
    UObject* m_shadowMeshComp = nullptr;
    UObject* m_shadowAnimInstance = nullptr;
    FProperty* m_shadowStateProp = nullptr;
    std::map<StringType, int32_t> m_shadowStateOffs;

    // BH chain: the main mesh has a linked/sub anim instance `AnimBP_player_bh_C` in
    // addition to the top-level `AnimBP_Player_C`. "bh" = body handler / locomotion layer.
    // Discovered via ANIM-INST probe: writes to the top-level instance don't reach the
    // linked layer, so directional locomotion data (Direction, BPDirection, gait) never
    // affects the visible pose in dialogue — matching the observed forward-only symptom.
    UObject* m_bhAnimInstance = nullptr;
    FProperty* m_bhStateProp = nullptr;
    FProperty* m_bhLocoProp = nullptr;
    std::map<StringType, int32_t> m_bhStateOffs;
    std::map<StringType, int32_t> m_bhLocoOffs;
    bool m_bhTriedResolve = false;

    void ResolveShadowChain(UObject* pawn) {
        if (m_shadowAnimInstance) return;
        if (!pawn) return;
        // pawn.shadow_mesh_component (ObjectProperty → SkeletalMeshComponent).
        FProperty* smcProp = pawn->GetPropertyByNameInChain(STR("shadow_mesh_component"));
        if (!smcProp) smcProp = pawn->GetPropertyByNameInChain(STR("ShadowMeshComponent"));
        if (!smcProp) {
            Output::send<LogLevel::Verbose>(STR("[ImmDlg] shadow probe: shadow_mesh_component prop MISSING on pawn\n"));
            return;
        }
        UObject** smcSlot = smcProp->ContainerPtrToValuePtr<UObject*>(pawn);
        if (!smcSlot || !*smcSlot) return;
        m_shadowMeshComp = *smcSlot;
        // Get AnimInstance on the shadow mesh (via GetAnimInstance UFUNCTION or AnimScriptInstance UPROPERTY).
        if (UFunction* getAI = m_shadowMeshComp->GetFunctionByNameInChain(FName(STR("GetAnimInstance")))) {
            struct { UObject* Ret; } p{nullptr};
            m_shadowMeshComp->ProcessEvent(getAI, &p);
            m_shadowAnimInstance = p.Ret;
        }
        if (!m_shadowAnimInstance) {
            if (FProperty* aip = m_shadowMeshComp->GetPropertyByNameInChain(STR("AnimScriptInstance"))) {
                UObject** slot = aip->ContainerPtrToValuePtr<UObject*>(m_shadowMeshComp);
                if (slot) m_shadowAnimInstance = *slot;
            }
        }
        if (!m_shadowAnimInstance) {
            Output::send<LogLevel::Verbose>(STR("[ImmDlg] shadow probe: AnimInstance not found on shadow mesh\n"));
            return;
        }
        // state_data offsets on the shadow AnimInstance.
        const wchar_t* sdNames[] = { STR("state_data"), STR("StateData") };
        FProperty* sp = nullptr;
        for (auto* n : sdNames) { sp = m_shadowAnimInstance->GetPropertyByNameInChain(n); if (sp) break; }
        if (!sp) {
            Output::send<LogLevel::Verbose>(STR("[ImmDlg] shadow probe: state_data prop MISSING on shadow AnimInstance\n"));
            return;
        }
        m_shadowStateProp = sp;
        FStructProperty* sfp = static_cast<FStructProperty*>(sp);
        UScriptStruct* stru = sfp->GetStruct();
        if (!stru) return;
        UStruct* walker = stru;
        while (walker) {
            for (FProperty* p : TFieldRange<FProperty>(walker, EFieldIterationFlags::None)) {
                if (p) m_shadowStateOffs[p->GetName()] = p->GetOffset_ForInternal();
            }
            walker = walker->GetSuperStruct();
        }
        Output::send<LogLevel::Verbose>(
            STR("[ImmDlg] shadow chain resolved: shadowAI={}, state_data props={}\n"),
            m_shadowAnimInstance->GetFullName(), (int)m_shadowStateOffs.size());
    }

    // Locate AnimBP_player_bh_C on our pawn's mesh hierarchy and cache its state_data +
    // locomotion_data struct offsets. This is a linked/sub AnimInstance living on the SAME
    // main mesh as AnimBP_Player_C — writes to the top-level instance don't reach it.
    void ResolveBhChain(UObject* pawn) {
        if (m_bhTriedResolve || !pawn) return;
        m_bhTriedResolve = true;
        StringType pawnFull = pawn->GetFullName();
        size_t sp = pawnFull.find(STR(' '));
        StringType pawnPath = (sp != StringType::npos) ? pawnFull.substr(sp + 1) : pawnFull;
        UObject* found = nullptr;
        UObjectGlobals::ForEachUObject([&](UObject* obj, int32_t, int32_t) -> LoopAction {
            if (!obj || found) return LoopAction::Continue;
            UClass* cls = obj->GetClassPrivate();
            if (!cls) return LoopAction::Continue;
            if (cls->GetName() != StringType(STR("AnimBP_player_bh_C"))) return LoopAction::Continue;
            StringType full = obj->GetFullName();
            if (full.find(pawnPath) == StringType::npos) return LoopAction::Continue;
            found = obj;
            return LoopAction::Continue;
        });
        if (!found) {
            Output::send<LogLevel::Verbose>(STR("[ImmDlg] BH probe: AnimBP_player_bh_C NOT FOUND for pawn\n"));
            return;
        }
        m_bhAnimInstance = found;
        // Same struct names as main AnimInstance.
        const wchar_t* sdNames[] = { STR("state_data"), STR("StateData") };
        for (auto* n : sdNames) { m_bhStateProp = m_bhAnimInstance->GetPropertyByNameInChain(n); if (m_bhStateProp) break; }
        const wchar_t* ldNames[] = { STR("locomotion_data"), STR("LocomotionData") };
        for (auto* n : ldNames) { m_bhLocoProp = m_bhAnimInstance->GetPropertyByNameInChain(n); if (m_bhLocoProp) break; }
        if (m_bhStateProp) {
            FStructProperty* sfp = static_cast<FStructProperty*>(m_bhStateProp);
            UScriptStruct* stru = sfp->GetStruct();
            for (UStruct* w = stru; w; w = w->GetSuperStruct()) {
                for (FProperty* p : TFieldRange<FProperty>(w, EFieldIterationFlags::None)) {
                    if (p) m_bhStateOffs[p->GetName()] = p->GetOffset_ForInternal();
                }
            }
        }
        if (m_bhLocoProp) {
            FStructProperty* sfp = static_cast<FStructProperty*>(m_bhLocoProp);
            UScriptStruct* stru = sfp->GetStruct();
            for (UStruct* w = stru; w; w = w->GetSuperStruct()) {
                for (FProperty* p : TFieldRange<FProperty>(w, EFieldIterationFlags::None)) {
                    if (p) m_bhLocoOffs[p->GetName()] = p->GetOffset_ForInternal();
                }
            }
        }
        Output::send<LogLevel::Verbose>(
            STR("[ImmDlg] BH chain resolved: instance={} state_data={} locomotion_data={} stateOffs={} locoOffs={}\n"),
            m_bhAnimInstance->GetFullName(),
            m_bhStateProp ? STR("ok") : STR("null"),
            m_bhLocoProp ? STR("ok") : STR("null"),
            (int)m_bhStateOffs.size(), (int)m_bhLocoOffs.size());
        // Dump the top-level class properties too — Blueprint may add fields specific to
        // AnimBP_player_bh_C that we don't know about yet.
        UClass* bhCls = m_bhAnimInstance->GetClassPrivate();
        if (bhCls) {
            int n = 0;
            for (UStruct* w = bhCls; w && n < 60; w = w->GetSuperStruct()) {
                for (FProperty* p : TFieldRange<FProperty>(w, EFieldIterationFlags::None)) {
                    if (!p || n >= 60) break;
                    Output::send<LogLevel::Verbose>(STR("[ImmDlg]   BH class prop: {}\n"), p->GetName());
                    n++;
                }
            }
        }
    }

    // Mirror the write pattern from ForceLocomotionData onto the BH AnimInstance.
    void ForceBhLocomotion(bool moving, double inputFwd, double inputStrafe) {
        if (!m_bhAnimInstance) return;
        // state_data first: DynamicGaitValue / CurveGaitValue + b* flags.
        if (m_bhStateProp) {
            uint8_t* base = m_bhStateProp->ContainerPtrToValuePtr<uint8_t>(m_bhAnimInstance);
            if (base) {
                auto wB = [&](const wchar_t* name, bool val) {
                    auto it = m_bhStateOffs.find(name);
                    if (it == m_bhStateOffs.end()) return;
                    *reinterpret_cast<bool*>(base + it->second) = val;
                };
                auto wF = [&](const wchar_t* name, float val) {
                    auto it = m_bhStateOffs.find(name);
                    if (it == m_bhStateOffs.end()) return;
                    *reinterpret_cast<float*>(base + it->second) = val;
                };
                wB(STR("bAlive"),     true);
                wB(STR("bMoving"),    moving);
                wB(STR("bWalking"),   moving);
                wB(STR("bRunning"),   false);
                wB(STR("bSprinting"), false);
                wB(STR("bJogging"),   false);
                wB(STR("bInAir"),     false);
                wB(STR("bCutscene"),  false);
                wB(STR("bInCombat"),  false);
                wF(STR("DynamicGaitValue"), moving ? 2.0f : 1.0f);
                wF(STR("CurveGaitValue"),   0.0f);
            }
        }
        // locomotion_data: Velocity / Direction / BPDirection / AngleDirection / PlayRate.
        if (m_bhLocoProp) {
            uint8_t* base = m_bhLocoProp->ContainerPtrToValuePtr<uint8_t>(m_bhAnimInstance);
            if (base) {
                auto wF = [&](const wchar_t* name, float val) {
                    auto it = m_bhLocoOffs.find(name);
                    if (it == m_bhLocoOffs.end()) return;
                    *reinterpret_cast<float*>(base + it->second) = val;
                };
                auto wByte = [&](const wchar_t* name, uint8_t val) {
                    auto it = m_bhLocoOffs.find(name);
                    if (it == m_bhLocoOffs.end()) return;
                    *reinterpret_cast<uint8_t*>(base + it->second) = val;
                };
                auto wB = [&](const wchar_t* name, bool val) {
                    auto it = m_bhLocoOffs.find(name);
                    if (it == m_bhLocoOffs.end()) return;
                    *reinterpret_cast<bool*>(base + it->second) = val;
                };
                float playRate = 0.0f;
                if (moving && inputFwd < 0.0) playRate = -1.0f;
                wF(STR("MovementPlayRate"), playRate);
                wF(STR("LegIKAlpha"),       1.0f);
                wB(STR("bLegIKEnabled"),    true);
                wB(STR("bEnablePlayRateCurves"), true);
                if (!moving) {
                    wF(STR("Velocity"), 0.0f);
                    return;
                }
                double angRad = std::atan2(inputStrafe, inputFwd);
                float  angDeg = (float)(angRad * 180.0 / 3.14159265358979323846);
                wF(STR("AngleDirection"),   angDeg);
                wF(STR("ClampedDirection"), angDeg);
                float velCms = 150.0f;
                if (inputFwd < 0.0) {
                    float backT = (float)(-inputFwd);
                    if (backT > 1.0f) backT = 1.0f;
                    velCms = 150.0f * (1.0f - backT) + 85.0f * backT;
                }
                wF(STR("Velocity"), velCms);
                uint8_t dirBitmask, bpDir;
                double a = angDeg;
                if      (a > -22.5   && a <=  22.5)  { dirBitmask = 1;    bpDir = 1; }
                else if (a >  22.5   && a <=  67.5)  { dirBitmask = 1|8;  bpDir = 6; }
                else if (a >  67.5   && a <= 112.5)  { dirBitmask = 8;    bpDir = 4; }
                else if (a > 112.5   && a <= 157.5)  { dirBitmask = 2|8;  bpDir = 8; }
                else if (a >  157.5  || a <= -157.5) { dirBitmask = 2;    bpDir = 2; }
                else if (a > -157.5  && a <= -112.5) { dirBitmask = 2|4;  bpDir = 7; }
                else if (a > -112.5  && a <=  -67.5) { dirBitmask = 4;    bpDir = 3; }
                else                                  { dirBitmask = 1|4; bpDir = 5; }
                wByte(STR("Direction"),   dirBitmask);
                wByte(STR("BPDirection"), bpDir);
            }
        }
    }

    // Mesh-rotation bypass: since STALKER 2 gates dialogue anim behind a native check we
    // can't intercept (proved by dynGait writes landing without effect), we can't get the
    // strafe animation to play. Instead, physically rotate the visible mesh component's
    // local yaw so the body faces the movement direction — from the mesh's frame, the
    // forward-walk animation (which the dialog anim state plays) now looks correct.
    //
    // Cache each mesh's baseline RelativeRotation on first entry, then set to
    // (base.Pitch, base.Yaw + movementDirYaw, base.Roll) while moving. Reset to baseline
    // when idle or leaving dialogue.
    double m_meshBaseYaw = 0.0;
    double m_shadowBaseYaw = 0.0;
    double m_meshBasePitch = 0.0;
    double m_meshBaseRoll = 0.0;
    double m_shadowBasePitch = 0.0;
    double m_shadowBaseRoll = 0.0;
    bool   m_meshBaseCached = false;
    bool   m_shadowBaseCached = false;

    FRotatorD ReadRelativeRotation(UObject* comp) {
        FRotatorD out{0,0,0};
        if (!comp) return out;
        FProperty* p = comp->GetPropertyByNameInChain(STR("RelativeRotation"));
        if (!p) return out;
        FRotatorD* slot = p->ContainerPtrToValuePtr<FRotatorD>(comp);
        if (slot) out = *slot;
        return out;
    }

    void CacheMeshBaseRotations() {
        if (!m_meshBaseCached && m_pawnMesh) {
            FRotatorD r = ReadRelativeRotation(m_pawnMesh);
            m_meshBasePitch = r.Pitch; m_meshBaseYaw = r.Yaw; m_meshBaseRoll = r.Roll;
            m_meshBaseCached = true;
            Output::send<LogLevel::Verbose>(
                STR("[ImmDlg] main mesh baseline rot: P={} Y={} R={}\n"),
                (float)r.Pitch, (float)r.Yaw, (float)r.Roll);
        }
        if (!m_shadowBaseCached && m_shadowMeshComp) {
            FRotatorD r = ReadRelativeRotation(m_shadowMeshComp);
            m_shadowBasePitch = r.Pitch; m_shadowBaseYaw = r.Yaw; m_shadowBaseRoll = r.Roll;
            m_shadowBaseCached = true;
            Output::send<LogLevel::Verbose>(
                STR("[ImmDlg] shadow mesh baseline rot: P={} Y={} R={}\n"),
                (float)r.Pitch, (float)r.Yaw, (float)r.Roll);
        }
    }

    // Apply mesh rotation offset for movement direction. Called each frame in dialogue.
    // yawOffsetDeg = atan2(strafe, fwd) — 0 for pure fwd, +90 for pure right, -90 for pure left.
    // When moving==false, reset both meshes to baseline.
    void ApplyMeshMovementRotation(bool moving, double yawOffsetDeg) {
        CacheMeshBaseRotations();
        if (moving) {
            if (m_pawnMesh && m_meshBaseCached) {
                SetMeshRelativeYaw(m_pawnMesh, m_meshBaseYaw + yawOffsetDeg);
            }
            if (m_shadowMeshComp && m_shadowBaseCached) {
                SetMeshRelativeYaw(m_shadowMeshComp, m_shadowBaseYaw + yawOffsetDeg);
            }
        } else {
            if (m_pawnMesh && m_meshBaseCached) {
                SetMeshRelativeYaw(m_pawnMesh, m_meshBaseYaw);
            }
            if (m_shadowMeshComp && m_shadowBaseCached) {
                SetMeshRelativeYaw(m_shadowMeshComp, m_shadowBaseYaw);
            }
        }
    }

    void ForceShadowAnimState(bool moving) {
        if (!m_shadowStateProp || !m_shadowAnimInstance) return;
        uint8_t* base = m_shadowStateProp->ContainerPtrToValuePtr<uint8_t>(m_shadowAnimInstance);
        if (!base) return;
        auto wB = [&](const wchar_t* name, bool val) {
            auto it = m_shadowStateOffs.find(name);
            if (it == m_shadowStateOffs.end()) return;
            *reinterpret_cast<bool*>(base + it->second) = val;
        };
        auto wF = [&](const wchar_t* name, float val) {
            auto it = m_shadowStateOffs.find(name);
            if (it == m_shadowStateOffs.end()) return;
            *reinterpret_cast<float*>(base + it->second) = val;
        };
        wB(STR("bAlive"),     true);
        wB(STR("bMoving"),    moving);
        wB(STR("bWalking"),   moving);
        wB(STR("bRunning"),   false);
        wB(STR("bSprinting"), false);
        wB(STR("bJogging"),   false);
        wB(STR("bInAir"),     false);
        wB(STR("bCutscene"),  false);
        wB(STR("bInCombat"),  false);
        // Mirror the same gait writes we do on the main AnimInstance's state_data. The
        // shadow AnimInstance has no locomotion_data (only state_data + shadow_data), so
        // its state machine likely reads DynamicGaitValue for gait selection. Match the
        // outside-dialogue ground truth: dynGait=2, curveGait=0 when moving; 1/0 when idle.
        wF(STR("DynamicGaitValue"), moving ? 2.0f : 1.0f);
        wF(STR("CurveGaitValue"),   0.0f);
    }

    // Every frame in dialogue: write our set of anim-state bool overrides. "moving" gates
    // the walking-related ones; dialogue-related ones are always forced false while in dlg.
    void ForceAnimState(bool moving) {
        if (!m_animInstance) return;
        for (auto& b : m_animBoolProps) {
            if (!b.prop) continue;
            // Skip walking-true when not moving so we don't lie that we're always walking.
            if (b.valueToForce == true && !moving) continue;
            bool* slot = b.prop->ContainerPtrToValuePtr<bool>(m_animInstance);
            if (slot) *slot = b.valueToForce;
        }
        // Direct write to `dialog_data.dialog` (bool) if we located the struct.
        // AnimPlayerDialogData has exactly ONE member (dialog: bool at offset 0), so writing
        // the first byte of the struct's memory IS writing the bool.
        if (m_dialogDataProp) {
            uint8_t* structMem = m_dialogDataProp->ContainerPtrToValuePtr<uint8_t>(m_animInstance);
            if (structMem) *structMem = 0; // dialog = false
        }
        // Write state_data override bools.
        ForceStateDataOverrides(moving);
    }
    FProperty* m_dialogDataProp = nullptr;

    // AnimPlayerStateData nested-struct writes. state_data is a StructProperty on the
    // AnimInstance; walking_override / jogging_override / sprinting_override / etc. are
    // bool members INSIDE it. We resolve their inner offsets once via UScriptStruct's
    // CustomFindProperty and then write per-frame.
    FProperty* m_stateDataProp = nullptr;
    FProperty* m_locomotionDataProp = nullptr;
    FProperty* m_shadowDataProp = nullptr;
    std::map<StringType, int32_t> m_locomotionOffsets;  // name -> offset (parents included)
    std::map<StringType, int32_t> m_shadowOffsets;      // name -> offset (parents included)
    // Direct children of AnimPlayerStateData (see log dump: state_data.b* off=N).
    int32_t m_offWalkingOverride = -1;   // bWalkingOverride
    int32_t m_offJoggingOverride = -1;   // bJoggingOverride
    int32_t m_offSprintingOverride = -1; // bSprintingOverride
    int32_t m_offCrouchingOverride = -1; // bCrouchingOverride
    int32_t m_offInAirOverride = -1;     // bInAirOverride
    int32_t m_offCombatMoveIdle = -1;    // bCombatMoveIdle
    int32_t m_offCombatCrouchIdle = -1;  // bCombatCrouchIdle
    // Parent struct AnimStateData bools (low offsets 0..9).
    int32_t m_offAlive = -1;             // bAlive
    int32_t m_offMoving = -1;            // bMoving
    int32_t m_offWalking = -1;           // bWalking
    int32_t m_offRunning = -1;           // bRunning
    int32_t m_offSprinting = -1;         // bSprinting
    int32_t m_offJogging = -1;           // bJogging
    int32_t m_offInAir = -1;             // bInAir
    int32_t m_offCutscene = -1;          // bCutscene
    int32_t m_offInCombat = -1;          // bInCombat

    void ResolveStateDataOffsets() {
        if (m_stateDataProp) return;
        if (!m_animInstance) return;
        // Try both snake_case (dump form) and PascalCase (UE C++ form).
        const wchar_t* sdNames[] = { STR("state_data"), STR("StateData") };
        FProperty* sp = nullptr;
        for (auto* n : sdNames) { sp = m_animInstance->GetPropertyByNameInChain(n); if (sp) break; }
        if (!sp) {
            Output::send<LogLevel::Verbose>(STR("[ImmDlg]   state_data prop NOT FOUND under any casing\n"));
            return;
        }
        m_stateDataProp = sp;
        FStructProperty* sfp = static_cast<FStructProperty*>(sp);
        UScriptStruct* stru = sfp->GetStruct();
        if (!stru) return;
        // Walk this struct + all parent structs, building a name -> offset map from the real
        // property list (rather than guessing at names). This handles both the direct child
        // b* props on AnimPlayerStateData and the inherited ones on AnimStateData parent.
        std::map<StringType, int32_t> offMap;
        UStruct* walker = stru;
        while (walker) {
            for (FProperty* p : TFieldRange<FProperty>(walker, EFieldIterationFlags::None)) {
                if (!p) continue;
                offMap[p->GetName()] = p->GetOffset_ForInternal();
            }
            walker = walker->GetSuperStruct();
        }
        auto off = [&](const wchar_t* name) -> int32_t {
            auto it = offMap.find(name);
            return it == offMap.end() ? -1 : it->second;
        };
        m_offWalkingOverride   = off(STR("bWalkingOverride"));
        m_offJoggingOverride   = off(STR("bJoggingOverride"));
        m_offSprintingOverride = off(STR("bSprintingOverride"));
        m_offCrouchingOverride = off(STR("bCrouchingOverride"));
        m_offInAirOverride     = off(STR("bInAirOverride"));
        m_offCombatMoveIdle    = off(STR("bCombatMoveIdle"));
        m_offCombatCrouchIdle  = off(STR("bCombatCrouchIdle"));
        m_offAlive             = off(STR("bAlive"));
        m_offMoving            = off(STR("bMoving"));
        m_offWalking           = off(STR("bWalking"));
        m_offRunning           = off(STR("bRunning"));
        m_offSprinting         = off(STR("bSprinting"));
        m_offJogging           = off(STR("bJogging"));
        m_offInAir             = off(STR("bInAir"));
        m_offCutscene          = off(STR("bCutscene"));
        m_offInCombat          = off(STR("bInCombat"));
        Output::send<LogLevel::Verbose>(
            STR("[ImmDlg]   state_data offsets: walk={}, jog={}, sprint={}, crouch={}, combatMoveIdle={}, combatCrouchIdle={}\n"),
            m_offWalkingOverride, m_offJoggingOverride, m_offSprintingOverride,
            m_offCrouchingOverride, m_offCombatMoveIdle, m_offCombatCrouchIdle);

        // Also resolve + dump locomotion_data and shadow_data properties, same technique.
        auto resolveAndDump = [&](const wchar_t* n1, const wchar_t* n2, const wchar_t* label,
                                  FProperty*& outProp, std::map<StringType, int32_t>& outMap) {
            FProperty* pp = m_animInstance->GetPropertyByNameInChain(n1);
            if (!pp) pp = m_animInstance->GetPropertyByNameInChain(n2);
            if (!pp) {
                Output::send<LogLevel::Verbose>(STR("[ImmDlg]   {} prop NOT FOUND\n"), label);
                return;
            }
            outProp = pp;
            FStructProperty* sfp2 = static_cast<FStructProperty*>(pp);
            UScriptStruct* stru2 = sfp2->GetStruct();
            if (!stru2) return;
            UStruct* walker2 = stru2;
            int printed2 = 0;
            while (walker2) {
                for (FProperty* p : TFieldRange<FProperty>(walker2, EFieldIterationFlags::None)) {
                    if (!p) continue;
                    outMap[p->GetName()] = p->GetOffset_ForInternal();
                    if (printed2 < 40) {
                        Output::send<LogLevel::Verbose>(STR("[ImmDlg]     {}.{} off={}\n"),
                                                         label, p->GetName(), p->GetOffset_ForInternal());
                        printed2++;
                    }
                }
                walker2 = walker2->GetSuperStruct();
            }
        };
        resolveAndDump(STR("locomotion_data"), STR("LocomotionData"), STR("locomotion_data"),
                       m_locomotionDataProp, m_locomotionOffsets);
        resolveAndDump(STR("shadow_data"),     STR("ShadowData"),     STR("shadow_data"),
                       m_shadowDataProp, m_shadowOffsets);

        // One-time overview: list every top-level property on the AnimInstance so we see
        // which sibling data structs might drive upper-body/shadow behavior we haven't hit.
        {
            int lim = 0;
            UClass* aic = m_animInstance->GetClassPrivate();
            UStruct* w2 = aic;
            while (w2 && lim < 40) {
                for (FProperty* p : TFieldRange<FProperty>(w2, EFieldIterationFlags::None)) {
                    if (!p) continue;
                    Output::send<LogLevel::Verbose>(STR("[ImmDlg]   AI top-level: {}\n"), p->GetName());
                    if (++lim >= 40) break;
                }
                w2 = w2->GetSuperStruct();
            }
        }
    }

    // Drive locomotion_data (numeric — Velocity, AngleDirection, MovementPlayRate, LegIK).
    // The anim graph's walk blend is driven by these; without them the body doesn't twist
    // even if state_data.bWalking is set. inputFwd/inputStrafe are the normalized WASD
    // vector — we convert to a direction angle in degrees.
    void ForceLocomotionData(bool moving, double inputFwd, double inputStrafe) {
        if (!m_locomotionDataProp || !m_animInstance) return;
        uint8_t* base = m_locomotionDataProp->ContainerPtrToValuePtr<uint8_t>(m_animInstance);
        if (!base) return;
        auto writeF = [&](const wchar_t* name, float val) {
            auto it = m_locomotionOffsets.find(name);
            if (it == m_locomotionOffsets.end()) return;
            *reinterpret_cast<float*>(base + it->second) = val;
        };
        auto writeB = [&](const wchar_t* name, bool val) {
            auto it = m_locomotionOffsets.find(name);
            if (it == m_locomotionOffsets.end()) return;
            *reinterpret_cast<bool*>(base + it->second) = val;
        };
        // Ground-truth PlayRate outside dialogue: 0 for fwd/strafe (game reads velocity
        // directly), -1 for backward (game plays fwd-walk anim in reverse). Our previous
        // constant 1 was wrong for both directions.
        float playRate = 0.0f;
        if (moving && inputFwd < 0.0) playRate = -1.0f;
        writeF(STR("MovementPlayRate"), playRate);
        writeF(STR("LegIKAlpha"),       1.0f);
        writeB(STR("bLegIKEnabled"),    true);
        writeB(STR("bEnablePlayRateCurves"), true);
        if (!moving) {
            writeF(STR("Velocity"), 0.0f);
            return;
        }
        // Ground-truth dump revealed:
        //   - Velocity is cm/s (~150 fwd walk, ~85 back walk)
        //   - Direction is BITMASK (Fwd=1 Back=2 Left=4 Right=8, OR'd for diagonals)
        //   - BPDirection is 8-way (1=Fwd 2=Back 3=Left 4=Right 5=FwdLeft 6=FwdRight 7=BackLeft 8=BackRight)
        //   - AngleDirection is atan2(strafe, fwd) in degrees
        // NOTE: We ALSO call PC::SetMoveVector in on_update, which is what the game's own
        // input pipeline uses. If that succeeds, the game overwrites these values with
        // correct ones (harmless — our writes get replaced by the game's). If not (or if
        // the func doesn't exist), these serve as the fallback.
        double angRad = std::atan2(inputStrafe, inputFwd);
        float  angDeg = (float)(angRad * 180.0 / 3.14159265358979323846);
        writeF(STR("AngleDirection"),   angDeg);
        writeF(STR("ClampedDirection"), angDeg);
        float velCms = 150.0f;
        if (inputFwd < 0.0) {
            float backT = (float)(-inputFwd);
            if (backT > 1.0f) backT = 1.0f;
            velCms = 150.0f * (1.0f - backT) + 85.0f * backT;
        }
        writeF(STR("Velocity"), velCms);

        uint8_t dirBitmask, bpDir;
        double a = angDeg;
        if      (a > -22.5   && a <=  22.5)  { dirBitmask = 1;    bpDir = 1; }
        else if (a >  22.5   && a <=  67.5)  { dirBitmask = 1|8;  bpDir = 6; }
        else if (a >  67.5   && a <= 112.5)  { dirBitmask = 8;    bpDir = 4; }
        else if (a > 112.5   && a <= 157.5)  { dirBitmask = 2|8;  bpDir = 8; }
        else if (a >  157.5  || a <= -157.5) { dirBitmask = 2;    bpDir = 2; }
        else if (a > -157.5  && a <= -112.5) { dirBitmask = 2|4;  bpDir = 7; }
        else if (a > -112.5  && a <=  -67.5) { dirBitmask = 4;    bpDir = 3; }
        else                                  { dirBitmask = 1|4; bpDir = 5; }
        auto writeByte = [&](const wchar_t* name, uint8_t val) {
            auto it = m_locomotionOffsets.find(name);
            if (it == m_locomotionOffsets.end()) return;
            *reinterpret_cast<uint8_t*>(base + it->second) = val;
        };
        writeByte(STR("Direction"),   dirBitmask);
        writeByte(STR("BPDirection"), bpDir);
    }


    // Drive shadow_data. bShouldUseBHLocomotion is the key — off = shadow uses static pose,
    // on = shadow follows the body's locomotion animation.
    void ForceShadowData(bool moving, double inputFwd, double inputStrafe) {
        if (!m_shadowDataProp || !m_animInstance) return;
        uint8_t* base = m_shadowDataProp->ContainerPtrToValuePtr<uint8_t>(m_animInstance);
        if (!base) return;
        auto writeF = [&](const wchar_t* name, float val) {
            auto it = m_shadowOffsets.find(name);
            if (it == m_shadowOffsets.end()) return;
            *reinterpret_cast<float*>(base + it->second) = val;
        };
        auto writeB = [&](const wchar_t* name, bool val) {
            auto it = m_shadowOffsets.find(name);
            if (it == m_shadowOffsets.end()) return;
            *reinterpret_cast<bool*>(base + it->second) = val;
        };
        writeB(STR("bShouldUseBHLocomotion"), moving);
        writeF(STR("MovementPlayRate"),      moving ? 1.0f : 0.0f);
        if (moving) {
            double angRad = std::atan2(inputStrafe, inputFwd);
            float  angDeg = (float)(angRad * 180.0 / 3.14159265358979323846);
            writeF(STR("AngleDirection"), angDeg);
        }
    }

    // Periodic diagnostic: log current state/locomotion/shadow values (~every 500ms) so we
    // can compare what the anim graph looks like outside dialogue vs. what our writes produce
    // inside dialogue. Runs both in AND out of dialogue.
    uint64_t m_lastStateLogMs = 0;
    void LogAnimStateOnce(bool inDlg, bool moving) {
        if (!m_animInstance) return;
        uint64_t now = GetTickCount64();
        if (now - m_lastStateLogMs < 500) return;
        m_lastStateLogMs = now;

        auto readBool = [&](FProperty* structProp, const std::map<StringType, int32_t>& map, const wchar_t* name) -> int {
            if (!structProp) return -1;
            auto it = map.find(name);
            if (it == map.end()) return -1;
            uint8_t* base = structProp->ContainerPtrToValuePtr<uint8_t>(m_animInstance);
            if (!base) return -1;
            return *(bool*)(base + it->second) ? 1 : 0;
        };
        auto readFloat = [&](FProperty* structProp, const std::map<StringType, int32_t>& map, const wchar_t* name) -> float {
            if (!structProp) return 0.0f;
            auto it = map.find(name);
            if (it == map.end()) return 0.0f;
            uint8_t* base = structProp->ContainerPtrToValuePtr<uint8_t>(m_animInstance);
            if (!base) return 0.0f;
            return *(float*)(base + it->second);
        };
        auto readByte = [&](FProperty* structProp, const std::map<StringType, int32_t>& map, const wchar_t* name) -> int {
            if (!structProp) return -1;
            auto it = map.find(name);
            if (it == map.end()) return -1;
            uint8_t* base = structProp->ContainerPtrToValuePtr<uint8_t>(m_animInstance);
            if (!base) return -1;
            return (int)*(uint8_t*)(base + it->second);
        };
        // Build state_data offset map to reuse (already have some as m_off* but not a map).
        // Simpler: hardcode via the members we resolved.
        auto readStateBool = [&](int32_t off) -> int {
            if (off < 0 || !m_stateDataProp) return -1;
            uint8_t* base = m_stateDataProp->ContainerPtrToValuePtr<uint8_t>(m_animInstance);
            if (!base) return -1;
            return *(bool*)(base + off) ? 1 : 0;
        };

        Output::send<LogLevel::Verbose>(
            STR("[ImmDlg] STATE inDlg={} mov={} | state: bMoving={} bWalking={} bWalkOvr={} bRunning={} bJogging={} bSprinting={} bCutscene={} bInCombat={} bInAir={} | loco: V={} AngDir={} Dir={} BPDir={} PlayRate={} LegIKAlpha={} bLegIK={} | shadow: bBHLoco={} PlayRate={} AngDir={}\n"),
            inDlg ? 1 : 0, moving ? 1 : 0,
            readStateBool(m_offMoving), readStateBool(m_offWalking), readStateBool(m_offWalkingOverride),
            readStateBool(m_offRunning), readStateBool(m_offJogging), readStateBool(m_offSprinting),
            readStateBool(m_offCutscene), readStateBool(m_offInCombat), readStateBool(m_offInAir),
            readFloat(m_locomotionDataProp, m_locomotionOffsets, STR("Velocity")),
            readFloat(m_locomotionDataProp, m_locomotionOffsets, STR("AngleDirection")),
            readByte (m_locomotionDataProp, m_locomotionOffsets, STR("Direction")),
            readByte (m_locomotionDataProp, m_locomotionOffsets, STR("BPDirection")),
            readFloat(m_locomotionDataProp, m_locomotionOffsets, STR("MovementPlayRate")),
            readFloat(m_locomotionDataProp, m_locomotionOffsets, STR("LegIKAlpha")),
            readBool (m_locomotionDataProp, m_locomotionOffsets, STR("bLegIKEnabled")),
            readBool (m_shadowDataProp,     m_shadowOffsets,     STR("bShouldUseBHLocomotion")),
            readFloat(m_shadowDataProp,     m_shadowOffsets,     STR("MovementPlayRate")),
            readFloat(m_shadowDataProp,     m_shadowOffsets,     STR("AngleDirection"))
        );

        // Extended dump: the state_data properties we haven't been reading yet. These are
        // the most likely gates for the "dialog locomotion state" that plays forward-walk
        // regardless of Direction. Compare in/out of dialogue to find the differing lever.
        auto readStateByteByName = [&](const wchar_t* name) -> int {
            if (!m_stateDataProp) return -1;
            uint8_t* base = m_stateDataProp->ContainerPtrToValuePtr<uint8_t>(m_animInstance);
            if (!base) return -1;
            FProperty* p = nullptr;
            {
                FStructProperty* sfp = static_cast<FStructProperty*>(m_stateDataProp);
                UScriptStruct* stru = sfp->GetStruct();
                UStruct* w = stru;
                while (w && !p) {
                    for (FProperty* pp : TFieldRange<FProperty>(w, EFieldIterationFlags::None)) {
                        if (pp && pp->GetName() == name) { p = pp; break; }
                    }
                    w = w->GetSuperStruct();
                }
            }
            if (!p) return -1;
            return (int)*(uint8_t*)(base + p->GetOffset_ForInternal());
        };
        auto readStateFloatByName = [&](const wchar_t* name) -> float {
            if (!m_stateDataProp) return 0.0f;
            uint8_t* base = m_stateDataProp->ContainerPtrToValuePtr<uint8_t>(m_animInstance);
            if (!base) return 0.0f;
            FProperty* p = nullptr;
            {
                FStructProperty* sfp = static_cast<FStructProperty*>(m_stateDataProp);
                UScriptStruct* stru = sfp->GetStruct();
                UStruct* w = stru;
                while (w && !p) {
                    for (FProperty* pp : TFieldRange<FProperty>(w, EFieldIterationFlags::None)) {
                        if (pp && pp->GetName() == name) { p = pp; break; }
                    }
                    w = w->GetSuperStruct();
                }
            }
            if (!p) return 0.0f;
            return *(float*)(base + p->GetOffset_ForInternal());
        };
        auto readStateBoolByName = [&](const wchar_t* name) -> int {
            if (!m_stateDataProp) return -1;
            uint8_t* base = m_stateDataProp->ContainerPtrToValuePtr<uint8_t>(m_animInstance);
            if (!base) return -1;
            FProperty* p = nullptr;
            {
                FStructProperty* sfp = static_cast<FStructProperty*>(m_stateDataProp);
                UScriptStruct* stru = sfp->GetStruct();
                UStruct* w = stru;
                while (w && !p) {
                    for (FProperty* pp : TFieldRange<FProperty>(w, EFieldIterationFlags::None)) {
                        if (pp && pp->GetName() == name) { p = pp; break; }
                    }
                    w = w->GetSuperStruct();
                }
            }
            if (!p) return -1;
            return *(bool*)(base + p->GetOffset_ForInternal()) ? 1 : 0;
        };
        // dialog_data.dialog is a single bool at offset 0 of the struct (we know from prior probe).
        int dialogBit = -1;
        if (m_dialogDataProp) {
            uint8_t* dm = m_dialogDataProp->ContainerPtrToValuePtr<uint8_t>(m_animInstance);
            if (dm) dialogBit = *dm ? 1 : 0;
        }
        Output::send<LogLevel::Verbose>(
            STR("[ImmDlg] STATE-EXT inDlg={} mov={} | gait={} curveGait={} dynGait={} actionSlot={} fullBodySlot={} combatAct={} climbing={} crouching={} dragBody={} inspItem={} leftHandBusy={} dialog_data.dialog={}\n"),
            inDlg ? 1 : 0, moving ? 1 : 0,
            readStateByteByName(STR("EnumGaitState")),
            readStateFloatByName(STR("CurveGaitValue")),
            readStateFloatByName(STR("DynamicGaitValue")),
            readStateBoolByName(STR("bActionSlotActive")),
            readStateBoolByName(STR("bFullBodySlotActive")),
            readStateBoolByName(STR("bCombatActionActive")),
            readStateBoolByName(STR("bClimbing")),
            readStateBoolByName(STR("bCrouching")),
            readStateBoolByName(STR("bDragDeadBody")),
            readStateBoolByName(STR("bIsInspectingItem")),
            readStateBoolByName(STR("bIsLeftHandBusy")),
            dialogBit
        );
    }

    // Apply the same set of "walking, alive, not-in-air/combat/cutscene" writes to any
    // struct whose name→offset map we resolved. Locomotion + shadow both benefit from
    // knowing "walking + moving, everything else off" so their anim states follow.
    void ForceStructWalkState(FProperty* structProp,
                              const std::map<StringType, int32_t>& offMap,
                              bool moving) {
        if (!structProp || !m_animInstance) return;
        uint8_t* base = structProp->ContainerPtrToValuePtr<uint8_t>(m_animInstance);
        if (!base) return;
        auto w = [&](const wchar_t* name, bool val) {
            auto it = offMap.find(name);
            if (it == offMap.end()) return;
            *reinterpret_cast<bool*>(base + it->second) = val;
        };
        w(STR("bAlive"),     true);
        w(STR("bMoving"),    moving);
        w(STR("bWalking"),   moving);
        w(STR("bIsWalking"), moving);
        w(STR("bIsMoving"),  moving);
        w(STR("bRunning"),   false);
        w(STR("bSprinting"), false);
        w(STR("bJogging"),   false);
        w(STR("bInAir"),     false);
        w(STR("bCutscene"),  false);
        w(STR("bInCombat"),  false);
        w(STR("bCrouching"), false);
        // Override inputs — bWalkingOverride left OFF (ground truth: 0 outside dialogue).
        w(STR("bJoggingOverride"),   false);
        w(STR("bSprintingOverride"), false);
        w(STR("bCrouchingOverride"), false);
        w(STR("bInAirOverride"),     false);
    }

    void ForceStateDataOverrides(bool moving) {
        if (!m_stateDataProp || !m_animInstance) return;
        uint8_t* structBase = m_stateDataProp->ContainerPtrToValuePtr<uint8_t>(m_animInstance);
        if (!structBase) return;
        auto write = [&](int32_t off, bool val) {
            if (off < 0) return;
            *reinterpret_cast<bool*>(structBase + off) = val;
        };
        // Ground-truth log comparison (outside vs inside dialogue while walking):
        //   outside walking: dynGait=2, curveGait=0
        //   outside idle   : dynGait=1, curveGait=0
        //   inside  walking: dynGait=1, curveGait=? (our previous force to 3 pushed graph
        //     into a dialogue-restricted gait branch with no strafe blends)
        // Match the outside values exactly so the anim graph enters its normal locomotion
        // state with directional blends available.
        {
            FStructProperty* sfp = static_cast<FStructProperty*>(m_stateDataProp);
            UScriptStruct* stru = sfp->GetStruct();
            UStruct* w = stru;
            FProperty* dynProp = nullptr;
            FProperty* curveProp = nullptr;
            while (w && !(dynProp && curveProp)) {
                for (FProperty* p : TFieldRange<FProperty>(w, EFieldIterationFlags::None)) {
                    if (!p) continue;
                    if (!dynProp   && p->GetName() == STR("DynamicGaitValue")) dynProp = p;
                    if (!curveProp && p->GetName() == STR("CurveGaitValue"))   curveProp = p;
                }
                w = w->GetSuperStruct();
            }
            float dynTarget   = moving ? 2.0f : 1.0f;
            float curveTarget = 0.0f;
            if (dynProp)   *reinterpret_cast<float*>(structBase + dynProp->GetOffset_ForInternal())   = dynTarget;
            if (curveProp) *reinterpret_cast<float*>(structBase + curveProp->GetOffset_ForInternal()) = curveTarget;
        }
        // Override flags: never jog/sprint/crouch/in-air/combat-idle in dialogue.
        write(m_offJoggingOverride,   false);
        write(m_offSprintingOverride, false);
        write(m_offCrouchingOverride, false);
        write(m_offInAirOverride,     false);
        write(m_offCombatMoveIdle,    false);
        write(m_offCombatCrouchIdle,  false);
        // Ground-truth dump: bWalkingOverride is 0 when walking works outside dialogue.
        // Forcing it to 1 was masking the underlying walk state — leave it off; the raw
        // bMoving/bWalking writes below carry the state transition.
        write(m_offWalkingOverride,   false);
        // Also force the raw state flags from the parent AnimStateData struct, in case the
        // anim graph reads those directly rather than through the *_override inputs.
        write(m_offAlive,      true);
        write(m_offInAir,      false);
        write(m_offCutscene,   false);
        write(m_offInCombat,   false);
        write(m_offRunning,    false);
        write(m_offSprinting,  false);
        write(m_offJogging,    false);
        write(m_offMoving,     moving);
        write(m_offWalking,    moving);
    }

    // Resolve pawn camera + FOV property (once, on first dialogue entry).
    void ResolvePawnCamera(UObject* pawn) {
        if (m_pawnCamera) return;
        UFunction* getCam = pawn->GetFunctionByNameInChain(FName(STR("GetCameraComponent")));
        if (!getCam) getCam = pawn->GetFunctionByNameInChain(FName(STR("K2_GetCameraComponent")));
        if (!getCam) return;
        struct { UObject* Ret; } p{nullptr};
        pawn->ProcessEvent(getCam, &p);
        m_pawnCamera = p.Ret;
        if (m_pawnCamera) {
            m_fovProp = m_pawnCamera->GetPropertyByNameInChain(STR("FieldOfView"));
            Output::send<LogLevel::Verbose>(STR("[ImmDlg] camera+fov resolved: cam={}, fovProp={}\n"),
                                             m_pawnCamera ? STR("ok") : STR("null"),
                                             m_fovProp    ? STR("ok") : STR("null"));
        }
    }

    // Read current camera FOV via property. Returns 0 if not available.
    float ReadCameraFov() {
        if (!m_pawnCamera || !m_fovProp) return 0.0f;
        float* slot = m_fovProp->ContainerPtrToValuePtr<float>(m_pawnCamera);
        return slot ? *slot : 0.0f;
    }
    void WriteCameraFov(float fov) {
        if (!m_pawnCamera || !m_fovProp) return;
        float* slot = m_fovProp->ContainerPtrToValuePtr<float>(m_pawnCamera);
        if (slot) *slot = fov;
    }

    // Camera is attached to a mesh socket (head bone for head-bob) — so when we rotate
    // the mesh for the strafe bypass, camera rotation follows. Call USceneComponent::
    // SetAbsolute(bAbsLoc, bAbsRot, bAbsScale) on the camera to make its rotation ignore
    // the parent bone. Camera location still tracks head bone (fine for yaw rotation
    // since head is directly above pawn origin — Y/X don't change).
    bool m_camAbsRotApplied = false;
    void SetCameraRotationAbsolute(bool absolute) {
        if (!m_pawnCamera) return;
        if (absolute == m_camAbsRotApplied) return;
        UFunction* fn = m_pawnCamera->GetFunctionByNameInChain(FName(STR("SetAbsolute")));
        if (!fn) return;
        struct { bool bAbsLoc; bool bAbsRot; bool bAbsScale; } p{false, absolute, false};
        m_pawnCamera->ProcessEvent(fn, &p);
        m_camAbsRotApplied = absolute;
        Output::send<LogLevel::Verbose>(
            STR("[ImmDlg] camera rotation absolute -> {}\n"),
            absolute ? STR("true") : STR("false"));
    }

    // Apply FOV policy in/out of dialogue.
    // Outside dialogue: cache the current FOV as the "normal" value (updated every ~1s
    // in case game changes it based on aiming/scope state — we grab the last stable one).
    // In dialogue: if DialogueFov > 0, force it; else if DisableDialogueFovChange, force the cached non-dialogue FOV.
    uint64_t m_lastFovSampleMs = 0;
    void ApplyFovPolicy(bool inDlg) {
        if (!m_pawnCamera || !m_fovProp) return;
        uint64_t now = GetTickCount64();
        if (!inDlg) {
            if (now - m_lastFovSampleMs > 1000) {
                float cur = ReadCameraFov();
                if (cur > 30.0f && cur < 170.0f) m_cachedNonDialogueFov = cur;
                m_lastFovSampleMs = now;
            }
            return;
        }
        float target = 0.0f;
        if (m_dialogueFovOverride > 0.0f)     target = m_dialogueFovOverride;
        else if (m_disableDialogueFov)        target = m_cachedNonDialogueFov;
        if (target > 30.0f && target < 170.0f) WriteCameraFov(target);
    }

    void PollHotkeys() {
        // F6 — camera centering (F5 is quicksave in STALKER 2, don't stomp it).
        bool f6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        if (f6 && !m_f5Prev) {
            m_camCenteringDisabled = !m_camCenteringDisabled;
            SaveConfig();
            Output::send<LogLevel::Verbose>(
                STR("[ImmDlg] camera centering (F6) -> {}\n"),
                m_camCenteringDisabled ? STR("DISABLED (camera free)") : STR("enabled (game default)"));
        }
        m_f5Prev = f6;

        // F7 — disable dialogue FOV change.
        bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        if (f7 && !m_f7Prev) {
            m_disableDialogueFov = !m_disableDialogueFov;
            SaveConfig();
            Output::send<LogLevel::Verbose>(
                STR("[ImmDlg] disable dialogue FOV change (F7) -> {}\n"),
                m_disableDialogueFov ? STR("YES (keep non-dialogue FOV)") : STR("no (game's dialogue FOV shift applies)"));
        }
        m_f7Prev = f7;
    }

    void ApplyCameraCenteringToggle(bool inDlg) {
        if (!m_camCenteringDisabled || !inDlg) return;
        // Rescan every ~2s for CameraModifier_LookAt instances (game may add/remove them on
        // dialogue enter/exit or per-NPC).
        uint64_t now = GetTickCount64();
        if (m_lookAtModifiers.empty() || (now - m_lastLookAtRescanMs) > 2000) {
            m_lastLookAtRescanMs = now;
            m_lookAtModifiers.clear();
            UObjectGlobals::FindAllOf(STR("CameraModifier_LookAt"), m_lookAtModifiers);
            if (!m_disableModifierFn && !m_lookAtModifiers.empty()) {
                // UCameraModifier::DisableModifier(bool bImmediate) — UFUNCTION on base class.
                m_disableModifierFn = m_lookAtModifiers[0]->GetFunctionByNameInChain(FName(STR("DisableModifier")));
                if (m_disableModifierFn) {
                    Output::send<LogLevel::Verbose>(
                        STR("[ImmDlg] look-at modifiers found: {}, disable fn resolved\n"),
                        (int)m_lookAtModifiers.size());
                }
            }
        }
        if (!m_disableModifierFn) return;
        // Call DisableModifier(true) on every instance every frame — game may re-enable.
        for (UObject* mod : m_lookAtModifiers) {
            if (!mod) continue;
            struct { bool bImmediate; } p{true};
            mod->ProcessEvent(m_disableModifierFn, &p);
        }
    }

    auto on_update() -> void override {
        UObject* pawn = GetPawn();
        if (!pawn) return;
        // Our own poll must bypass the lie hook — set thread-local guard around the call.
        tl_selfDialogueQuery = true;
        bool inDlg = CallBool(pawn, STR("IsInStaticDialog"));
        tl_selfDialogueQuery = false;
        g_inDialogue.store(inDlg, std::memory_order_relaxed);

        // Resolve camera once (runs in AND out of dialogue so we can sample non-dialogue FOV).
        ResolvePawnCamera(pawn);
        ResolvePawnAnimInstance(pawn);
        ResolveShadowChain(pawn);
        ResolveBhChain(pawn);
        ResolveRotationControl(pawn);
        ApplyFovPolicy(inDlg);
        LogPcState(pawn, inDlg);

        // Hotkey polling every frame (works even outside dialogue).
        PollHotkeys();
        // Apply camera-centering-disable in dialogue if user has toggled it on.
        ApplyCameraCenteringToggle(inDlg);

        if (!inDlg) {
            g_dx.exchange(0); g_dy.exchange(0);
            m_pending_dx = 0.0; m_pending_dy = 0.0;
            m_prevInDialog = false;
            // Log outside-dialogue anim state too — need this to compare against in-dialogue.
            // We don't know if user is moving outside dialogue at this point (WASD not sampled
            // here), so pass 'moving' as -1 sentinel via any non-zero: reflect actual movement
            // by peeking key state.
            bool wOut = (GetAsyncKeyState('W') & 0x8000) != 0;
            bool aOut = (GetAsyncKeyState('A') & 0x8000) != 0;
            bool sOut = (GetAsyncKeyState('S') & 0x8000) != 0;
            bool dOut = (GetAsyncKeyState('D') & 0x8000) != 0;
            bool movingOut = wOut || aOut || sOut || dOut;
            LogAnimStateOnce(false, movingOut);
            return;
        }

        // Refresh in-game sensitivity on each dialogue entry.
        if (!m_prevInDialog) {
            LoadStalker2Settings();
            ProbeFootstepAudio(pawn);
            ProbeAllAnimInstances(pawn);
            DumpMainAnimInstanceProps();
            m_prevInDialog = true;
        }

        ResetIgnore(pawn);

        // ---- Controller poll (once per frame) ----
        double padMoveX = 0.0, padMoveY = 0.0, padLookX = 0.0, padLookY = 0.0;
        ReadPadSticks(padMoveX, padMoveY, padLookX, padLookY);

        // ---- Movement: WASD + left stick (pad overrides keyboard per axis) ----
        bool w = (GetAsyncKeyState('W') & 0x8000) != 0;
        bool a = (GetAsyncKeyState('A') & 0x8000) != 0;
        bool s = (GetAsyncKeyState('S') & 0x8000) != 0;
        bool d = (GetAsyncKeyState('D') & 0x8000) != 0;
        double fwd    = (w ? 1.0 : 0.0) - (s ? 1.0 : 0.0);
        double strafe = (d ? 1.0 : 0.0) - (a ? 1.0 : 0.0);
        if (padMoveY != 0.0) fwd    = padMoveY;
        if (padMoveX != 0.0) strafe = padMoveX;
        bool moving = (fwd != 0.0 || strafe != 0.0);
        if (moving) {
            double yawDeg = ControlRotation(pawn).Yaw;
            double r = yawDeg * 3.14159265358979323846 / 180.0;
            double fX = std::cos(r), fY = std::sin(r);
            double rX = -std::sin(r), rY = std::cos(r);
            // Backward walking is naturally ~55% of forward speed in-game — mirror that.
            float fwdScale = (fwd < 0.0) ? (WALK_SCALE * 0.55f) : WALK_SCALE;
            if (fwd    != 0.0) AddMovement(pawn, fX, fY, (float)(fwd    * fwdScale));
            if (strafe != 0.0) AddMovement(pawn, rX, rY, (float)(strafe * WALK_SCALE));
        }
        MaybeFireFootstep(moving);
        // Force anim state overrides (in dialogue). Idle/dialogue flags always false; walking
        // flags true only when actually moving. Locomotion + shadow driven by numeric writes.
        if (inDlg) {
            // Call the game's own input-feed primitive with the raw WASD/stick vector.
            // Outside dialogue the game's input pipeline calls this every frame; in dialogue
            // it's gated, so anim state (Direction/Gait) never gets the correct directional
            // signal. Feeding it ourselves lets the game's natural locomotion pipeline drive
            // the anim graph — same class of bypass as the Wwise footstep fix.
            //   Pawn-relative convention: X=forward, Y=right (strafe), Z=0
            SetMoveVector(pawn, fwd, strafe, 0.0);
            ForceAnimState(moving);
            ForceLocomotionData(moving, fwd, strafe);
            ForceShadowAnimState(moving);
            ForceBhLocomotion(moving, fwd, strafe);
        }
        // Comparison logging: dumps current anim-instance values every 500ms in BOTH dialogue
        // and non-dialogue. Walk outside dialogue -> see what "correct walking" looks like;
        // walk inside dialogue -> compare our writes vs ground truth.
        LogAnimStateOnce(inDlg, moving);

        // ---- Look: mouse (smoothed) + right stick ----
        m_pending_dx += (double)g_dx.exchange(0);
        m_pending_dy += (double)g_dy.exchange(0);
        double dx = m_pending_dx * MOUSE_SMOOTH;
        double dy = m_pending_dy * MOUSE_SMOOTH;
        m_pending_dx -= dx;
        m_pending_dy -= dy;

        double mouseScale = BASE_MOUSE_SENS * g_mouseSensCoef.load(std::memory_order_relaxed);
        double yawVal   = dx * mouseScale * (g_invertMouseX.load() ? -1.0 : 1.0);
        double pitchVal = dy * mouseScale * (g_invertMouseY.load() ? -1.0 : 1.0);

        if (padLookX != 0.0 || padLookY != 0.0) {
            double padScale = PAD_LOOK_SCALE * g_padSensCoef.load(std::memory_order_relaxed);
            yawVal   +=  padLookX * padScale * (g_invertPadX.load() ? -1.0 : 1.0);
            pitchVal += -padLookY * padScale * (g_invertPadY.load() ? -1.0 : 1.0);
        }

        if (yawVal   != 0.0) AddYaw  (pawn, (float)yawVal);
        if (pitchVal != 0.0) AddPitch(pawn, (float)pitchVal);
    }
};

// ================= UE4SS entry points =================
#define IMMDLG_API __declspec(dllexport)
extern "C" {
    IMMDLG_API CppUserModBase* start_mod()   { return new ImmersiveDialogue(); }
    IMMDLG_API void            uninstall_mod(CppUserModBase* mod) { delete mod; }
}
