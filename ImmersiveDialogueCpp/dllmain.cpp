// ImmersiveDialogue (C++ UE4SS mod) - free movement + MOUSE look during interactive dialogue.
//
// While PC:IsInStaticDialog() is true:
//   - WASD walk (input direction rotated by control yaw). W/A/S/D are swallowed at the
//     WndProc layer so the dialogue widget's option list doesn't also scroll.
//   - Raw mouse look (Win32 Raw Input), release-fraction smoothed, scaled by the game's
//     MouseSensitivityCoef read from AppliedSettingsWin64.cfg, with InvertMouseYAxis
//     honored automatically.
//   - Escape TAP -> two synthetic Escapes (close dialogue + open pause menu).
//   - Escape HOLD (>=500ms) -> one synthetic Escape (close dialogue only).

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

#include <Windows.h>
#include <Xinput.h>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <string>

using namespace RC;
using namespace RC::Unreal;

// ---- UE5 math types are DOUBLE-based (STALKER 2 is UE5.5) ----
struct FVectorD  { double X, Y, Z; };
struct FRotatorD { double Pitch, Yaw, Roll; };

// ================= Shared state (WndProc thread <-> game thread) =================
static std::atomic<long>     g_dx{0};
static std::atomic<long>     g_dy{0};
static std::atomic<bool>     g_inDialogue{false};

// Escape hold/tap state
static std::atomic<uint64_t> g_escDownTime{0};       // GetTickCount64() at press; 0 = not pressed
static std::atomic<bool>     g_escSwallowed{false};  // we're currently swallowing the user's Esc
static std::atomic<int>      g_synthEscConsume{0};   // number of Esc key events to treat as ours (pass through)
static std::atomic<bool>     g_pauseTapPending{false}; // WndProc -> on_update: user tapped Esc
static std::atomic<uint64_t> g_lieDialogUntil{0};    // window during which IsInStaticDialog is forced to false
                                                     // (currently unused — tap-pause is parked)
static std::atomic<UObject*> g_hookPawn{nullptr};    // pawn the animation-state lie hooks fire for

// Game settings loaded from AppliedSettingsWin64.cfg (refreshed on each dialogue entry)
static std::atomic<double>   g_mouseSensCoef{1.0};
static std::atomic<double>   g_padSensCoef{1.0};
static std::atomic<bool>     g_invertMouseX{false};
static std::atomic<bool>     g_invertMouseY{false};
static std::atomic<bool>     g_invertPadX{false};
static std::atomic<bool>     g_invertPadY{false};

static WNDPROC g_origWndProc = nullptr;
static bool    g_rawReady    = false;

// Escape timing constants
static constexpr uint64_t ESC_TAP_MAX_MS   = 300;  // release before this -> tap
static constexpr uint64_t ESC_HOLD_MIN_MS  = 500;  // held past this -> hold
// (SYNTH_GUARD_MS removed — replaced with per-event consumption counter g_synthEscConsume)

// ================= Controller (XInput) with IAT-patched hook =================
// We patch the GAME's Import Address Table for XInputGetState so its polling sees zeroed
// left-stick values while g_inDialogue is true. Our own polling in this DLL uses a
// separate IAT (each module has its own) so we always get real values — no thread-local
// flag needed. IAT patching is way safer than inline hooking a native DLL export.
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
        auto name = reinterpret_cast<const char*>(
            reinterpret_cast<BYTE*>(hModule) + imp->Name);
        if (_stricmp(name, dllName) != 0) continue;
        auto thunk = reinterpret_cast<PIMAGE_THUNK_DATA>(
            reinterpret_cast<BYTE*>(hModule) + imp->FirstThunk);
        auto origThunk = reinterpret_cast<PIMAGE_THUNK_DATA>(
            reinterpret_cast<BYTE*>(hModule) + (imp->OriginalFirstThunk
                                                ? imp->OriginalFirstThunk
                                                : imp->FirstThunk));
        for (; origThunk->u1.AddressOfData != 0; ++origThunk, ++thunk) {
            if (origThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            auto ibn = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(
                reinterpret_cast<BYTE*>(hModule) + origThunk->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(ibn->Name), funcName) != 0) continue;
            DWORD oldProt = 0;
            if (!VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProt))
                return false;
            if (outOrig) *outOrig = reinterpret_cast<void*>(thunk->u1.Function);
            thunk->u1.Function = reinterpret_cast<uintptr_t>(newFunc);
            VirtualProtect(&thunk->u1.Function, sizeof(void*), oldProt, &oldProt);
            return true;
        }
    }
    return false;
}

static void InstallXInputHook() {
    HMODULE exe = GetModuleHandleW(nullptr); // main .exe
    void* orig = nullptr;
    const char* dlls[] = { "xinput1_4.dll", "XINPUT1_4.dll",
                           "xinput1_3.dll", "xinput9_1_0.dll" };
    for (auto* d : dlls) {
        if (PatchIATEntry(exe, d, "XInputGetState", (void*)&HookedXInputGetState, &orig)) {
            if (!g_realXInputGetState && orig)
                g_realXInputGetState = reinterpret_cast<XInputGetStateFn>(orig);
            g_xinputHooked = true;
        }
    }
}
static bool ReadPadSticks(double& outMoveX, double& outMoveY,
                          double& outLookX, double& outLookY,
                          WORD* outButtons /* nullable */) {
    outMoveX = outMoveY = outLookX = outLookY = 0.0;
    if (outButtons) *outButtons = 0;
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
            if (outButtons) *outButtons = st.Gamepad.wButtons;
            return true;
        }
    }
    return false;
}

// ================= Settings reader =================
// AppliedSettingsWin64.cfg is plain "Key = Value" lines; we only care about a few.
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
static void SendSyntheticEscape(int presses) {
    // Reserve exactly 2 pass-through slots per press (KEYDOWN + KEYUP). WndProc consumes
    // one per Esc event, so user's real held-Esc repeats after us are correctly swallowed.
    g_synthEscConsume.fetch_add(presses * 2, std::memory_order_relaxed);
    INPUT ins[4] = {};
    int n = 0;
    for (int i = 0; i < presses; ++i) {
        ins[n].type = INPUT_KEYBOARD; ins[n].ki.wVk = VK_ESCAPE; ins[n].ki.dwFlags = 0; n++;
        ins[n].type = INPUT_KEYBOARD; ins[n].ki.wVk = VK_ESCAPE; ins[n].ki.dwFlags = KEYEVENTF_KEYUP; n++;
    }
    SendInput(n, ins, sizeof(INPUT));
}

static LRESULT CALLBACK HookedWndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    // Raw mouse
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

    // Keyboard handling only during dialogue
    if ((msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP)
        && g_inDialogue.load(std::memory_order_relaxed)) {

        // Swallow W/A/S/D so the dialogue option list doesn't move; GetAsyncKeyState still sees them.
        if (w == 'W' || w == 'A' || w == 'S' || w == 'D') {
            return 0;
        }

        // Escape: hold/tap arbitration, but let our own synthetic Escapes through.
        if (w == VK_ESCAPE) {
            uint64_t now = GetTickCount64();
            // If this event was queued by our SendSyntheticEscape, consume one slot and pass
            // through to the game. Real user repeats never get miscounted as synth.
            int expected = g_synthEscConsume.load(std::memory_order_relaxed);
            while (expected > 0) {
                if (g_synthEscConsume.compare_exchange_weak(expected, expected - 1,
                                                             std::memory_order_relaxed)) {
                    return CallWindowProc(g_origWndProc, h, msg, w, l);
                }
            }
            if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
                bool isRepeat = (l & 0x40000000) != 0;
                if (!isRepeat && !g_escSwallowed.load(std::memory_order_relaxed)) {
                    g_escDownTime.store(now, std::memory_order_relaxed);
                    g_escSwallowed.store(true, std::memory_order_relaxed);
                }
                return 0; // swallow while we decide
            }
            if (msg == WM_KEYUP || msg == WM_SYSKEYUP) {
                if (g_escSwallowed.load(std::memory_order_relaxed)) {
                    g_escSwallowed.store(false, std::memory_order_relaxed);
                    uint64_t down = g_escDownTime.exchange(0, std::memory_order_relaxed);
                    uint64_t held = (down == 0) ? 0 : (now - down);
                    // Tap: don't send synthetic Esc (that would close dialogue). Instead
                    // hand off to on_update to toggle the game's pause state via reflection
                    // (freezes world without closing the dialogue view).
                    if (held < ESC_TAP_MAX_MS) {
                        g_pauseTapPending.store(true, std::memory_order_relaxed);
                    }
                    return 0;
                }
            }
        }
    }

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
    rid.usUsagePage = 0x01; // generic desktop
    rid.usUsage     = 0x02; // mouse
    rid.dwFlags     = RIDEV_INPUTSINK;
    rid.hwndTarget  = h;
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

    // tuning
    static constexpr double WALK_SCALE      = 0.15;  // 0..1 fraction of MaxWalkSpeed (which the game
                                                     // already adjusts via walk/sprint toggle state).
                                                     // Lower to walk slower, raise to walk faster.
    static constexpr double BASE_MOUSE_SENS = 0.10;  // baseline counts->input scalar; the game's
                                                     // MouseSensitivityCoef from the cfg multiplies this
                                                     // (0.10 * 0.687 ~= the 0.06 that felt smooth pre-cfg).
                                                     // Bigger values = per-frame yaw is larger, which also
                                                     // avoids the "notchy" feel at sub-pixel input.
    static constexpr double MOUSE_SMOOTH    = 0.5;   // release fraction per frame (1=raw, lower=smoother).

    // Controller (XInput) tuning. Right-stick unit deflection -> yaw/pitch input value per frame.
    // Left-stick is passed straight through as the movement scale, capped at WALK_SCALE.
    static constexpr double PAD_LOOK_SCALE  = 0.4;   // gamepad right stick -> yaw/pitch input per frame.
                                                     // The in-game GamepadSensitivityCoef from the cfg
                                                     // multiplies this, same layering as mouse.

    // mouse smoothing state
    double m_pending_dx = 0.0;
    double m_pending_dy = 0.0;

    // dialogue-entry edge detection for freshening settings
    bool m_prevInDialog = false;

    // hold-esc fire latch
    bool m_escHoldFired = false;

    // world-pause state (toggled by tap-Escape / pad Start button in dialogue)
    bool m_worldPaused = false;

    // controller Start-button edge detection (fire once per press)
    bool m_padStartPrev = false;

    ImmersiveDialogue() {
        ModName        = STR("ImmersiveDialogue");
        ModVersion     = STR("2.1");
        ModAuthors     = STR("Noah");
        ModDescription = STR("Free movement + mouse look during NPC dialogue.");
    }

    // We install a post-hook on PC::IsInStaticDialog. Earlier attempt to also hook
    // IsInteractionInProgress + IsInCinematic caused a launch crash — those UFunction
    // paths likely aren't resolvable on the PC class and RegisterHook doesn't fail
    // gracefully on missing functions. Sticking to the one hook that's proven to register.
    std::pair<int,int> m_hookIsInDialog{-1,-1};
    std::pair<int,int> m_hookIsRelaxIdle{-1,-1};
    void InstallStateLieHooks() {
        UnrealScriptFunctionCallable postFalseWhilePauseLie =
            [](UnrealScriptFunctionCallableContext& ctx, void*) {
                if (GetTickCount64() < g_lieDialogUntil.load(std::memory_order_relaxed)) {
                    ctx.SetReturnValue<bool>(false);
                }
            };
        m_hookIsInDialog = UObjectGlobals::RegisterHook(
            StringType(STR("/Script/Stalker2.PC:IsInStaticDialog")),
            UnrealScriptFunctionCallable{}, postFalseWhilePauseLie, nullptr);

        // Footsteps: dialogue system re-asserts IsStandToRelaxIdle=true each frame after our
        // setter. Hook the getter itself to always return false for our pawn while we're in
        // dialogue — anim graph then sees "not in relax-idle" and plays normal locomotion,
        // which fires the walk-cycle foot-IK notify that plays footstep audio.
        UnrealScriptFunctionCallable postRelaxIdleFalseForOurPawn =
            [](UnrealScriptFunctionCallableContext& ctx, void*) {
                if (ctx.Context == g_hookPawn.load(std::memory_order_relaxed)
                    && g_inDialogue.load(std::memory_order_relaxed)) {
                    ctx.SetReturnValue<bool>(false);
                }
            };
        m_hookIsRelaxIdle = UObjectGlobals::RegisterHook(
            StringType(STR("/Script/Stalker2.Obj:IsStandToRelaxIdle")),
            UnrealScriptFunctionCallable{}, postRelaxIdleFalseForOurPawn, nullptr);
    }

    auto on_unreal_init() -> void override {
        SetupInputHook();
        InstallXInputHook();
        LoadStalker2Settings();
        InstallStateLieHooks();
        Output::send<LogLevel::Verbose>(
            STR("[ImmDlg] unreal init (mouse hook={}, xinput hook={}, mouseSens={}, padSens={}, invertY={}, dlgHook={},{} relaxIdleHook={},{})\n"),
            g_rawReady      ? STR("ok") : STR("FAILED"),
            g_xinputHooked  ? STR("ok") : STR("SKIPPED"),
            g_mouseSensCoef.load(),
            g_padSensCoef.load(),
            g_invertMouseY.load() ? STR("true") : STR("false"),
            m_hookIsInDialog.first, m_hookIsInDialog.second,
            m_hookIsRelaxIdle.first, m_hookIsRelaxIdle.second);
    }

    UObject* GetPawn() {
        if (m_pawn && m_pawn->IsUnreachable() == false) return m_pawn;
        m_pawn = UObjectGlobals::FindFirstOf(STR("PC"));
        g_hookPawn.store(m_pawn, std::memory_order_relaxed); // let hooks filter by receiver
        return m_pawn;
    }

    // Read pawn.Controller directly (most reliable — FindFirstOf misses class-name variants).
    UObject* GetControllerViaProperty(UObject* pawn) {
        FProperty* prop = pawn->GetPropertyByNameInChain(STR("Controller"));
        if (!prop) return nullptr;
        UObject** slot = prop->ContainerPtrToValuePtr<UObject*>(pawn);
        return slot ? *slot : nullptr;
    }

    // FindFirstOf can't find CDOs. For library classes like UGameplayStatics (which only has
    // a CDO, no instances), use the full-path lookup — same pattern UE4SS's Lua binding uses.
    UObject* FindDefaultObject(const wchar_t* fullPath) {
        return UObjectGlobals::FindObject(nullptr, nullptr, fullPath, false);
    }

    // Get PlayerState — required to write into AWorldSettings.Pauser to freeze the world.
    UObject* GetPlayerStateViaProperty(UObject* pawn) {
        if (UObject* c = GetControllerViaProperty(pawn)) {
            if (FProperty* p = c->GetPropertyByNameInChain(STR("PlayerState"))) {
                if (UObject** s = p->ContainerPtrToValuePtr<UObject*>(c)) if (*s) return *s;
            }
        }
        if (FProperty* p = pawn->GetPropertyByNameInChain(STR("PlayerState"))) {
            if (UObject** s = p->ContainerPtrToValuePtr<UObject*>(pawn)) return *s;
        }
        return nullptr;
    }

    // Freeze / unfreeze the world by writing AWorldSettings.Pauser directly. This bypasses
    // APlayerController::SetPause's CanPause() virtual entirely — CanPause is what STALKER 2
    // clamps down on during dialogue. Setting Pauser to a non-null player state makes the
    // world's tick see itself as paused; setting to null unpauses.
    bool DirectWorldPause(UObject* pawn, bool paused) {
        UObject* ws = UObjectGlobals::FindFirstOf(STR("WorldSettings"));
        if (!ws) { Output::send<LogLevel::Verbose>(STR("[ImmDlg] DirectWorldPause: WorldSettings not found\n")); return false; }
        FProperty* prop = ws->GetPropertyByNameInChain(STR("Pauser"));
        if (!prop) prop = ws->GetPropertyByNameInChain(STR("PauserPlayerState"));
        if (!prop) { Output::send<LogLevel::Verbose>(STR("[ImmDlg] DirectWorldPause: Pauser property not found\n")); return false; }
        UObject** slot = prop->ContainerPtrToValuePtr<UObject*>(ws);
        if (!slot) return false;
        UObject* ps = paused ? GetPlayerStateViaProperty(pawn) : nullptr;
        *slot = ps;
        Output::send<LogLevel::Verbose>(STR("[ImmDlg] DirectWorldPause: set Pauser={}\n"),
                                         ps ? STR("PlayerState") : STR("null"));
        return true;
    }

    // Diagnostic: walk EVERY loaded UObject once and log those whose name or full path
    // suggests they're pause-menu related — WBP class objects, view widgets, IPUs, etc.
    // FindAllOf("Class") returns 0 in this UE4SS version so we go through ForEachUObject.
    bool m_menuClassesLogged = false;
    UObject* m_discoveredPauseWidgetClass = nullptr;
    void LogMenuClassesOnce() {
        if (m_menuClassesLogged) return;
        m_menuClassesLogged = true;
        int scanned = 0;
        int found = 0;
        UObjectGlobals::ForEachUObject([&](UObject* obj, int32_t, int32_t) -> LoopAction {
            scanned++;
            if (!obj) return LoopAction::Continue;
            StringType n = obj->GetName();
            bool interesting =
                n.find(STR("PauseMenu"))   != StringType::npos ||
                n.find(STR("PauseGame"))   != StringType::npos ||
                n.find(STR("MainMenu"))    != StringType::npos ||
                (n.find(STR("WBP_")) != StringType::npos &&
                    (n.find(STR("Menu")) != StringType::npos ||
                     n.find(STR("Pause")) != StringType::npos));
            if (!interesting) return LoopAction::Continue;
            StringType full = obj->GetFullName();
            Output::send<LogLevel::Verbose>(STR("[ImmDlg]   candidate: {}\n"), full);
            // The pause menu widget's UClass shows up as "WidgetBlueprintGeneratedClass /...".
            // Pick the first _C-suffixed one containing PauseMenu.
            bool isClassObj =
                full.find(STR("Class ")) == 0 ||
                full.find(STR("WidgetBlueprintGeneratedClass ")) == 0 ||
                full.find(STR("BlueprintGeneratedClass ")) == 0;
            if (!m_discoveredPauseWidgetClass
                && isClassObj
                && (n.find(STR("PauseMenu")) != StringType::npos || n.find(STR("PauseGame")) != StringType::npos)
                && n.find(STR("_C")) != StringType::npos) {
                m_discoveredPauseWidgetClass = obj;
                Output::send<LogLevel::Verbose>(STR("[ImmDlg]   -> using this as pause widget class\n"));
            }
            if (++found >= 80) { Output::send<LogLevel::Verbose>(STR("[ImmDlg]   (truncated at 80)\n")); return LoopAction::Break; }
            return LoopAction::Continue;
        });
        Output::send<LogLevel::Verbose>(STR("[ImmDlg] scanned {} UObjects, {} menu-like\n"), scanned, found);
    }

    bool m_pauseWidgetLogged = false;
    UObject* TrySpawnPauseMenuWidget(UObject* pawn) {
        UObject* widLib = FindDefaultObject(STR("/Script/UMG.Default__WidgetBlueprintLibrary"));
        UObject* pc     = GetControllerViaProperty(pawn);
        if (!widLib || !pc) return nullptr;

        UFunction* createFn = widLib->GetFunctionByNameInChain(FName(STR("Create")));
        if (!createFn) return nullptr;

        // Prefer whatever LogMenuClassesOnce discovered via the full-UObject scan; only if
        // that's null do we try the hand-written candidate names.
        UObject* widClass = m_discoveredPauseWidgetClass;
        const wchar_t* usedName = widClass ? STR("(from scan)") : nullptr;
        if (!widClass) {
            const wchar_t* candidates[] = {
                STR("/Game/GameLite/FPS_Game/UIRemaster/View/MenuMainView/W_PauseMenuMainView.W_PauseMenuMainView_C"),
                STR("W_PauseMenuMainView_C"),
                STR("W_PauseMenuSubViewRoot_C"),
                STR("/Script/Stalker2.PauseMenuMainView"),
                STR("/Script/Stalker2.PauseGameView"),
            };
            for (auto* n : candidates) {
                UObject* c = UObjectGlobals::FindObject(nullptr, nullptr, n, false);
                if (c) { widClass = c; usedName = n; break; }
            }
        }
        if (!m_pauseWidgetLogged) {
            m_pauseWidgetLogged = true;
            Output::send<LogLevel::Verbose>(
                STR("[ImmDlg] pause widget: using class '{}'\n"),
                usedName ? usedName : STR("NONE"));
        }
        if (!widClass) return nullptr;

        alignas(8) char buf[64] = {};
        *reinterpret_cast<UObject**>(buf + 0) = pc;
        *reinterpret_cast<UObject**>(buf + 8) = widClass;
        widLib->ProcessEvent(createFn, buf);
        UObject* widget = *reinterpret_cast<UObject**>(buf + 24);
        if (!widget) return nullptr;

        if (UFunction* addFn = widget->GetFunctionByNameInChain(FName(STR("AddToViewport")))) {
            struct { int32_t ZOrder; } addP{1000};
            widget->ProcessEvent(addFn, &addP);
        }
        if (UFunction* visFn = widget->GetFunctionByNameInChain(FName(STR("SetVisibility")))) {
            struct { uint8_t InVisibility; } visP{0};
            widget->ProcessEvent(visFn, &visP);
        }
        // Deliberately NOT SetInputMode_UIOnlyEx — that broke cursor lock last time.
        return widget;
    }
    UObject* m_spawnedPauseWidget = nullptr;

    // Diagnostic: on first dialogue entry, dump every UInputAction instance name to log.
    // Next iteration I use the list to find the dialogue-exit action + add a UInputTriggerHold
    // to its Triggers array (which makes the bottom-left keybind hint show the native fill
    // animation while you hold).
    bool m_inputActionsLogged = false;
    void LogInputActionsOnce() {
        if (m_inputActionsLogged) return;
        m_inputActionsLogged = true;
        std::vector<UObject*> actions;
        UObjectGlobals::FindAllOf(STR("InputAction"), actions);
        Output::send<LogLevel::Verbose>(STR("[ImmDlg] InputAction dump ({} total):\n"),
                                         (int)actions.size());
        int printed = 0;
        for (UObject* a : actions) {
            if (!a) continue;
            StringType name = a->GetName();
            Output::send<LogLevel::Verbose>(STR("[ImmDlg]   IA: {}\n"), name);
            if (++printed >= 200) { // cap so we don't dump 10k lines
                Output::send<LogLevel::Verbose>(STR("[ImmDlg]   ... (truncated)\n"));
                break;
            }
        }
    }

    // Cached pause-mechanism handles. Filled on first attempt so we don't re-lookup each toggle.
    UFunction* m_fnSetPause = nullptr;
    UObject*   m_pcForSetPause = nullptr;
    UFunction* m_fnSetGlobalTimeDilation = nullptr;
    UObject*   m_gsCdoForTimeDilation = nullptr;
    bool       m_pauseHandlesResolved = false;

    void ResolvePauseHandles(UObject* pawn) {
        if (m_pauseHandlesResolved) return;
        m_pauseHandlesResolved = true;

        // A) SetPause on player controller
        UObject* pc = GetControllerViaProperty(pawn);
        if (!pc) pc = UObjectGlobals::FindFirstOf(STR("Stalker2PlayerController"));
        if (!pc) pc = UObjectGlobals::FindFirstOf(STR("PlayerController"));
        if (pc) {
            m_pcForSetPause = pc;
            m_fnSetPause    = pc->GetFunctionByNameInChain(FName(STR("SetPause")));
        }

        // B) SetGlobalTimeDilation on UGameplayStatics CDO — bypasses APlayerController::CanPause
        //    gate that STALKER 2 clamps down on during dialogue (proven: SetPause fires but
        //    the world doesn't freeze; native Start button behaves the same). Time dilation
        //    of ~0 stops world tick while dialogue widget remains on screen.
        const wchar_t* paths[] = {
            STR("/Script/Engine.Default__GameplayStatics"),
            STR("Default__GameplayStatics"),
            STR("/Script/Engine.Default__KismetSystemLibrary"),
        };
        for (auto* p : paths) {
            if (UObject* cdo = FindDefaultObject(p)) {
                if (UFunction* fn = cdo->GetFunctionByNameInChain(FName(STR("SetGlobalTimeDilation")))) {
                    m_gsCdoForTimeDilation = cdo;
                    m_fnSetGlobalTimeDilation = fn;
                    break;
                }
            }
        }

        Output::send<LogLevel::Verbose>(
            STR("[ImmDlg] pause handles: SetPause={}, SetGlobalTimeDilation={}\n"),
            m_fnSetPause              ? STR("found") : STR("MISSING"),
            m_fnSetGlobalTimeDilation ? STR("found") : STR("MISSING"));
    }

    // Toggle world pause without closing the dialogue widget. Calls SetPause (may be blocked
    // by game's CanPause gate) AND SetGlobalTimeDilation (bypasses that gate) — whichever
    // takes effect first freezes the world. Returns a label for the log.
    const wchar_t* TryPauseGame(UObject* pawn, bool paused) {
        ResolvePauseHandles(pawn);
        bool sp = false, td = false;
        if (m_fnSetPause && m_pcForSetPause) {
            alignas(8) char buf[64] = {};
            buf[0] = paused ? 1 : 0;
            m_pcForSetPause->ProcessEvent(m_fnSetPause, buf);
            sp = true;
        }
        if (m_fnSetGlobalTimeDilation && m_gsCdoForTimeDilation) {
            alignas(8) char buf[32] = {};
            *reinterpret_cast<UObject**>(buf) = pawn;
            *reinterpret_cast<float*>(buf + 8) = paused ? 0.00001f : 1.0f;
            m_gsCdoForTimeDilation->ProcessEvent(m_fnSetGlobalTimeDilation, buf);
            td = true;
        }
        if (sp && td) return STR("SetPause+TimeDilation");
        if (td)       return STR("TimeDilation-only");
        if (sp)       return STR("SetPause-only");
        return nullptr;
    }

    // Break the character out of the "dialogue relaxed idle" animation state AND flip footsteps on
    // every frame. Without exiting the relax-idle pose, the anim graph doesn't play walk cycles,
    // so no foot-IK notify fires, so no footstep sound. Setting SetStandToRelaxIdle(false) each
    // tick keeps overriding whatever dialogue system pushes.
    UFunction* m_fnFootsteps       = nullptr;
    UFunction* m_fnSetRelaxIdle    = nullptr;
    UFunction* m_fnRelaxToStandDone = nullptr;
    UFunction* m_fnUpdateAnimInst   = nullptr;
    bool       m_animGatesResolved = false;
    void ForceWalkAnimatable(UObject* pawn) {
        if (!m_animGatesResolved) {
            m_animGatesResolved = true;
            m_fnFootsteps        = Fn(pawn, STR("SetFootstepsEnabled"));
            m_fnSetRelaxIdle     = Fn(pawn, STR("SetStandToRelaxIdle"));
            m_fnRelaxToStandDone = Fn(pawn, STR("SetRelaxToStandFinished"));
            m_fnUpdateAnimInst   = Fn(pawn, STR("UpdateObjAnimInstancesByReason"));
            Output::send<LogLevel::Verbose>(
                STR("[ImmDlg] anim gates: SetFootstepsEnabled={}, SetStandToRelaxIdle={}, SetRelaxToStandFinished={}, UpdateObjAnimInstancesByReason={}\n"),
                m_fnFootsteps         ? STR("found") : STR("MISSING"),
                m_fnSetRelaxIdle      ? STR("found") : STR("MISSING"),
                m_fnRelaxToStandDone  ? STR("found") : STR("MISSING"),
                m_fnUpdateAnimInst    ? STR("found") : STR("MISSING"));
        }
        if (m_fnFootsteps) {
            struct { bool NewEnabled; } p{true};
            pawn->ProcessEvent(m_fnFootsteps, &p);
        }
        // Force anim state machine: NOT in dialogue-idle AND transition-to-stand is finished.
        if (m_fnSetRelaxIdle) {
            struct { bool StandToRelaxIdle; } p{false};
            pawn->ProcessEvent(m_fnSetRelaxIdle, &p);
        }
        if (m_fnRelaxToStandDone) {
            struct { bool RelaxToStandFinished; } p{true};
            pawn->ProcessEvent(m_fnRelaxToStandDone, &p);
        }
    }

    static UFunction* Fn(UObject* o, const wchar_t* name) {
        return o->GetFunctionByNameInChain(FName(name));
    }

    bool CallBool(UObject* o, const wchar_t* name) {
        UFunction* fn = Fn(o, name);
        if (!fn) return false;
        struct { bool ReturnValue = false; } p;
        o->ProcessEvent(fn, &p);
        return p.ReturnValue;
    }

    FRotatorD ControlRotation(UObject* o) {
        FRotatorD out{0,0,0};
        UFunction* fn = Fn(o, STR("GetControlRotation"));
        if (!fn) return out;
        struct { FRotatorD ReturnValue; } p{{0,0,0}};
        o->ProcessEvent(fn, &p);
        return p.ReturnValue;
    }

    void ResetIgnore(UObject* o) {
        if (UFunction* f = Fn(o, STR("ResetIgnoreMoveInput"))) { char none[1]; o->ProcessEvent(f, none); }
        if (UFunction* f = Fn(o, STR("ResetIgnoreLookInput"))) { char none[1]; o->ProcessEvent(f, none); }
    }

    void AddMovement(UObject* o, double x, double y, float scale) {
        UFunction* fn = Fn(o, STR("AddMovementInput"));
        if (!fn) return;
        struct { FVectorD WorldDirection; float ScaleValue; bool bForce; } p{{x,y,0.0}, scale, false};
        o->ProcessEvent(fn, &p);
    }
    void AddYaw(UObject* o, float v) {
        UFunction* fn = Fn(o, STR("AddControllerYawInput"));
        if (!fn) return;
        struct { float Val; } p{v};
        o->ProcessEvent(fn, &p);
    }
    void AddPitch(UObject* o, float v) {
        UFunction* fn = Fn(o, STR("AddControllerPitchInput"));
        if (!fn) return;
        struct { float Val; } p{v};
        o->ProcessEvent(fn, &p);
    }

    auto on_update() -> void override {
        UObject* pawn = GetPawn();
        if (!pawn) return;
        // If we're inside our own IsInStaticDialog lie window, don't re-query — our own hook
        // would return false and we'd bail out of dialogue-mode logic mid-flight.
        bool inDlg;
        if (GetTickCount64() < g_lieDialogUntil.load(std::memory_order_relaxed)) {
            inDlg = m_prevInDialog;
        } else {
            inDlg = CallBool(pawn, STR("IsInStaticDialog"));
        }
        g_inDialogue.store(inDlg, std::memory_order_relaxed);

        if (!inDlg) {
            g_dx.exchange(0); g_dy.exchange(0);
            m_pending_dx = 0.0; m_pending_dy = 0.0;
            m_prevInDialog = false;
            m_escHoldFired = false;
            g_pauseTapPending.store(false, std::memory_order_relaxed);
            g_lieDialogUntil.store(0, std::memory_order_relaxed);
            // Auto-unpause if we paused earlier and dialogue has since ended
            if (m_worldPaused) {
                DirectWorldPause(pawn, false);
                m_worldPaused = false;
            }
            return;
        }

        // Freshen sensitivity + invert from cfg on entry to dialogue
        if (!m_prevInDialog) {
            LoadStalker2Settings();
            LogInputActionsOnce();
            LogMenuClassesOnce();
            m_prevInDialog = true;
        }

        // Tap-Escape: NO-OP. Feature dropped per user decision — spawning the real STALKER 2
        // pause menu (WBP_PauseMenuMainView) during dialogue proved intractable: the WBP
        // isn't reliably pre-loaded, spawning from the empty C++ parent renders nothing, and
        // the game's own pause pipeline refuses during dialogue at a level below the UFunction
        // reflection layer. Freezing world time alone is useless without menu buttons. Just
        // consume the flag so pad-Start / tap-Esc don't stack pending events.
        (void)g_pauseTapPending.exchange(false, std::memory_order_relaxed);

        // Hold-Escape latch: once we cross the hold threshold, fire close-dialogue and mark.
        if (g_escSwallowed.load(std::memory_order_relaxed) && !m_escHoldFired) {
            uint64_t down = g_escDownTime.load(std::memory_order_relaxed);
            if (down != 0 && (GetTickCount64() - down) >= ESC_HOLD_MIN_MS) {
                m_escHoldFired = true;
                SendSyntheticEscape(1); // one Esc -> close dialogue; do NOT open pause
                Output::send<LogLevel::Verbose>(STR("[ImmDlg] hold-Esc fired -> synthetic Esc -> game\n"));
            }
        }
        if (!g_escSwallowed.load(std::memory_order_relaxed)) {
            m_escHoldFired = false;
        }

        ResetIgnore(pawn);
        ForceWalkAnimatable(pawn); // flip footsteps flag AND break out of dialogue relax-idle so anim graph plays walk cycle

        // ---- read controller ONCE per frame (also used for movement/look below) ----
        double padMoveX = 0.0, padMoveY = 0.0, padLookX = 0.0, padLookY = 0.0;
        WORD padButtons = 0;
        ReadPadSticks(padMoveX, padMoveY, padLookX, padLookY, &padButtons);

        // Pad Start button -> same pause toggle path as tap-Escape (rising edge only).
        bool padStart = (padButtons & XINPUT_GAMEPAD_START) != 0;
        if (padStart && !m_padStartPrev) {
            g_pauseTapPending.store(true, std::memory_order_relaxed);
        }
        m_padStartPrev = padStart;

        // ---- movement: WASD (keyboard) + left stick (pad), whichever is active ----
        bool w = (GetAsyncKeyState('W') & 0x8000) != 0;
        bool a = (GetAsyncKeyState('A') & 0x8000) != 0;
        bool s = (GetAsyncKeyState('S') & 0x8000) != 0;
        bool d = (GetAsyncKeyState('D') & 0x8000) != 0;
        double fwd    = (w ? 1.0 : 0.0) - (s ? 1.0 : 0.0);
        double strafe = (d ? 1.0 : 0.0) - (a ? 1.0 : 0.0);
        // Pad overrides keyboard on an axis-by-axis basis (so mixed keyboard+pad still works).
        if (padMoveY != 0.0) fwd    = padMoveY;
        if (padMoveX != 0.0) strafe = padMoveX;
        if (fwd != 0.0 || strafe != 0.0) {
            double yawDeg = ControlRotation(pawn).Yaw;
            double r = yawDeg * 3.14159265358979323846 / 180.0;
            double fX = std::cos(r), fY = std::sin(r);
            double rX = -std::sin(r), rY = std::cos(r);
            if (fwd != 0.0)    AddMovement(pawn, fX, fY, (float)(fwd * WALK_SCALE));
            if (strafe != 0.0) AddMovement(pawn, rX, rY, (float)(strafe * WALK_SCALE));
        }

        // ---- look: mouse (smoothed) + right stick (frame-rate scaled), both apply ----
        m_pending_dx += (double)g_dx.exchange(0);
        m_pending_dy += (double)g_dy.exchange(0);
        double dx = m_pending_dx * MOUSE_SMOOTH;
        double dy = m_pending_dy * MOUSE_SMOOTH;
        m_pending_dx -= dx;
        m_pending_dy -= dy;

        double mouseScale = BASE_MOUSE_SENS * g_mouseSensCoef.load(std::memory_order_relaxed);
        double yawVal   = dx * mouseScale * (g_invertMouseX.load(std::memory_order_relaxed) ? -1.0 : 1.0);
        double pitchVal = dy * mouseScale * (g_invertMouseY.load(std::memory_order_relaxed) ? -1.0 : 1.0);

        // Right stick contribution. +Y on the stick means "look up" in XInput convention;
        // AddPitch takes positive-down by UE default, so subtract padLookY.
        if (padLookX != 0.0 || padLookY != 0.0) {
            double padScale = PAD_LOOK_SCALE * g_padSensCoef.load(std::memory_order_relaxed);
            yawVal   += padLookX * padScale * (g_invertPadX.load(std::memory_order_relaxed) ? -1.0 : 1.0);
            pitchVal += -padLookY * padScale * (g_invertPadY.load(std::memory_order_relaxed) ? -1.0 : 1.0);
        }

        if (yawVal   != 0.0) AddYaw(pawn,   (float)yawVal);
        if (pitchVal != 0.0) AddPitch(pawn, (float)pitchVal);
    }
};

// ================= UE4SS entry points =================
#define IMMDLG_API __declspec(dllexport)
extern "C" {
    IMMDLG_API CppUserModBase* start_mod()   { return new ImmersiveDialogue(); }
    IMMDLG_API void            uninstall_mod(CppUserModBase* mod) { delete mod; }
}
