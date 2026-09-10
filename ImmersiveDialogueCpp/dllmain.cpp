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

    // Fire a synthetic footstep sound via the pawn's AkComponent, at walk cadence.
    // Set MovementType + SurfaceMaterial + ArmorType + Wetness switches by UObject pointer
    // so Wwise's switch container picks the right variant (walk on dirt, no sprint, etc.).
    void MaybeFireFootstep(bool moving) {
        if (!m_ftFootstepEvent || !m_ftAkComponent || !m_ftPostEventFn) return;
        if (!moving) { m_ftNextStepAtMs = 0; return; }
        uint64_t now = GetTickCount64();
        if (m_ftNextStepAtMs == 0) { m_ftNextStepAtMs = now + FT_STEP_INTERVAL_MS / 2; return; }
        if (now < m_ftNextStepAtMs) return;
        m_ftNextStepAtMs = now + FT_STEP_INTERVAL_MS;

        SetAkSwitch(m_swWalk);
        SetAkSwitch(m_swMedium);
        SetAkSwitch(m_swDirt);
        SetAkSwitch(m_swDry);

        alignas(8) char buf[128] = {};
        *reinterpret_cast<UObject**>(buf) = m_ftFootstepEvent;
        m_ftAkComponent->ProcessEvent(m_ftPostEventFn, buf);
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

        // Diagnostic: dump EVERY property on the state_data struct (name + offset). Walk
        // parent structs too so inherited members show up. This tells us exactly what
        // the names are so future lookups match.
        int printed = 0;
        UStruct* s = stru;
        while (s) {
            for (FProperty* p : TFieldRange<FProperty>(s, EFieldIterationFlags::None)) {
                if (!p) continue;
                Output::send<LogLevel::Verbose>(STR("[ImmDlg]     state_data.{} off={}\n"),
                                                 p->GetName(), p->GetOffset_ForInternal());
                if (++printed >= 40) break;
            }
            if (printed >= 40) break;
            s = s->GetSuperStruct();
        }
    }

    void ForceStateDataOverrides(bool moving) {
        if (!m_stateDataProp || !m_animInstance) return;
        uint8_t* structBase = m_stateDataProp->ContainerPtrToValuePtr<uint8_t>(m_animInstance);
        if (!structBase) return;
        auto write = [&](int32_t off, bool val) {
            if (off < 0) return;
            *reinterpret_cast<bool*>(structBase + off) = val;
        };
        // Override flags: never jog/sprint/crouch/in-air/combat-idle in dialogue.
        write(m_offJoggingOverride,   false);
        write(m_offSprintingOverride, false);
        write(m_offCrouchingOverride, false);
        write(m_offInAirOverride,     false);
        write(m_offCombatMoveIdle,    false);
        write(m_offCombatCrouchIdle,  false);
        // Walking-override drives the state-machine transition to walk state.
        write(m_offWalkingOverride,   moving);
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
        ApplyFovPolicy(inDlg);

        // Hotkey polling every frame (works even outside dialogue).
        PollHotkeys();
        // Apply camera-centering-disable in dialogue if user has toggled it on.
        ApplyCameraCenteringToggle(inDlg);

        if (!inDlg) {
            g_dx.exchange(0); g_dy.exchange(0);
            m_pending_dx = 0.0; m_pending_dy = 0.0;
            m_prevInDialog = false;
            return;
        }

        // Refresh in-game sensitivity on each dialogue entry.
        if (!m_prevInDialog) {
            LoadStalker2Settings();
            ProbeFootstepAudio(pawn);
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
            if (fwd    != 0.0) AddMovement(pawn, fX, fY, (float)(fwd    * WALK_SCALE));
            if (strafe != 0.0) AddMovement(pawn, rX, rY, (float)(strafe * WALK_SCALE));
        }
        MaybeFireFootstep(moving);
        // Force anim state overrides (in dialogue). Idle/dialogue flags always false; walking
        // flags true only when actually moving.
        if (inDlg) ForceAnimState(moving);

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
