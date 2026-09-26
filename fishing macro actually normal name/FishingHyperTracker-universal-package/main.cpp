#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <mmsystem.h>
#include <objbase.h>
#include <gdiplus.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "fish_icons.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")

namespace {

constexpr wchar_t kClassName[] = L"FishingHyperTrackerWindow";
constexpr UINT WM_TRACKER_UPDATE = WM_APP + 1;
constexpr UINT WM_CALIBRATION_DONE = WM_APP + 2;
constexpr UINT WM_KEYBIND_DONE = WM_APP + 3;
constexpr int ID_TOGGLE = 1001;
constexpr int ID_EXIT = 1002;
constexpr int ID_CALIBRATE = 1003;
constexpr int ID_BIND_TOGGLE = 1004;
constexpr int ID_TAB = 1005;
constexpr int ID_BIND_QUIT = 1006;
constexpr int ID_WAIT_FISH_EDIT = 1007;
constexpr int ID_CALIBRATE_BAR = 1008;
constexpr int ID_CALIBRATE_EXIT = 1009;
constexpr int HOTKEY_TOGGLE = 1;
constexpr int HOTKEY_QUIT = 2;
// Shared by both "Calibrate Start Point" and the hotkey binder: whichever
// capture is in progress, Esc cancels it.
constexpr int HOTKEY_CANCEL_CAPTURE = 3;

// How often the tracker thread's telemetry actually repaints the HUD. The
// scan/control loop itself still runs at full speed (unrelated to this) -
// this only throttles how often the window redraws, which is what was
// causing the buttons to visibly flicker.
constexpr ULONGLONG kUiRefreshIntervalMs = 90;

enum class ControlState { Waiting, Holding, Releasing, Floating, Paused };

// The overall loop the tracker drives once a cycle is running:
//   Cast    -> a single click starts the next fishing minigame
//   Hook    -> existing bar-catching control, until the exit bar disappears
//   Collect -> a fixed pause after hooking ends
//   HoldKey -> holds the T key to collect/confirm, then loops back to Cast
enum class Phase { Cast, Hook, Collect, HoldKey, WaitForFish };

constexpr ULONGLONG kCollectDelayMs = 4000;
constexpr ULONGLONG kHoldKeyDurationMs = 4000;
// Require the exit bar to read as gone for this many consecutive frames
// (at roughly 250-330 scans/sec, so well under 50 ms) before treating
// hooking as actually finished - filters out a single dropped/misread
// capture frame so a brief flicker doesn't end the minigame early.
constexpr int kExitGoneDebounceFrames = 6;
// If the exit bar never appears at all within this long after a Cast
// click, the click almost certainly didn't register (rather than the fish
// just taking a while to bite - the exit bar is tied to the minigame
// opening, not to a bite). Rather than sit there waiting on a minigame
// that never started, automatically go back and retry the cast. This is
// what actually eliminates occasional dropped clicks, rather than just
// tuning timing and hoping.
constexpr ULONGLONG kCastConfirmTimeoutMs = 3000;
// Failsafe against a cast click landing more than once (e.g. a double-fire
// at the OS/game level) and the minigame appearing to start, end, and
// restart in a fast little loop: once a click is actually confirmed
// successful - the exit bar has appeared, meaning the reel really is out
// in the water - hold off on firing another cast click for this long,
// no matter what brings the state machine back around to Phase::Cast.
constexpr ULONGLONG kPostCastConfirmCooldownMs = 10000;
// Delay between finishing the collection step and sending the next cast.
// This is intentionally user-configurable because different rods can have
// different reel/return timing. It prevents the next cast click from landing
// immediately and accidentally restarting the reel.
constexpr ULONGLONG kDefaultWaitForFishMs = 3000;
constexpr ULONGLONG kMinWaitForFishMs = 0;
constexpr ULONGLONG kMaxWaitForFishMs = 60000;
// A dragged calibration region smaller than this in either dimension is
// almost certainly an accidental click rather than a real drag, and is
// rejected so it doesn't silently leave a near-zero-size capture region.
constexpr int kMinRegionSize = 6;

struct Telemetry {
    bool enabled = false;
    bool minigame = false;
    bool targetFound = false;
    bool playerFound = false;
    bool mouseDown = false;
    bool captureReady = false; // false until both bar and exit regions are calibrated
    float targetY = 0.0f;
    float playerY = 0.0f;
    float error = 0.0f;
    float fps = 0.0f;
    float phaseElapsedMs = 0.0f;
    float castCooldownRemainingMs = 0.0f; // >0 while the post-confirm failsafe is holding off the next cast click
    float waitForFishRemainingMs = 0.0f;
    int confidence = 0;
    ControlState state = ControlState::Paused;
    Phase phase = Phase::Cast;
};

std::atomic<bool> gQuit{false};
std::atomic<bool> gEnabled{false};
std::atomic<ULONGLONG> gWaitForFishMs{kDefaultWaitForFishMs};
std::mutex gTelemetryMutex;
Telemetry gTelemetry;
HWND gWindow = nullptr;
HWND gToggleButton = nullptr;
HWND gExitButton = nullptr;
HWND gWaitFishEdit = nullptr;
HWND gWaitFishLabel = nullptr;
HWND gWaitFishSecondsLabel = nullptr;
HWND gCalibrateButton = nullptr;
HWND gCalibrateBarButton = nullptr;
HWND gCalibrateExitButton = nullptr;
HWND gToggleBindButton = nullptr; // Keystrokes tab: rebind Start/Pause
HWND gExitBindButton = nullptr;   // Keystrokes tab: rebind Exit
HWND gTabControl = nullptr;
HFONT gUiFont = nullptr;
HBRUSH gBackgroundBrush = nullptr;
int gActiveTab = 0; // 0 = Fishing, 1 = Keystrokes, 2 = Calibration

// Which calibration is currently in progress, if any.
enum class CalibrationTarget { None, CastPoint, BarRegion, ExitRegion };
CalibrationTarget gCalibrationTarget = CalibrationTarget::None; // UI thread only
POINT gDragStart{};   // first corner of a region drag, set by the mouse hook
bool gDragging = false; // UI thread only; true between mousedown and mouseup

// Screen point the Cast phase clicks to start the next fishing minigame.
// Defaults to the primary screen's center; "Calibrate Start Point" lets the
// user click anywhere to set it instead.
std::mutex gCastPointMutex;
POINT gCastPoint{0, 0};
std::atomic<bool> gCalibrating{false};
HHOOK gMouseHook = nullptr;

// The top-level window the calibration click landed on. Used to force that
// window to the foreground for a moment before the Cast click and before
// holding the key, so both still land correctly even if the game isn't the
// active/focused window at the time.
std::atomic<HWND> gTargetWindow{nullptr};

// Fixed key the HoldKey phase holds down.
constexpr WORD kHoldKeyVk = static_cast<WORD>('T');

// The Start/Pause and Exit hotkeys, rebindable from the Keystrokes tab.
std::atomic<WORD> gToggleHotkeyVk{static_cast<WORD>(VK_F6)};
std::atomic<WORD> gQuitHotkeyVk{static_cast<WORD>(VK_F8)};
std::atomic<bool> gBindingHotkey{false};
int gBindingTarget = 0; // 0 = none, 1 = Start/Pause, 2 = Exit (UI thread only)
WORD gCapturedVk = 0;   // set by the hook, read once WM_KEYBIND_DONE arrives
HHOOK gKeyboardHook = nullptr;

// GDI+ is only used to decode/draw the two embedded LIVE/OFF fish-icon PNGs.
ULONG_PTR gGdiplusToken = 0;
Gdiplus::Bitmap* gFishOnIcon = nullptr;
Gdiplus::Bitmap* gFishOffIcon = nullptr;

Gdiplus::Bitmap* LoadPngFromMemory(const unsigned char* data, size_t size) {
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!hMem) return nullptr;
    void* mem = GlobalLock(hMem);
    if (!mem) {
        GlobalFree(hMem);
        return nullptr;
    }
    memcpy(mem, data, size);
    GlobalUnlock(hMem);

    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(hMem, TRUE, &stream) != S_OK) {
        // TRUE above means the stream owns hMem and frees it on release
        // even on this failure path, so no separate GlobalFree here.
        return nullptr;
    }
    Gdiplus::Bitmap* bitmap = Gdiplus::Bitmap::FromStream(stream);
    stream->Release();
    if (!bitmap) return nullptr;
    if (bitmap->GetLastStatus() != Gdiplus::Ok) {
        delete bitmap;
        return nullptr;
    }
    return bitmap;
}

POINT GetCastPoint() {
    std::lock_guard<std::mutex> lock(gCastPointMutex);
    return gCastPoint;
}

void SetCastPoint(POINT p) {
    std::lock_guard<std::mutex> lock(gCastPointMutex);
    gCastPoint = p;
}

// The fishing bar and exit-button regions, in absolute screen coordinates -
// calibrated live by dragging over them on whatever screen/resolution this
// particular user actually has, rather than assumed from any fixed
// reference resolution. {0,0,0,0} means "not calibrated yet".
std::mutex gBarRectMutex;
RECT gBarRect{0, 0, 0, 0};
std::mutex gExitRectMutex;
RECT gExitRect{0, 0, 0, 0};

RECT GetBarRect() {
    std::lock_guard<std::mutex> lock(gBarRectMutex);
    return gBarRect;
}

void SetBarRect(RECT r) {
    std::lock_guard<std::mutex> lock(gBarRectMutex);
    gBarRect = r;
}

RECT GetExitRect() {
    std::lock_guard<std::mutex> lock(gExitRectMutex);
    return gExitRect;
}

void SetExitRect(RECT r) {
    std::lock_guard<std::mutex> lock(gExitRectMutex);
    gExitRect = r;
}

bool RectCalibrated(const RECT& r) {
    return (r.right - r.left) >= kMinRegionSize && (r.bottom - r.top) >= kMinRegionSize;
}

// Human-readable name for a virtual-key code (e.g. "T", "Space", "F6").
std::wstring KeyDisplayName(WORD vk) {
    const UINT scan = MapVirtualKey(vk, MAPVK_VK_TO_VSC);
    const LONG lParam = static_cast<LONG>(scan) << 16;
    wchar_t buf[64] = L"";
    if (GetKeyNameTextW(lParam, buf, 64) == 0 || buf[0] == L'\0') {
        swprintf_s(buf, L"VK 0x%02X", vk);
    }
    return buf;
}

const wchar_t* StateText(ControlState state) {
    switch (state) {
        case ControlState::Waiting: return L"WAITING FOR FISH";
        case ControlState::Holding: return L"HOLDING / RISING";
        case ControlState::Releasing: return L"RELEASED / FALLING";
        case ControlState::Floating: return L"MICRO-PULSING";
        case ControlState::Paused: return L"PAUSED";
    }
    return L"UNKNOWN";
}

const wchar_t* PhaseText(Phase phase) {
    switch (phase) {
        case Phase::Cast: return L"CASTING";
        case Phase::Hook: return L"HOOKING";
        case Phase::Collect: return L"COLLECTING";
        case Phase::HoldKey: return L"HOLDING KEY";
        case Phase::WaitForFish: return L"WAIT FOR FISH";
    }
    return L"UNKNOWN";
}

void MouseButton(bool down) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    SendInput(1, &input, sizeof(input));
}

// Sleeps in short steps, bailing out early if the app is shutting down, so a
// long deliberate dwell (see CastClick below) doesn't delay Exit.
void InterruptibleSleep(ULONGLONG ms) {
    ULONGLONG remaining = ms;
    while (remaining > 0 && !gQuit.load()) {
        const ULONGLONG step = std::min<ULONGLONG>(remaining, 25);
        Sleep(static_cast<DWORD>(step));
        remaining -= step;
    }
}

// An explicit absolute MOUSEEVENTF_MOVE, on top of SetCursorPos: some games
// only react to a genuine mouse-move input event rather than the cursor
// simply teleporting, so CastClick sends both. Must be normalized against
// the FULL virtual desktop (all monitors, MOUSEEVENTF_VIRTUALDESK) rather
// than just the primary monitor - without that, this call was silently
// overriding SetCursorPos's correct placement with a wrong one whenever
// the calibrated point wasn't on the primary display, which is what was
// causing the click to land somewhere other than the actual start point.
void MoveMouseTo(int x, int y) {
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1);
    const int vh = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1);
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = static_cast<LONG>((x - vx) * 65535.0 / vw);
    input.mi.dy = static_cast<LONG>((y - vy) * 65535.0 / vh);
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    SendInput(1, &input, sizeof(input));
}

void ClickAt(POINT p) {
    POINT previous{};
    GetCursorPos(&previous);
    SetCursorPos(p.x, p.y);
    MouseButton(true);
    Sleep(35);
    MouseButton(false);
    SetCursorPos(previous.x, previous.y);
}

// Brings a window to the foreground/front even though we're a different,
// background process - a plain SetForegroundWindow call is usually blocked
// by Windows unless the calling thread's input state is attached to the
// currently-foreground thread's, so we do that attach/detach dance around
// it. This is what lets the Cast click and the held key actually reach the
// game even when it isn't the window the user currently has focused.
void ForceForeground(HWND target) {
    if (!target || !IsWindow(target)) return;
    const HWND current = GetForegroundWindow();
    if (current == target) return;
    const DWORD thisThread = GetCurrentThreadId();
    const DWORD curThread = current ? GetWindowThreadProcessId(current, nullptr) : 0;
    const DWORD tgtThread = GetWindowThreadProcessId(target, nullptr);
    const bool attachedCur = curThread && curThread != thisThread &&
                              AttachThreadInput(thisThread, curThread, TRUE);
    const bool attachedTgt = tgtThread && tgtThread != thisThread &&
                              AttachThreadInput(thisThread, tgtThread, TRUE);
    if (IsIconic(target)) ShowWindow(target, SW_RESTORE);
    SetForegroundWindow(target);
    BringWindowToTop(target);
    if (attachedTgt) AttachThreadInput(thisThread, tgtThread, FALSE);
    if (attachedCur) AttachThreadInput(thisThread, curThread, FALSE);
}

// The Cast phase's click specifically: firing a click the instant the
// cursor teleports (and right after force-focusing a window that may not
// have finished activating yet) was sometimes getting dropped by the game.
// This instead moves to the point, dwells there briefly so the game
// registers the hover/focus, performs a firm held click, then stays put
// there a while longer before restoring the cursor - yanking the cursor
// away immediately after mouse-up was apparently also causing missed
// clicks.
void CastClick(POINT p) {
    ForceForeground(gTargetWindow.load());

    POINT previous{};
    GetCursorPos(&previous);

    SetCursorPos(p.x, p.y);
    MoveMouseTo(p.x, p.y);
    InterruptibleSleep(250);

    MouseButton(true);
    InterruptibleSleep(50);
    MouseButton(false);

    // Stay put for a second after the click instead of immediately
    // yanking the cursor back - moving away right after mouse-up was
    // apparently letting the game miss the click sometimes.
    InterruptibleSleep(1000);

    SetCursorPos(previous.x, previous.y);
}

// Scan-code-based injection (KEYEVENTF_SCANCODE) rather than a bare virtual-
// key code: many games read keyboard state via DirectInput/raw input, which
// tracks scan codes and can miss a plain vk-code SendInput entirely - this
// is what was actually causing the T key to not register as held.
void KeyEvent(bool down, WORD vk) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = 0;
    input.ki.wScan = static_cast<WORD>(MapVirtualKey(vk, MAPVK_VK_TO_VSC));
    input.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &input, sizeof(input));
}

struct CaptureSurface {
    HDC screen = nullptr;
    HDC memory = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ oldBitmap = nullptr;
    uint32_t* pixels = nullptr;
    int width = 0;
    int height = 0;

    bool Create(int w, int h) {
        Destroy();
        width = w;
        height = h;
        screen = GetDC(nullptr);
        memory = CreateCompatibleDC(screen);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h; // top-down pixels
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS,
                                  reinterpret_cast<void**>(&pixels), nullptr, 0);
        if (!screen || !memory || !bitmap || !pixels) {
            Destroy();
            return false;
        }
        oldBitmap = SelectObject(memory, bitmap);
        return true;
    }

    bool Grab(int x, int y) {
        return BitBlt(memory, 0, 0, width, height, screen, x, y,
                      SRCCOPY | CAPTUREBLT) != FALSE;
    }

    void Destroy() {
        if (memory && oldBitmap) SelectObject(memory, oldBitmap);
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        if (screen) ReleaseDC(nullptr, screen);
        screen = nullptr;
        memory = nullptr;
        bitmap = nullptr;
        oldBitmap = nullptr;
        pixels = nullptr;
    }

    ~CaptureSurface() { Destroy(); }
};

inline void ReadRgb(uint32_t p, int& r, int& g, int& b) {
    b = static_cast<int>(p & 0xff);
    g = static_cast<int>((p >> 8) & 0xff);
    r = static_cast<int>((p >> 16) & 0xff);
}

// The game applies lighting/transparency, so these predicates deliberately
// recognize the displayed color family instead of requiring one exact RGB.
bool IsTargetGreen(uint32_t p) {
    int r, g, b;
    ReadRgb(p, r, g, b);
    const int maxOther = std::max(r, b);
    const bool dominantGreen = g >= 75 && g - maxOther >= 15 && g >= r * 11 / 10;
    const int dr = r - 60, dg = g - 200, db = b - 80;
    const bool nearReference = dr * dr + dg * dg + db * db <= 95 * 95;
    return dominantGreen || nearReference;
}

// The target zone turns from green to a yellow-green "warning" color after
// the player block has been outside it too long. Sampled reference points:
// a bright gold border around RGB(215,196,5) and a dimmer, semi-transparent
// olive fill around RGB(62,68,28) - very different brightness, but both
// land in the same yellow-to-green hue band (~50-70 degrees), clearly
// separate from the normal target's pure green (~120-140 degrees). Hue is
// used instead of a fixed RGB distance so this still matches regardless of
// how bright/transparent the fill happens to render.
bool IsTargetWarning(uint32_t p) {
    int r, g, b;
    ReadRgb(p, r, g, b);
    const int cmax = std::max({r, g, b});
    const int cmin = std::min({r, g, b});
    const int delta = cmax - cmin;
    if (delta < 12 || cmax < 30) return false; // too gray or too dark to tell
    double hue;
    if (cmax == r) {
        hue = 60.0 * std::fmod((static_cast<double>(g - b) / delta), 6.0);
        if (hue < 0.0) hue += 360.0;
    } else if (cmax == g) {
        hue = 60.0 * ((static_cast<double>(b - r) / delta) + 2.0);
    } else {
        hue = 60.0 * ((static_cast<double>(r - g) / delta) + 4.0);
    }
    const double saturation = static_cast<double>(delta) / cmax;
    return hue >= 35.0 && hue <= 95.0 && saturation >= 0.35;
}

bool IsPlayerWhite(uint32_t p) {
    int r, g, b;
    ReadRgb(p, r, g, b);
    const int hi = std::max({r, g, b});
    const int lo = std::min({r, g, b});
    // Allows the green target to tint the white block when they overlap.
    return r >= 168 && g >= 178 && b >= 145 && hi - lo <= 92;
}

bool IsExitRed(uint32_t p) {
    int r, g, b;
    ReadRgb(p, r, g, b);
    const int dr = r - 190, dg = g - 30, db = b - 30;
    return (r >= 120 && r >= g * 2 && r >= b * 2) ||
           (dr * dr + dg * dg + db * db <= 85 * 85);
}

struct Detection {
    bool found = false;
    float center = 0.0f;
    int top = 0;
    int bottom = 0;
    int score = 0;
};

Detection StrongestRun(const std::vector<int>& rowScore, int minimumPerRow,
                       int minimumRows) {
    Detection best{};
    int runTop = -1;
    int runScore = 0;
    for (int y = 0; y <= static_cast<int>(rowScore.size()); ++y) {
        const bool active = y < static_cast<int>(rowScore.size()) &&
                            rowScore[y] >= minimumPerRow;
        if (active) {
            if (runTop < 0) runTop = y;
            runScore += rowScore[y];
        } else if (runTop >= 0) {
            const int runBottom = y - 1;
            if (runBottom - runTop + 1 >= minimumRows && runScore > best.score) {
                best.found = true;
                best.top = runTop;
                best.bottom = runBottom;
                best.center = (runTop + runBottom) * 0.5f;
                best.score = runScore;
            }
            runTop = -1;
            runScore = 0;
        }
    }
    return best;
}

bool ExitVisible(CaptureSurface& exitSurface, int x, int y) {
    if (!exitSurface.Grab(x, y)) return false;
    int red = 0;
    const int total = exitSurface.width * exitSurface.height;
    for (int i = 0; i < total; ++i) red += IsExitRed(exitSurface.pixels[i]);
    return red >= std::max(24, total / 35);
}

void FindBarObjects(const CaptureSurface& surface, Detection& target,
                    Detection& player) {
    std::vector<int> green(surface.height, 0);
    std::vector<int> white(surface.height, 0);
    for (int y = 0; y < surface.height; ++y) {
        for (int x = 0; x < surface.width; ++x) {
            const uint32_t p = surface.pixels[y * surface.width + x];
            green[y] += (IsTargetGreen(p) || IsTargetWarning(p));
            white[y] += IsPlayerWhite(p);
        }
    }
    target = StrongestRun(green, std::max(5, surface.width * 28 / 100), 5);
    player = StrongestRun(white, std::max(3, surface.width * 7 / 100), 4);
}

DWORD WINAPI TrackerThread(void*) {
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    // Bar/exit regions are calibrated live (see the Calibration tab) rather
    // than assumed from any fixed reference resolution, so this works the
    // same regardless of the user's monitor/resolution/game window size.
    RECT bar = GetBarRect();
    RECT exitRect = GetExitRect();
    CaptureSurface barSurface;
    CaptureSurface exitSurface;
    bool barReady = RectCalibrated(bar) &&
        barSurface.Create(bar.right - bar.left, bar.bottom - bar.top);
    bool exitReady = RectCalibrated(exitRect) &&
        exitSurface.Create(exitRect.right - exitRect.left, exitRect.bottom - exitRect.top);

    bool mouseDown = false;
    bool hadPlayer = false;
    float previousPlayer = 0.0f;
    float playerVelocity = 0.0f;
    float previousTarget = 0.0f;
    float targetVelocity = 0.0f;
    double pwmPhaseMs = 0.0;
    ULONGLONG previousTick = GetTickCount64();
    ULONGLONG lastUiTick = 0;
    ULONGLONG holdStarted = 0;
    int missingFrames = 0;
    float fpsAverage = 0.0f;

    auto setMouse = [&](bool down) {
        if (mouseDown != down) {
            MouseButton(down);
            mouseDown = down;
            if (down) holdStarted = GetTickCount64();
        }
    };

    Phase phase = Phase::Cast;
    bool sawExitDuringHook = false;
    bool exitSeenWhileWaiting = false;
    int exitGoneFrames = 0; // consecutive frames the exit bar has read as gone, once it's been seen
    bool keyDown = false;
    ULONGLONG phaseChangedAt = GetTickCount64();
    HWND cycleUserWindow = nullptr; // foreground window just before this cycle's cast click
    // 0 means "no confirmed cast yet" - never blocks the very first cast of a run.
    ULONGLONG lastCastConfirmedAt = 0;

    while (!gQuit.load()) {
        const ULONGLONG now = GetTickCount64();
        const double dtMs = std::clamp<double>(now - previousTick, 1.0, 30.0);
        previousTick = now;
        const float instantFps = static_cast<float>(1000.0 / dtMs);
        fpsAverage += (instantFps - fpsAverage) * 0.08f;

        // Pick up any (re)calibration of the bar/exit regions - recreate the
        // capture surface only when the size actually changed, since that's
        // the only part CaptureSurface::Create needs to redo; the position
        // is read fresh from `bar`/`exitRect` every Grab() call already.
        {
            const RECT freshBar = GetBarRect();
            if (freshBar.right - freshBar.left != bar.right - bar.left ||
                freshBar.bottom - freshBar.top != bar.bottom - bar.top) {
                barReady = RectCalibrated(freshBar) &&
                    barSurface.Create(freshBar.right - freshBar.left, freshBar.bottom - freshBar.top);
            } else {
                barReady = RectCalibrated(freshBar);
            }
            bar = freshBar;

            const RECT freshExit = GetExitRect();
            if (freshExit.right - freshExit.left != exitRect.right - exitRect.left ||
                freshExit.bottom - freshExit.top != exitRect.bottom - exitRect.top) {
                exitReady = RectCalibrated(freshExit) &&
                    exitSurface.Create(freshExit.right - freshExit.left, freshExit.bottom - freshExit.top);
            } else {
                exitReady = RectCalibrated(freshExit);
            }
            exitRect = freshExit;
        }
        const bool captureReady = barReady && exitReady;

        Telemetry current{};
        current.enabled = gEnabled.load();
        current.fps = fpsAverage;
        current.captureReady = captureReady;
        current.state = current.enabled ? ControlState::Waiting : ControlState::Paused;

        if (!current.enabled || !captureReady) {
            // Disabled (or capture unavailable): release everything and reset
            // the cycle so the next Start always begins from a fresh cast.
            setMouse(false);
            if (keyDown) {
                KeyEvent(false, kHoldKeyVk);
                keyDown = false;
            }
            phase = Phase::Cast;
            sawExitDuringHook = false;
            exitSeenWhileWaiting = false;
            exitGoneFrames = 0;
            missingFrames = 0;
            hadPlayer = false;
            playerVelocity = targetVelocity = 0.0f;
            pwmPhaseMs = 0.0;
            phaseChangedAt = now;
            lastCastConfirmedAt = 0; // a fresh Start shouldn't inherit a cooldown from before the pause
            current.phase = phase;
            current.mouseDown = mouseDown;
            {
                std::lock_guard<std::mutex> lock(gTelemetryMutex);
                gTelemetry = current;
            }
            if (now - lastUiTick >= kUiRefreshIntervalMs) {
                lastUiTick = now;
                PostMessage(gWindow, WM_TRACKER_UPDATE, 0, 0);
            }
            Sleep(2);
            continue;
        }

        current.phase = phase;

        if (phase == Phase::WaitForFish) {
            // Cast -> WaitForFish -> Hook. No second cast is possible here.
            // The configurable timer is a minimum safety window: we can see
            // the red Exit button while waiting, but we do not hand control
            // to the reel/bar logic until that window has elapsed.
            setMouse(false);
            const ULONGLONG waitMs = gWaitForFishMs.load();
            const ULONGLONG elapsed = now - phaseChangedAt;
            current.phaseElapsedMs = static_cast<float>(elapsed);

            const bool exitVisible = ExitVisible(exitSurface, exitRect.left, exitRect.top);
            if (exitVisible) exitSeenWhileWaiting = true;

            current.waitForFishRemainingMs = elapsed < waitMs
                ? static_cast<float>(waitMs - elapsed) : 0.0f;

            if (exitSeenWhileWaiting) {
                // The red Exit button is the real signal that the fishing
                // minigame has started. The configurable timer is ONLY a
                // safety net to prevent an immediate second cast; it must not
                // add unnecessary delay once the minigame is actually ready.
                // As soon as Exit is detected, end WaitForFish immediately.
                phase = Phase::Hook;
                current.phase = phase;
                sawExitDuringHook = true;
                exitGoneFrames = 0;
                missingFrames = 0;
                hadPlayer = false;
                playerVelocity = targetVelocity = 0.0f;
                pwmPhaseMs = 0.0;
                phaseChangedAt = now;
                lastCastConfirmedAt = now;
            } else if (elapsed >= waitMs) {
                // The safety window ran out and the exit bar never showed up
                // at all - the cast click almost certainly didn't register.
                // Go back and retry it rather than sit here forever waiting
                // on a minigame that never actually started.
                setMouse(false);
                exitSeenWhileWaiting = false;
                phase = Phase::Cast;
                current.phase = phase;
                phaseChangedAt = now;
            }
        } else if (phase == Phase::Cast) {
            const ULONGLONG sinceConfirmed = now - lastCastConfirmedAt;
            if (lastCastConfirmedAt != 0 && sinceConfirmed < kPostCastConfirmCooldownMs) {
                // Failsafe: a click was already confirmed successful recently
                // (the exit bar actually appeared, so the reel really did go
                // out) - whatever brought us back to Phase::Cast this soon
                // after that, hold off on firing another click rather than
                // risk a double-cast that starts/ends/restarts the minigame.
                // Just wait it out; nothing else to do here.
                setMouse(false);
                current.castCooldownRemainingMs =
                    static_cast<float>(kPostCastConfirmCooldownMs - sinceConfirmed);
            } else {
                // Guide the cursor to the point, dwell there, then click firmly -
                // the exit bar appearing (handled in the Hook phase below)
                // confirms the minigame actually started.
                cycleUserWindow = GetForegroundWindow();
                setMouse(false);
                CastClick(GetCastPoint());
                // Casting is a one-way transition into WaitForFish. No
                // additional click is permitted until the red Exit button
                // proves that the fish/minigame has started.
                phase = Phase::WaitForFish;
                current.phase = phase;
                sawExitDuringHook = false;
                exitSeenWhileWaiting = false;
                exitGoneFrames = 0;
                missingFrames = 0;
                hadPlayer = false;
                playerVelocity = targetVelocity = 0.0f;
                pwmPhaseMs = 0.0;
                phaseChangedAt = now;
            }
        } else if (phase == Phase::Hook) {
            const bool exitVisible = ExitVisible(exitSurface, exitRect.left, exitRect.top);
            Detection target{}, player{};
            if (barSurface.Grab(bar.left, bar.top)) FindBarObjects(barSurface, target, player);
            if (exitVisible) {
                if (!sawExitDuringHook) {
                    // First sighting of the exit bar this cycle: the click is
                    // now actually confirmed successful (the reel is out in
                    // the water), which starts the post-cast cooldown above.
                    lastCastConfirmedAt = now;
                }
                sawExitDuringHook = true;
                exitGoneFrames = 0;
            } else if (sawExitDuringHook) {
                ++exitGoneFrames;
            }

            current.targetFound = target.found;
            current.playerFound = player.found;
            current.minigame = exitVisible && target.found;
            current.targetY = target.center;
            current.playerY = player.center;
            current.confidence = target.found
                ? std::clamp(target.score * 100 /
                             std::max(1, barSurface.width * (target.bottom - target.top + 1)), 0, 100)
                : 0;

            if (!exitVisible || !target.found) {
                setMouse(false);
                missingFrames = 0;
                hadPlayer = false;
                playerVelocity = targetVelocity = 0.0f;
                pwmPhaseMs = 0.0;
            } else if (!player.found) {
                // Never continue a blind hold. A few missing frames are tolerated,
                // then recovery alternates short nudges rather than sticking at an edge.
                ++missingFrames;
                if (missingFrames <= 2) {
                    setMouse(mouseDown);
                } else {
                    const bool recoveryPulse = ((now / 18) % 3) != 2;
                    setMouse(recoveryPulse);
                    current.state = recoveryPulse ? ControlState::Holding : ControlState::Releasing;
                }
            } else {
                missingFrames = 0;
                const float alpha = 0.34f;
                const float rawPlayerVelocity = hadPlayer
                    ? (player.center - previousPlayer) / static_cast<float>(dtMs) : 0.0f;
                const float rawTargetVelocity = hadPlayer
                    ? (target.center - previousTarget) / static_cast<float>(dtMs) : 0.0f;
                playerVelocity += (rawPlayerVelocity - playerVelocity) * alpha;
                targetVelocity += (rawTargetVelocity - targetVelocity) * 0.28f;
                previousPlayer = player.center;
                previousTarget = target.center;
                hadPlayer = true;

                // Predict 14 ms ahead so the player block brakes before crossing a moving zone.
                const float desired = std::clamp(target.center + targetVelocity * 14.0f,
                                                 static_cast<float>(target.top + 2),
                                                 static_cast<float>(target.bottom - 2));
                const float error = player.center - desired; // positive = player is below target
                const float relativeVelocity = playerVelocity - targetVelocity;
                current.error = error;
                const float targetHalf = std::max(6.0f, (target.bottom - target.top + 1) * 0.5f);
                const float safeHalf = std::max(3.0f, targetHalf -
                    std::max(3.0f, (player.bottom - player.top + 1) * 0.45f));
                const bool nearTop = player.top <= 3;
                const bool nearBottom = player.bottom >= barSurface.height - 4;

                bool commandDown = false;
                if (nearTop) {
                    commandDown = false;
                    current.state = ControlState::Releasing;
                } else if (nearBottom) {
                    commandDown = true;
                    current.state = ControlState::Holding;
                } else if (error > safeHalf * 0.80f) {
                    commandDown = true;
                    current.state = ControlState::Holding;
                } else if (error < -safeHalf * 0.80f) {
                    commandDown = false;
                    current.state = ControlState::Releasing;
                } else {
                    // Binary PID converted to 18 ms PWM. The 0.52 hover term is
                    // continuously corrected by position and vertical momentum.
                    float duty = 0.52f + 0.070f * error + 0.095f * relativeVelocity;
                    if (error > 0.0f) duty += 0.035f;
                    if (error < 0.0f) duty -= 0.035f;
                    duty = std::clamp(duty, 0.08f, 0.92f);
                    constexpr double periodMs = 18.0;
                    pwmPhaseMs = std::fmod(pwmPhaseMs + dtMs, periodMs);
                    commandDown = pwmPhaseMs < periodMs * duty;
                    current.state = ControlState::Floating;
                }

                // Safety release: long rises get a tiny braking gap, preventing a
                // lost frame from pinning the block at the top.
                if (commandDown && mouseDown && now - holdStarted >= 135) {
                    commandDown = false;
                    pwmPhaseMs = 16.0;
                }
                setMouse(commandDown);
            }

            if (sawExitDuringHook && exitGoneFrames >= kExitGoneDebounceFrames) {
                // The exit bar has reliably disappeared (not just one flickered
                // frame) - that marks the end of hooking; wait out the collect
                // delay before the hold-key press.
                setMouse(false);
                phase = Phase::Collect;
                current.phase = phase;
                phaseChangedAt = now;
            }
        } else if (phase == Phase::Collect) {
            setMouse(false);
            current.phaseElapsedMs = static_cast<float>(now - phaseChangedAt);
            if (now - phaseChangedAt >= kCollectDelayMs) {
                phase = Phase::HoldKey;
                current.phase = phase;
                phaseChangedAt = now;
                ForceForeground(gTargetWindow.load());
                KeyEvent(true, kHoldKeyVk);
                keyDown = true;
            }
        } else if (phase == Phase::HoldKey) {
            current.phaseElapsedMs = static_cast<float>(now - phaseChangedAt);
            if (now - phaseChangedAt >= kHoldKeyDurationMs) {
                KeyEvent(false, kHoldKeyVk);
                keyDown = false;
                // Give focus back to whatever the user was on before this
                // cycle's cast click, rather than leaving the game focused.
                if (cycleUserWindow && cycleUserWindow != gTargetWindow.load()) {
                    ForceForeground(cycleUserWindow);
                }
                // Claiming is done - restart the whole cycle with a real
                // cast click. Going to WaitForFish here was the bug: nothing
                // in that phase ever fires a new cast, so if the exit bar
                // happened to still read as visible for a moment right after
                // claiming (a leftover from the previous minigame fading
                // out), it could fall straight through WaitForFish -> Hook
                // -> Collect -> HoldKey again without ever actually
                // re-casting, looping between Collect and HoldKey forever.
                phase = Phase::Cast;
                current.phase = phase;
                phaseChangedAt = now;
            }
        }

        current.mouseDown = mouseDown;
        {
            std::lock_guard<std::mutex> lock(gTelemetryMutex);
            gTelemetry = current;
        }
        if (now - lastUiTick >= kUiRefreshIntervalMs) {
            lastUiTick = now;
            PostMessage(gWindow, WM_TRACKER_UPDATE, 0, 0);
        }
        Sleep(2); // approximately 250-330 observations/sec including capture time
    }

    setMouse(false);
    if (keyDown) KeyEvent(false, kHoldKeyVk);
    timeEndPeriod(1);
    return 0;
}

void DrawTextLine(HDC dc, const wchar_t* text, int x, int y, COLORREF color,
                  int size, bool bold = false) {
    LOGFONT lf{};
    lf.lfHeight = -MulDiv(size, GetDeviceCaps(dc, LOGPIXELSY), 72);
    lf.lfWeight = bold ? FW_BOLD : FW_NORMAL;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    HFONT font = CreateFontIndirect(&lf);
    const HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    TextOut(dc, x, y, text, static_cast<int>(wcslen(text)));
    SelectObject(dc, old);
    DeleteObject(font);
}

void PaintHud(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC windowDc = BeginPaint(hwnd, &ps);
    RECT client{};
    GetClientRect(hwnd, &client);

    // Double-buffered: draw everything into an off-screen bitmap first, then
    // blit it to the window in one go. Painting many separate GDI primitives
    // directly to the window DC is what was causing the visible flicker
    // (most noticeable on the buttons sitting on top of this translucent
    // window), since the window could otherwise be shown mid-draw.
    HDC dc = CreateCompatibleDC(windowDc);
    HBITMAP memBitmap = CreateCompatibleBitmap(windowDc, client.right, client.bottom);
    HGDIOBJ oldBitmap = SelectObject(dc, memBitmap);

    FillRect(dc, &client, gBackgroundBrush);

    Telemetry t;
    {
        std::lock_guard<std::mutex> lock(gTelemetryMutex);
        t = gTelemetry;
    }

    const COLORREF accent = RGB(63, 220, 113);
    const COLORREF muted = RGB(148, 160, 177);
    const COLORREF white = RGB(241, 245, 249);
    HPEN pen = CreatePen(PS_SOLID, 2, accent);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, 0, 48, nullptr);
    LineTo(dc, client.right, 48);
    SelectObject(dc, oldPen);
    DeleteObject(pen);

    DrawTextLine(dc, L"FISHING HYPER TRACKER", 18, 13, white, 14, true);
    {
        Gdiplus::Bitmap* icon = t.enabled ? gFishOnIcon : gFishOffIcon;
        if (icon) {
            Gdiplus::Graphics graphics(dc);
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            constexpr int kIconSize = 28;
            const int iconX = client.right - kIconSize - 14;
            const int iconY = 10;
            graphics.DrawImage(icon, iconX, iconY, kIconSize, kIconSize);
        }
    }

    wchar_t line[160];
    if (gActiveTab == 0) {
        if (!t.captureReady) {
            DrawTextLine(dc, L"NOT CALIBRATED", 18, 93, RGB(248, 113, 113), 12, true);
            DrawTextLine(dc, L"Set up the Fishing Bar and Exit Button", 18, 121, white, 10);
            DrawTextLine(dc, L"regions on the Calibration tab first.", 18, 140, muted, 9);
        } else {
            const wchar_t* phaseHeadline = (t.phase == Phase::Hook) ? StateText(t.state) : PhaseText(t.phase);
            DrawTextLine(dc, phaseHeadline, 18, 93,
                         (t.phase == Phase::Hook && t.minigame) ? accent : muted, 12, true);

            if (t.phase == Phase::Hook) {
                if (t.targetFound && t.playerFound) {
                    swprintf_s(line, L"Target %.1f   Player %.1f   Error %+.1f px",
                               t.targetY, t.playerY, t.error);
                } else {
                    swprintf_s(line, L"Target %s   Player %s",
                               t.targetFound ? L"FOUND" : L"---",
                               t.playerFound ? L"FOUND" : L"---");
                }
            } else if (t.phase == Phase::Collect) {
                swprintf_s(line, L"Waiting to collect: %.1fs / %.0fs",
                           t.phaseElapsedMs / 1000.0f, kCollectDelayMs / 1000.0f);
            } else if (t.phase == Phase::HoldKey) {
                swprintf_s(line, L"Holding T: %.1fs / %.0fs",
                           t.phaseElapsedMs / 1000.0f, kHoldKeyDurationMs / 1000.0f);
            } else if (t.phase == Phase::WaitForFish) {
                const float elapsed = t.phaseElapsedMs / 1000.0f;
                const float configured = gWaitForFishMs.load() / 1000.0f;
                if (t.waitForFishRemainingMs > 0.0f) {
                    swprintf_s(line, L"Waiting for fish... %.1fs safety delay remaining",
                               t.waitForFishRemainingMs / 1000.0f);
                } else {
                    swprintf_s(line, L"Waiting for fish... red Exit detected, starting reel", elapsed);
                }
            } else if (t.castCooldownRemainingMs > 0.0f) {
                swprintf_s(line, L"Post-cast failsafe: %.1fs left before next click",
                           t.castCooldownRemainingMs / 1000.0f);
            } else {
                swprintf_s(line, L"Clicking to cast...");
            }
            DrawTextLine(dc, line, 18, 121, white, 10);
            swprintf_s(line, L"Detection %d%%    Scan %.0f FPS    Mouse %s",
                       t.confidence, t.fps, t.mouseDown ? L"DOWN" : L"UP");
            DrawTextLine(dc, line, 18, 146, muted, 9);
        }

        if (gCalibrating.load()) {
            DrawTextLine(dc, L"CALIBRATING - see Calibration tab  (Esc cancels)",
                         18, 167, RGB(250, 204, 21), 9, true);
        } else {
            const POINT cast = GetCastPoint();
            swprintf_s(line, L"Cast point: (%ld, %ld)", cast.x, cast.y);
            DrawTextLine(dc, line, 18, 167, muted, 9);
        }

        swprintf_s(line, L"%s  START / PAUSE", KeyDisplayName(gToggleHotkeyVk.load()).c_str());
        DrawTextLine(dc, line, 18, 184, muted, 9, true);
        swprintf_s(line, L"%s  SAFE EXIT", KeyDisplayName(gQuitHotkeyVk.load()).c_str());
        DrawTextLine(dc, line, 177, 184, muted, 9, true);
    } else if (gActiveTab == 1) {
        DrawTextLine(dc, L"HOTKEYS", 18, 93, muted, 12, true);
        swprintf_s(line, L"Start/Pause: %s     Exit: %s",
                   KeyDisplayName(gToggleHotkeyVk.load()).c_str(),
                   KeyDisplayName(gQuitHotkeyVk.load()).c_str());
        DrawTextLine(dc, line, 18, 121, white, 10);
        DrawTextLine(dc, L"Click a button below, then press any key", 18, 146, muted, 9);
        DrawTextLine(dc, L"to rebind it. Esc cancels.", 18, 163, muted, 9);

        if (gBindingHotkey.load()) {
            DrawTextLine(dc, L"WAITING FOR KEY PRESS...  (Esc cancels)", 18, 272,
                         RGB(250, 204, 21), 9, true);
        }
    } else {
        DrawTextLine(dc, L"CALIBRATION", 18, 93, muted, 12, true);

        const POINT cast = GetCastPoint();
        swprintf_s(line, L"Cast point:   (%ld, %ld)", cast.x, cast.y);
        DrawTextLine(dc, line, 18, 121, white, 9);

        const RECT bar = GetBarRect();
        if (RectCalibrated(bar)) {
            swprintf_s(line, L"Fishing bar:  %ldx%ld at (%ld, %ld)",
                       bar.right - bar.left, bar.bottom - bar.top, bar.left, bar.top);
        } else {
            swprintf_s(line, L"Fishing bar:  NOT SET");
        }
        DrawTextLine(dc, line, 18, 140, white, 9);

        const RECT exitR = GetExitRect();
        if (RectCalibrated(exitR)) {
            swprintf_s(line, L"Exit button:  %ldx%ld at (%ld, %ld)",
                       exitR.right - exitR.left, exitR.bottom - exitR.top, exitR.left, exitR.top);
        } else {
            swprintf_s(line, L"Exit button:  NOT SET");
        }
        DrawTextLine(dc, line, 18, 159, white, 9);

        if (gCalibrating.load()) {
            const wchar_t* banner = (gCalibrationTarget == CalibrationTarget::CastPoint)
                ? L"CLICK anywhere on screen  (Esc cancels)"
                : L"DRAG a box over the region  (Esc cancels)";
            DrawTextLine(dc, banner, 18, 310, RGB(250, 204, 21), 9, true);
        }
    }

    BitBlt(windowDc, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBitmap);
    DeleteObject(memBitmap);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

void UpdateButtonLabel() {
    if (gToggleButton) {
        const std::wstring key = KeyDisplayName(gToggleHotkeyVk.load());
        const std::wstring label = (gEnabled.load() ? L"Pause  (" : L"Start  (") + key + L")";
        SetWindowText(gToggleButton, label.c_str());
    }
}

void UpdateExitButtonLabel() {
    if (gExitButton) {
        const std::wstring label = L"Exit  (" + KeyDisplayName(gQuitHotkeyVk.load()) + L")";
        SetWindowText(gExitButton, label.c_str());
    }
}

void ToggleTracker() {
    gEnabled.store(!gEnabled.load());
    if (!gEnabled.load()) MouseButton(false);
    UpdateButtonLabel();
    InvalidateRect(gWindow, nullptr, FALSE);
}

// While calibrating, a low-level mouse hook swallows left-button input and
// reports it back to the window (via WM_CALIBRATION_DONE) instead of
// letting it reach whatever is underneath. The Cast point is a single
// click; the bar/exit regions are a click-drag (mousedown = one corner,
// mouseup = the opposite corner). Actually finishing calibration
// (unhooking, restoring button labels) happens in WindowProc, not here,
// since a low-level hook procedure should stay minimal and return quickly.
LRESULT CALLBACK CalibrationMouseProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code != HC_ACTION || (wParam != WM_LBUTTONDOWN && wParam != WM_LBUTTONUP)) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }
    const auto* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
    // Ignore our own SendInput-generated clicks (e.g. the Cast phase's
    // click firing mid-calibration) - only real, physical input counts.
    if (info->flags & LLMHF_INJECTED) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }
    const POINT pt{info->pt.x, info->pt.y};

    if (gCalibrationTarget == CalibrationTarget::CastPoint) {
        if (wParam != WM_LBUTTONDOWN) return CallNextHookEx(nullptr, code, wParam, lParam);
        SetCastPoint(pt);
        // Remember which top-level window is under that point, so Cast
        // clicks and key holds can force it to the foreground later.
        if (HWND under = WindowFromPoint(pt)) {
            gTargetWindow.store(GetAncestor(under, GA_ROOT));
        }
        PostMessage(gWindow, WM_CALIBRATION_DONE, 0, 0);
        return 1;
    }

    if (gCalibrationTarget == CalibrationTarget::BarRegion ||
        gCalibrationTarget == CalibrationTarget::ExitRegion) {
        if (wParam == WM_LBUTTONDOWN) {
            gDragStart = pt;
            gDragging = true;
            return 1; // Swallow so the drag doesn't also click the game.
        }
        if (wParam == WM_LBUTTONUP && gDragging) {
            gDragging = false;
            const RECT rect{std::min(gDragStart.x, pt.x), std::min(gDragStart.y, pt.y),
                             std::max(gDragStart.x, pt.x), std::max(gDragStart.y, pt.y)};
            if (RectCalibrated(rect)) {
                if (gCalibrationTarget == CalibrationTarget::BarRegion) SetBarRect(rect);
                else SetExitRect(rect);
                PostMessage(gWindow, WM_CALIBRATION_DONE, 0, 0);
            }
            // Too small to be a real drag (likely an accidental click) -
            // swallow it and silently let the user try the drag again;
            // calibration mode stays active either way.
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void StartCalibration(CalibrationTarget target) {
    if (gCalibrationTarget != CalibrationTarget::None || gBindingHotkey.load()) return;
    gMouseHook = SetWindowsHookEx(WH_MOUSE_LL, CalibrationMouseProc,
                                  GetModuleHandle(nullptr), 0);
    if (!gMouseHook) return;
    gCalibrationTarget = target;
    gDragging = false;
    gCalibrating.store(true);
    RegisterHotKey(gWindow, HOTKEY_CANCEL_CAPTURE, MOD_NOREPEAT, VK_ESCAPE);
    if (target == CalibrationTarget::CastPoint && gCalibrateButton) {
        SetWindowText(gCalibrateButton, L"Click anywhere...  (Esc cancels)");
    } else if (target == CalibrationTarget::BarRegion && gCalibrateBarButton) {
        SetWindowText(gCalibrateBarButton, L"Drag over the bar...  (Esc cancels)");
    } else if (target == CalibrationTarget::ExitRegion && gCalibrateExitButton) {
        SetWindowText(gCalibrateExitButton, L"Drag over Exit...  (Esc cancels)");
    }
    InvalidateRect(gWindow, nullptr, FALSE);
}

void EndCalibration() {
    if (gMouseHook) {
        UnhookWindowsHookEx(gMouseHook);
        gMouseHook = nullptr;
    }
    UnregisterHotKey(gWindow, HOTKEY_CANCEL_CAPTURE);
    gCalibrationTarget = CalibrationTarget::None;
    gDragging = false;
    gCalibrating.store(false);
    if (gCalibrateButton) SetWindowText(gCalibrateButton, L"Calibrate Cast Point");
    if (gCalibrateBarButton) SetWindowText(gCalibrateBarButton, L"Calibrate Fishing Bar");
    if (gCalibrateExitButton) SetWindowText(gCalibrateExitButton, L"Calibrate Exit Button");
    InvalidateRect(gWindow, nullptr, FALSE);
}

void UpdateHotkeyButtonLabels() {
    if (gToggleBindButton) {
        const std::wstring label = L"Start/Pause: " + KeyDisplayName(gToggleHotkeyVk.load()) + L"  (click to change)";
        SetWindowText(gToggleBindButton, label.c_str());
    }
    if (gExitBindButton) {
        const std::wstring label = L"Exit: " + KeyDisplayName(gQuitHotkeyVk.load()) + L"  (click to change)";
        SetWindowText(gExitBindButton, label.c_str());
    }
}

// Settings persist to a small INI file next to the exe, so rebound
// Start/Pause and Exit hotkeys survive closing and reopening the program.
std::wstring GetSettingsPath() {
    wchar_t modulePath[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    std::wstring path(modulePath, (len > 0 && len < MAX_PATH) ? len : 0);
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring dir = (slash == std::wstring::npos) ? L"" : path.substr(0, slash + 1);
    return dir + L"FishingHyperTracker.ini";
}

// GetPrivateProfileInt is documented/typed as unsigned, which doesn't
// suit screen coordinates (can be negative on multi-monitor setups with a
// display above/left of the primary) - read/write coordinates as signed
// text instead.
constexpr int kNoSavedValue = -2147483647;

int ReadIntSetting(const wchar_t* section, const wchar_t* key, int def, const std::wstring& path) {
    wchar_t buf[32]{};
    GetPrivateProfileStringW(section, key, L"", buf, 32, path.c_str());
    if (buf[0] == L'\0') return def;
    wchar_t* end = nullptr;
    const long value = wcstol(buf, &end, 10);
    if (end == buf) return def;
    return static_cast<int>(value);
}

void WriteIntSetting(const wchar_t* section, const wchar_t* key, int value, const std::wstring& path) {
    wchar_t buf[16];
    swprintf_s(buf, L"%d", value);
    WritePrivateProfileStringW(section, key, buf, path.c_str());
}

void LoadSettings() {
    const std::wstring path = GetSettingsPath();
    const UINT toggleVk = GetPrivateProfileIntW(L"Hotkeys", L"ToggleVk",
                                                static_cast<UINT>(VK_F6), path.c_str());
    const UINT quitVk = GetPrivateProfileIntW(L"Hotkeys", L"QuitVk",
                                              static_cast<UINT>(VK_F8), path.c_str());
    if (toggleVk > 0 && toggleVk <= 0xFF) gToggleHotkeyVk.store(static_cast<WORD>(toggleVk));
    if (quitVk > 0 && quitVk <= 0xFF) gQuitHotkeyVk.store(static_cast<WORD>(quitVk));
    const UINT waitSeconds = GetPrivateProfileIntW(L"Fishing", L"WaitForFishSeconds",
                                                    static_cast<UINT>(kDefaultWaitForFishMs / 1000),
                                                    path.c_str());
    const ULONGLONG waitMs = std::clamp<ULONGLONG>(
        static_cast<ULONGLONG>(waitSeconds) * 1000ULL,
        kMinWaitForFishMs, kMaxWaitForFishMs);
    gWaitForFishMs.store(waitMs);

    // Calibration - each user's own cast point/bar/exit regions, saved so
    // they don't have to recalibrate on every single launch.
    const int castX = ReadIntSetting(L"Calibration", L"CastX", kNoSavedValue, path);
    const int castY = ReadIntSetting(L"Calibration", L"CastY", kNoSavedValue, path);
    if (castX != kNoSavedValue && castY != kNoSavedValue) {
        SetCastPoint(POINT{castX, castY});
        if (HWND under = WindowFromPoint(POINT{castX, castY})) {
            gTargetWindow.store(GetAncestor(under, GA_ROOT));
        }
    }

    const RECT bar{ReadIntSetting(L"Calibration", L"BarLeft", 0, path),
                    ReadIntSetting(L"Calibration", L"BarTop", 0, path),
                    ReadIntSetting(L"Calibration", L"BarRight", 0, path),
                    ReadIntSetting(L"Calibration", L"BarBottom", 0, path)};
    if (RectCalibrated(bar)) SetBarRect(bar);

    const RECT exit{ReadIntSetting(L"Calibration", L"ExitLeft", 0, path),
                     ReadIntSetting(L"Calibration", L"ExitTop", 0, path),
                     ReadIntSetting(L"Calibration", L"ExitRight", 0, path),
                     ReadIntSetting(L"Calibration", L"ExitBottom", 0, path)};
    if (RectCalibrated(exit)) SetExitRect(exit);
}

void SaveSettings() {
    const std::wstring path = GetSettingsPath();
    wchar_t buf[16];
    swprintf_s(buf, L"%u", static_cast<unsigned>(gToggleHotkeyVk.load()));
    WritePrivateProfileStringW(L"Hotkeys", L"ToggleVk", buf, path.c_str());
    swprintf_s(buf, L"%u", static_cast<unsigned>(gQuitHotkeyVk.load()));
    WritePrivateProfileStringW(L"Hotkeys", L"QuitVk", buf, path.c_str());
    swprintf_s(buf, L"%u", static_cast<unsigned>(gWaitForFishMs.load() / 1000));
    WritePrivateProfileStringW(L"Fishing", L"WaitForFishSeconds", buf, path.c_str());

    const POINT cast = GetCastPoint();
    WriteIntSetting(L"Calibration", L"CastX", cast.x, path);
    WriteIntSetting(L"Calibration", L"CastY", cast.y, path);

    const RECT bar = GetBarRect();
    WriteIntSetting(L"Calibration", L"BarLeft", bar.left, path);
    WriteIntSetting(L"Calibration", L"BarTop", bar.top, path);
    WriteIntSetting(L"Calibration", L"BarRight", bar.right, path);
    WriteIntSetting(L"Calibration", L"BarBottom", bar.bottom, path);

    const RECT exit = GetExitRect();
    WriteIntSetting(L"Calibration", L"ExitLeft", exit.left, path);
    WriteIntSetting(L"Calibration", L"ExitTop", exit.top, path);
    WriteIntSetting(L"Calibration", L"ExitRight", exit.right, path);
    WriteIntSetting(L"Calibration", L"ExitBottom", exit.bottom, path);
}

void ApplyToggleHotkey(WORD vk) {
    UnregisterHotKey(gWindow, HOTKEY_TOGGLE);
    RegisterHotKey(gWindow, HOTKEY_TOGGLE, MOD_NOREPEAT, vk);
    gToggleHotkeyVk.store(vk);
    UpdateButtonLabel();
    SaveSettings();
}

void ApplyQuitHotkey(WORD vk) {
    UnregisterHotKey(gWindow, HOTKEY_QUIT);
    RegisterHotKey(gWindow, HOTKEY_QUIT, MOD_NOREPEAT, vk);
    gQuitHotkeyVk.store(vk);
    UpdateExitButtonLabel();
    SaveSettings();
}

// Same idea as CalibrationMouseProc, but for keyboard: swallows the next
// real keypress and reports it back (via WM_KEYBIND_DONE) to be bound as
// whichever hotkey is currently being rebound, instead of letting it reach
// whatever window currently has focus.
LRESULT CALLBACK HotkeyBindProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && wParam == WM_KEYDOWN) {
        const auto* info = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        if (!(info->flags & LLKHF_INJECTED) && info->vkCode != VK_ESCAPE) {
            gCapturedVk = static_cast<WORD>(info->vkCode);
            PostMessage(gWindow, WM_KEYBIND_DONE, 0, 0);
            return 1; // Swallow it so it doesn't also type into whatever's focused.
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void StartHotkeyBind(int target) {
    if (gBindingHotkey.load() || gCalibrating.load()) return;
    gKeyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, HotkeyBindProc,
                                     GetModuleHandle(nullptr), 0);
    if (!gKeyboardHook) return;
    gBindingTarget = target;
    gBindingHotkey.store(true);
    RegisterHotKey(gWindow, HOTKEY_CANCEL_CAPTURE, MOD_NOREPEAT, VK_ESCAPE);
    HWND button = (target == 1) ? gToggleBindButton : gExitBindButton;
    if (button) SetWindowText(button, L"Press a key...  (Esc cancels)");
    InvalidateRect(gWindow, nullptr, FALSE);
}

void EndHotkeyBind() {
    if (gKeyboardHook) {
        UnhookWindowsHookEx(gKeyboardHook);
        gKeyboardHook = nullptr;
    }
    UnregisterHotKey(gWindow, HOTKEY_CANCEL_CAPTURE);
    gBindingHotkey.store(false);
    gBindingTarget = 0;
    UpdateHotkeyButtonLabels();
    InvalidateRect(gWindow, nullptr, FALSE);
}

void SetActiveTab(int tab) {
    gActiveTab = tab;
    const bool fishing = (tab == 0);
    const bool keystrokes = (tab == 1);
    const bool calibration = (tab == 2);
    if (gToggleButton) ShowWindow(gToggleButton, fishing ? SW_SHOW : SW_HIDE);
    if (gExitButton) ShowWindow(gExitButton, fishing ? SW_SHOW : SW_HIDE);
    if (gWaitFishEdit) ShowWindow(gWaitFishEdit, fishing ? SW_SHOW : SW_HIDE);
    if (gWaitFishLabel) ShowWindow(gWaitFishLabel, fishing ? SW_SHOW : SW_HIDE);
    if (gWaitFishSecondsLabel) ShowWindow(gWaitFishSecondsLabel, fishing ? SW_SHOW : SW_HIDE);
    if (gToggleBindButton) ShowWindow(gToggleBindButton, keystrokes ? SW_SHOW : SW_HIDE);
    if (gExitBindButton) ShowWindow(gExitBindButton, keystrokes ? SW_SHOW : SW_HIDE);
    if (gCalibrateButton) ShowWindow(gCalibrateButton, calibration ? SW_SHOW : SW_HIDE);
    if (gCalibrateBarButton) ShowWindow(gCalibrateBarButton, calibration ? SW_SHOW : SW_HIDE);
    if (gCalibrateExitButton) ShowWindow(gCalibrateExitButton, calibration ? SW_SHOW : SW_HIDE);
    InvalidateRect(gWindow, nullptr, TRUE);
}


ULONGLONG ReadWaitForFishEdit() {
    if (!gWaitFishEdit) return gWaitForFishMs.load();
    wchar_t buf[32]{};
    GetWindowTextW(gWaitFishEdit, buf, 32);
    wchar_t* end = nullptr;
    const unsigned long value = wcstoul(buf, &end, 10);
    if (end == buf) return gWaitForFishMs.load();
    return std::clamp<ULONGLONG>(static_cast<ULONGLONG>(value) * 1000ULL,
                                  kMinWaitForFishMs, kMaxWaitForFishMs);
}

void UpdateWaitForFishSetting() {
    const ULONGLONG ms = ReadWaitForFishEdit();
    gWaitForFishMs.store(ms);
    wchar_t buf[32];
    swprintf_s(buf, L"%u", static_cast<unsigned>(ms / 1000));
    SetWindowTextW(gWaitFishEdit, buf);
    SaveSettings();
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            gTabControl = CreateWindow(WC_TABCONTROL, L"", WS_CHILD | WS_VISIBLE,
                14, 52, 338, 28, hwnd, reinterpret_cast<HMENU>(ID_TAB), nullptr, nullptr);
            SendMessage(gTabControl, WM_SETFONT, reinterpret_cast<WPARAM>(gUiFont), TRUE);
            TCITEM tab{};
            tab.mask = TCIF_TEXT;
            tab.pszText = const_cast<LPWSTR>(L"Fishing");
            TabCtrl_InsertItem(gTabControl, 0, &tab);
            tab.pszText = const_cast<LPWSTR>(L"Keystrokes");
            TabCtrl_InsertItem(gTabControl, 1, &tab);
            tab.pszText = const_cast<LPWSTR>(L"Calibration");
            TabCtrl_InsertItem(gTabControl, 2, &tab);

            gWaitFishLabel = CreateWindow(L"STATIC", L"WAIT FOR FISH",
                WS_CHILD | WS_VISIBLE, 24, 220, 180, 22,
                hwnd, nullptr, nullptr, nullptr);
            gWaitFishSecondsLabel = CreateWindow(L"STATIC", L"Seconds",
                WS_CHILD | WS_VISIBLE, 282, 220, 55, 22,
                hwnd, nullptr, nullptr, nullptr);
            gWaitFishEdit = CreateWindowEx(
                WS_EX_CLIENTEDGE, L"EDIT", L"3",
                WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_AUTOHSCROLL | ES_CENTER,
                222, 216, 52, 30, hwnd,
                reinterpret_cast<HMENU>(ID_WAIT_FISH_EDIT), nullptr, nullptr);

            gToggleButton = CreateWindow(L"BUTTON", L"Start  (F6)",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 24, 252, 156, 40,
                hwnd, reinterpret_cast<HMENU>(ID_TOGGLE), nullptr, nullptr);
            gExitButton = CreateWindow(L"BUTTON", L"Exit  (F8)",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 186, 252, 156, 40,
                hwnd, reinterpret_cast<HMENU>(ID_EXIT), nullptr, nullptr);
            // Keystrokes tab's controls; hidden until that tab is selected.
            gToggleBindButton = CreateWindow(L"BUTTON", L"Start/Pause: F6  (click to change)",
                WS_CHILD | BS_PUSHBUTTON, 40, 225, 282, 40,
                hwnd, reinterpret_cast<HMENU>(ID_BIND_TOGGLE), nullptr, nullptr);
            gExitBindButton = CreateWindow(L"BUTTON", L"Exit: F8  (click to change)",
                WS_CHILD | BS_PUSHBUTTON, 40, 273, 282, 40,
                hwnd, reinterpret_cast<HMENU>(ID_BIND_QUIT), nullptr, nullptr);
            // Calibration tab's controls; hidden until that tab is selected.
            // Each region is calibrated by clicking (Cast Point) or
            // click-dragging (Bar/Exit) live on the user's own screen, so
            // nothing here assumes any particular resolution or monitor.
            gCalibrateButton = CreateWindow(L"BUTTON", L"Calibrate Cast Point",
                WS_CHILD | BS_PUSHBUTTON, 38, 176, 290, 36,
                hwnd, reinterpret_cast<HMENU>(ID_CALIBRATE), nullptr, nullptr);
            gCalibrateBarButton = CreateWindow(L"BUTTON", L"Calibrate Fishing Bar",
                WS_CHILD | BS_PUSHBUTTON, 38, 220, 290, 36,
                hwnd, reinterpret_cast<HMENU>(ID_CALIBRATE_BAR), nullptr, nullptr);
            gCalibrateExitButton = CreateWindow(L"BUTTON", L"Calibrate Exit Button",
                WS_CHILD | BS_PUSHBUTTON, 38, 264, 290, 36,
                hwnd, reinterpret_cast<HMENU>(ID_CALIBRATE_EXIT), nullptr, nullptr);

            SendMessage(gWaitFishLabel, WM_SETFONT, reinterpret_cast<WPARAM>(gUiFont), TRUE);
            SendMessage(gWaitFishSecondsLabel, WM_SETFONT, reinterpret_cast<WPARAM>(gUiFont), TRUE);
            SendMessage(gWaitFishEdit, WM_SETFONT, reinterpret_cast<WPARAM>(gUiFont), TRUE);
            {
                wchar_t waitBuf[32];
                swprintf_s(waitBuf, L"%u", static_cast<unsigned>(gWaitForFishMs.load() / 1000));
                SetWindowTextW(gWaitFishEdit, waitBuf);
            }
            SendMessage(gToggleButton, WM_SETFONT, reinterpret_cast<WPARAM>(gUiFont), TRUE);
            SendMessage(gExitButton, WM_SETFONT, reinterpret_cast<WPARAM>(gUiFont), TRUE);
            SendMessage(gCalibrateButton, WM_SETFONT, reinterpret_cast<WPARAM>(gUiFont), TRUE);
            SendMessage(gCalibrateBarButton, WM_SETFONT, reinterpret_cast<WPARAM>(gUiFont), TRUE);
            SendMessage(gCalibrateExitButton, WM_SETFONT, reinterpret_cast<WPARAM>(gUiFont), TRUE);
            SendMessage(gToggleBindButton, WM_SETFONT, reinterpret_cast<WPARAM>(gUiFont), TRUE);
            SendMessage(gExitBindButton, WM_SETFONT, reinterpret_cast<WPARAM>(gUiFont), TRUE);
            UpdateButtonLabel();
            UpdateExitButtonLabel();
            UpdateHotkeyButtonLabels();
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == ID_WAIT_FISH_EDIT &&
                HIWORD(wParam) == EN_KILLFOCUS) {
                UpdateWaitForFishSetting();
            }
            if (LOWORD(wParam) == ID_TOGGLE) ToggleTracker();
            if (LOWORD(wParam) == ID_EXIT) PostMessage(hwnd, WM_CLOSE, 0, 0);
            if (LOWORD(wParam) == ID_CALIBRATE) {
                if (gCalibrating.load() && gCalibrationTarget == CalibrationTarget::CastPoint) EndCalibration();
                else StartCalibration(CalibrationTarget::CastPoint);
            }
            if (LOWORD(wParam) == ID_CALIBRATE_BAR) {
                if (gCalibrating.load() && gCalibrationTarget == CalibrationTarget::BarRegion) EndCalibration();
                else StartCalibration(CalibrationTarget::BarRegion);
            }
            if (LOWORD(wParam) == ID_CALIBRATE_EXIT) {
                if (gCalibrating.load() && gCalibrationTarget == CalibrationTarget::ExitRegion) EndCalibration();
                else StartCalibration(CalibrationTarget::ExitRegion);
            }
            if (LOWORD(wParam) == ID_BIND_TOGGLE) {
                if (gBindingHotkey.load() && gBindingTarget == 1) EndHotkeyBind();
                else StartHotkeyBind(1);
            }
            if (LOWORD(wParam) == ID_BIND_QUIT) {
                if (gBindingHotkey.load() && gBindingTarget == 2) EndHotkeyBind();
                else StartHotkeyBind(2);
            }
            return 0;
        case WM_NOTIFY: {
            const NMHDR* header = reinterpret_cast<NMHDR*>(lParam);
            if (header->hwndFrom == gTabControl && header->code == TCN_SELCHANGE) {
                SetActiveTab(TabCtrl_GetCurSel(gTabControl));
            }
            return 0;
        }
        case WM_HOTKEY:
            if (wParam == HOTKEY_TOGGLE) ToggleTracker();
            if (wParam == HOTKEY_QUIT) PostMessage(hwnd, WM_CLOSE, 0, 0);
            if (wParam == HOTKEY_CANCEL_CAPTURE) {
                if (gCalibrating.load()) EndCalibration();
                if (gBindingHotkey.load()) EndHotkeyBind();
            }
            return 0;
        case WM_CALIBRATION_DONE: {
            // The hook already stored the new point/region; remember which
            // target that was before EndCalibration resets it, since only
            // the Cast Point gets a confirmation click - firing a click at
            // the cast point after calibrating the bar/exit regions would
            // be an unwanted extra cast every time.
            const CalibrationTarget justCalibrated = gCalibrationTarget;
            EndCalibration();
            SaveSettings();
            if (justCalibrated == CalibrationTarget::CastPoint) {
                ForceForeground(gTargetWindow.load());
                ClickAt(GetCastPoint());
            }
            return 0;
        }
        case WM_KEYBIND_DONE: {
            const int target = gBindingTarget;
            const WORD vk = gCapturedVk;
            if (target == 1) ApplyToggleHotkey(vk);
            else if (target == 2) ApplyQuitHotkey(vk);
            EndHotkeyBind();
            return 0;
        }
        case WM_TRACKER_UPDATE:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_PAINT:
            PaintHud(hwnd);
            return 0;
        case WM_NCHITTEST: {
            const LRESULT result = DefWindowProc(hwnd, message, wParam, lParam);
            if (result == HTCLIENT) {
                POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ScreenToClient(hwnd, &pt);
                if (pt.y < 49) return HTCAPTION;
            }
            return result;
        }
        case WM_CLOSE:
            gEnabled.store(false);
            gQuit.store(true);
            MouseButton(false);
            KeyEvent(false, kHoldKeyVk);
            if (gCalibrating.load()) EndCalibration();
            if (gBindingHotkey.load()) EndHotkeyBind();
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    SetProcessDPIAware();
    LoadSettings();

    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    Gdiplus::GdiplusStartup(&gGdiplusToken, &gdiplusStartupInput, nullptr);
    gFishOnIcon = LoadPngFromMemory(kFishOnPng, kFishOnPngSize);
    gFishOffIcon = LoadPngFromMemory(kFishOffPng, kFishOffPngSize);

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_TAB_CLASSES;
    InitCommonControlsEx(&icc);

    gBackgroundBrush = CreateSolidBrush(RGB(17, 24, 39));
    gUiFont = CreateFont(-16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    WNDCLASSEX wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = gBackgroundBrush;
    wc.lpszClassName = kClassName;
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassEx(&wc);

    gWindow = CreateWindowEx(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        kClassName, L"Fishing Hyper Tracker", WS_POPUP | WS_BORDER,
        24, 80, 366, 360, nullptr, nullptr, instance, nullptr);
    if (!gWindow) return 1;

    // Slight overall translucency. Note this is whole-window alpha (text and
    // buttons included, not just the background) - true background-only
    // translucency would need per-pixel-alpha compositing (UpdateLayeredWindow
    // with an ARGB surface), which doesn't play well with native child
    // controls like the buttons and tab strip here.
    SetLayeredWindowAttributes(gWindow, 0, 235, LWA_ALPHA);

    SetActiveTab(0);

    // Default the cast point to screen center until the user calibrates it -
    // but don't clobber one already loaded from settings above.
    {
        const POINT loaded = GetCastPoint();
        if (loaded.x == 0 && loaded.y == 0) {
            SetCastPoint(POINT{GetSystemMetrics(SM_CXSCREEN) / 2, GetSystemMetrics(SM_CYSCREEN) / 2});
        }
    }

    BOOL dark = TRUE;
    DwmSetWindowAttribute(gWindow, 20, &dark, sizeof(dark));
    RegisterHotKey(gWindow, HOTKEY_TOGGLE, MOD_NOREPEAT, gToggleHotkeyVk.load());
    RegisterHotKey(gWindow, HOTKEY_QUIT, MOD_NOREPEAT, gQuitHotkeyVk.load());
    ShowWindow(gWindow, showCommand);
    UpdateWindow(gWindow);

    HANDLE thread = CreateThread(nullptr, 0, TrackerThread, nullptr, 0, nullptr);
    MSG msg{};
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    gQuit.store(true);
    MouseButton(false);
    if (gCalibrating.load()) EndCalibration();
    if (gBindingHotkey.load()) EndHotkeyBind();
    if (thread) {
        WaitForSingleObject(thread, 2000);
        CloseHandle(thread);
    }
    UnregisterHotKey(gWindow, HOTKEY_TOGGLE);
    UnregisterHotKey(gWindow, HOTKEY_QUIT);
    DeleteObject(gUiFont);
    DeleteObject(gBackgroundBrush);
    // GDI+ objects must be destroyed before GdiplusShutdown.
    delete gFishOnIcon;
    gFishOnIcon = nullptr;
    delete gFishOffIcon;
    gFishOffIcon = nullptr;
    Gdiplus::GdiplusShutdown(gGdiplusToken);
    return 0;
}
