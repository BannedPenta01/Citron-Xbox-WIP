#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include <cwctype>
#include <objidl.h> 
#include <shlobj.h>
#include <windows.h>
#include <iostream>

#ifndef PROPID
typedef ULONG PROPID;
#endif

#include <gdiplus.h>
#include <xinput.h>
#include "common/settings.h"
#include "core/core.h"
#include "core/cpu_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/loader/loader.h"
#include "video_core/gpu.h"

using namespace Gdiplus;
namespace fs = std::filesystem;

// --- Constants ---
#define MAX_CONTROLLERS 4
const int INPUT_DEADZONE = 8000;
const Color COLOR_BG(255, 30, 30, 30);
const Color COLOR_ACCENT(255, 255, 140, 0); 
const Color COLOR_TEXT(255, 255, 255, 255);
const Color COLOR_TEXT_DIM(255, 150, 150, 150);
const Color COLOR_TEXT_SELECTED(255, 0, 0, 0);
const Color COLOR_ITEM_BG(255, 50, 50, 50);
const Color COLOR_ITEM_SELECTED(255, 255, 140, 0);    
const Color COLOR_HIGHLIGHT_BROWN(255, 180, 110, 60); 
const Color COLOR_EDITING(255, 255, 160, 40);         
const Color COLOR_TAB_INACTIVE(255, 70, 70, 70);

// --- Global Data Structures ---
struct Game {
    std::wstring name;
    std::filesystem::path path;
};

enum class AppState { GameList, Settings, Running };
enum class SettingsTab { General, System, Graphics, Audio, Network };

// --- Forward Declarations ---
static void ScanGames();
static void RenderUI(HDC hdc, int width, int height);
static void HandleInput(HWND hwnd);
static void StartGame(HWND hwnd, const Game& game);
static void InstallFiles(HWND hwnd, const std::wstring& title, const std::filesystem::path& subPath);
static void SaveSettings();
static void LoadSettings();
[[maybe_unused]] static void EnforceMemoryLimit();
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

// --- Global State ---
bool g_IsEditingSetting = false;
AppState g_AppState = AppState::GameList;
SettingsTab g_CurrentTab = SettingsTab::General;
std::atomic<bool> g_IsInstalling = false;
std::wstring g_InstallStatus = L"";
std::vector<std::filesystem::path> g_UserGamePaths;
std::vector<Game> g_Games;
int g_SelectedGameIndex = 0;
int g_SelectedSettingIndex = 0; 
ULONGLONG g_NextInputTime = 0;
WORD g_LastInputMask = 0;
ULONG_PTR g_gdiplusToken;

// --- Emu Window Classes ---
class DummyContext : public Core::Frontend::GraphicsContext {
public:
    explicit DummyContext() {};
    void MakeCurrent() override {};
    void DoneCurrent() override {};
    void SwapBuffers() override {};
};

class XboxEmuWindow : public Core::Frontend::EmuWindow {
public:
    XboxEmuWindow(HWND hwnd) : hwnd_(hwnd) {
        window_info.type = Core::Frontend::WindowSystemType::Windows;
        window_info.render_surface = static_cast<void*>(hwnd_);
    }
    std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const override {
        return std::make_unique<DummyContext>();
    }
    bool IsShown() const override { return true; }
private:
    HWND hwnd_;
};

std::unique_ptr<XboxEmuWindow> g_EmuWindow;
std::unique_ptr<Core::System> g_System;

// --- Helper Functions ---

static std::wstring Trim(const std::wstring& s) {
    if (s.empty()) return s;
    size_t start = 0;
    while (start < s.length() && (s[start] == L' ' || s[start] == L'\t' || s[start] == L'\r' || s[start] == L'\n')) start++;
    size_t end = s.length();
    while (end > start && (s[end - 1] == L' ' || s[end - 1] == L'\t' || s[end - 1] == L'\r' || s[end - 1] == L'\n')) end--;
    return s.substr(start, end - start);
}

// ---------------------------------------------------------
// CRITICAL: XBOX PATH FIX
// This gets the "LocalState" folder which is the ONLY writable place.
// ---------------------------------------------------------
static std::filesystem::path GetUserDirectory() {
    wchar_t buffer[32767];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, 32767)) {
        std::filesystem::path p(buffer);
        p /= "Citron"; 
        
        // Ensure folder structure exists immediately
        std::error_code ec;
        if (!fs::exists(p, ec)) fs::create_directories(p, ec);
        if (!fs::exists(p / "user", ec)) fs::create_directories(p / "user", ec);
        if (!fs::exists(p / "user" / "keys", ec)) fs::create_directories(p / "user" / "keys", ec);
        if (!fs::exists(p / "user" / "nand", ec)) fs::create_directories(p / "user" / "nand", ec);
        if (!fs::exists(p / "user" / "config", ec)) fs::create_directories(p / "user" / "config", ec);
        
        return p / "user";
    }
    
    // Fallback for PC testing
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    return std::filesystem::path(path).parent_path() / "user";
}

static std::filesystem::path GetConfigPath() {
    // Save config inside the user directory so it's portable-ish
    return GetUserDirectory() / "config.ini";
}

// --- Implementation ---

static void ScanGames() {
    g_Games.clear();

    std::vector<std::filesystem::path> searchPaths;
    searchPaths.push_back("D:\\Games"); 

    // Add user paths
    for (const auto& p : g_UserGamePaths) {
        if (fs::exists(p)) {
            bool found = false;
            for(const auto& s : searchPaths) if(s == p) found = true;
            if(!found) searchPaths.push_back(p);
        }
    }

    // Auto-scan drives
    for (char letter = 'E'; letter <= 'Z'; ++letter) {
        std::string drive = ""; drive += letter; drive += ":\\Games";
        if (fs::exists(drive)) searchPaths.push_back(drive);
    }

    for (const auto& path : searchPaths) {
        if (!fs::exists(path)) continue;
        try {
            for (const auto& entry : fs::recursive_directory_iterator(path)) {
                if (entry.is_regular_file()) {
                    std::string ext = entry.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext == ".nsp" || ext == ".xci") {
                        g_Games.push_back({entry.path().filename().wstring(), entry.path()});
                    }
                }
            }
        } catch (...) {}
    }
}

static const size_t MAX_MEMORY_BYTES = 6ULL * 1024 * 1024 * 1024; 
[[maybe_unused]] static void EnforceMemoryLimit() {
    HANDLE hJob = CreateJobObject(NULL, NULL);
    if (hJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {0};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        jeli.ProcessMemoryLimit = MAX_MEMORY_BYTES;
        if (SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli))) {
            AssignProcessToJobObject(hJob, GetCurrentProcess());
        }
    }
}

static void SaveSettings() {
    auto path = GetConfigPath();
    // Use std::wfstream for better compatibility
    std::wofstream file(path);
    if (!file.is_open()) return;

    file << L"[System]" << std::endl;
    file << L"Language=" << (int)Settings::values.language_index.GetValue() << std::endl;
    file << L"Region=" << (int)Settings::values.region_index.GetValue() << std::endl;
    file << L"CustomRTC=" << (Settings::values.custom_rtc_enabled.GetValue() ? 1 : 0) << std::endl;
    file << L"MultiCore=" << (Settings::values.use_multi_core.GetValue() ? 1 : 0) << std::endl;
    file << L"MemoryLayout=" << (int)Settings::values.memory_layout_mode.GetValue() << std::endl;

    file << std::endl << L"[Paths]" << std::endl;
    
    // Deduplicate
    std::sort(g_UserGamePaths.begin(), g_UserGamePaths.end());
    g_UserGamePaths.erase(std::unique(g_UserGamePaths.begin(), g_UserGamePaths.end()), g_UserGamePaths.end());
    
    for (const auto& p : g_UserGamePaths) {
        file << L"GamePath=" << p.wstring() << std::endl;
    }
    file.close();
}

static void LoadSettings() {
    auto path = GetConfigPath();
    std::wifstream file(path);
    if (!file.is_open()) return;

    g_UserGamePaths.clear(); 

    std::wstring line;
    while (std::getline(file, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == L'[') continue;

        size_t eq = line.find(L'=');
        if (eq != std::wstring::npos) {
            std::wstring key = Trim(line.substr(0, eq));
            std::wstring val = Trim(line.substr(eq + 1));

            if (key == L"Language") Settings::values.language_index.SetValue((Settings::Language)_wtoi(val.c_str()));
            else if (key == L"Region") Settings::values.region_index.SetValue((Settings::Region)_wtoi(val.c_str()));
            else if (key == L"CustomRTC") Settings::values.custom_rtc_enabled.SetValue(_wtoi(val.c_str()) != 0);
            else if (key == L"MultiCore") Settings::values.use_multi_core.SetValue(_wtoi(val.c_str()) != 0);
            else if (key == L"MemoryLayout") Settings::values.memory_layout_mode.SetValue((Settings::MemoryLayout)_wtoi(val.c_str()));
            else if (key == L"GamePath") {
                 g_UserGamePaths.push_back(val);
            }
        }
    }
    file.close();
}

static void InstallFilesThread(HWND hwnd, std::wstring sourcePath, std::filesystem::path dest_dir) {
    g_IsInstalling = true;
    g_InstallStatus = L"Copying files...";
    InvalidateRect(hwnd, NULL, FALSE);
    try {
        std::filesystem::create_directories(dest_dir);
        auto options = std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing;
        std::filesystem::copy(sourcePath, dest_dir, options);
        g_InstallStatus = L"Done!";
        MessageBoxW(hwnd, L"Files Copied!", L"Success", MB_OK);
    } catch (const std::exception& e) {
        g_InstallStatus = L"Error!";
        std::string what = e.what();
        std::wstring wwhat(what.begin(), what.end());
        MessageBoxW(hwnd, (L"Failed: " + wwhat).c_str(), L"Error", MB_OK);
    }
    g_IsInstalling = false;
    InvalidateRect(hwnd, NULL, FALSE);
}

static void InstallFiles(HWND hwnd, const std::wstring& title, const std::filesystem::path& subPath) {
    if (g_IsInstalling) return;
    std::filesystem::path dest_dir = GetUserDirectory() / subPath;
    
    IFileOpenDialog* pFileOpen;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL, IID_IFileOpenDialog, reinterpret_cast<void**>(&pFileOpen));
    if (SUCCEEDED(hr)) {
        pFileOpen->SetTitle(title.c_str());
        pFileOpen->SetOptions(FOS_PICKFOLDERS);
        if (SUCCEEDED(pFileOpen->Show(hwnd))) {
            IShellItem* pItem;
            if (SUCCEEDED(pFileOpen->GetResult(&pItem))) {
                PWSTR pszFilePath;
                if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath))) {
                    std::wstring source(pszFilePath);
                    CoTaskMemFree(pszFilePath);
                    
                    if (title == L"Add Game Directory") {
                        // Store path and save immediately
                        g_UserGamePaths.push_back(source);
                        SaveSettings(); 
                        ScanGames();    
                        InvalidateRect(hwnd, NULL, FALSE);
                        MessageBoxW(hwnd, L"Game Directory Saved!", L"Citron", MB_OK);
                    } else {
                        std::thread(InstallFilesThread, hwnd, source, dest_dir).detach();
                    }
                }
                pItem->Release();
            }
        }
        pFileOpen->Release();
    }
}

static void RenderUI(HDC hdc, int width, int height) {
    if (g_AppState == AppState::Running) return;

    Graphics graphics(hdc);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    SolidBrush bgBrush(COLOR_BG);
    graphics.FillRectangle(&bgBrush, 0, 0, width, height);

    FontFamily fontFamily(L"Segoe UI");
    Font titleFont(&fontFamily, 28, FontStyleBold, UnitPixel);
    SolidBrush accentBrush(COLOR_ACCENT);
    SolidBrush textBrush(COLOR_TEXT);
    StringFormat format;
    format.SetAlignment(StringAlignmentCenter);
    format.SetLineAlignment(StringAlignmentCenter);

    RectF titleRect(0, 10, (REAL)width, 40);
    graphics.DrawString(L"CITRON", -1, &titleFont, titleRect, &format, &accentBrush);

    if (g_IsInstalling) {
        Font statusFont(&fontFamily, 24, FontStyleRegular, UnitPixel);
        RectF r(0, (REAL)height / 2, (REAL)width, 50);
        graphics.DrawString(g_InstallStatus.c_str(), -1, &statusFont, r, &format, &textBrush);
        return;
    }

    if (g_AppState == AppState::GameList) {
        if (g_Games.empty()) {
            Font msgFont(&fontFamily, 18, FontStyleRegular, UnitPixel);
            RectF msgRect(0, (REAL)height / 2, (REAL)width, 40);
            graphics.DrawString(L"No games found.\n1. Settings > Add Game Directory\n2. Settings > Install Prod Keys", -1, &msgFont, msgRect, &format, &textBrush);
        } else {
            int visibleItems = (height - 100) / 40;
            int startIdx = std::max(0, g_SelectedGameIndex - visibleItems / 2);
            int endIdx = std::min((int)g_Games.size(), startIdx + visibleItems);
            Font itemFont(&fontFamily, 20, FontStyleRegular, UnitPixel);
            SolidBrush itemBgBrush(COLOR_ITEM_BG);
            SolidBrush selBrush(COLOR_ITEM_SELECTED);
            SolidBrush selTextBrush(COLOR_TEXT_SELECTED);
            float y = 80;
            for (int i = startIdx; i < endIdx; ++i) {
                RectF r(100.0f, y, (REAL)(width - 200), 36.0f);
                if (i == g_SelectedGameIndex) {
                    graphics.FillRectangle(&selBrush, r);
                    graphics.DrawString(g_Games[i].name.c_str(), -1, &itemFont, r, &format, &selTextBrush);
                } else {
                    graphics.FillRectangle(&itemBgBrush, r);
                    graphics.DrawString(g_Games[i].name.c_str(), -1, &itemFont, r, &format, &textBrush);
                }
                y += 40;
            }
        }
    } else if (g_AppState == AppState::Settings) {
        const wchar_t* tabs[] = {L"General", L"System", L"Graphics", L"Audio", L"Network"};
        float tabW = (float)(width - 40) / 5;
        Font tabFont(&fontFamily, 16, FontStyleBold, UnitPixel);
        SolidBrush inactiveBrush(COLOR_TAB_INACTIVE);

        for (int i = 0; i < 5; ++i) {
            RectF r(20 + i * tabW, 60, tabW - 5, 30);
            if ((int)g_CurrentTab == i) {
                graphics.FillRectangle(&accentBrush, r);
                graphics.DrawString(tabs[i], -1, &tabFont, r, &format, &textBrush);
            } else {
                graphics.FillRectangle(&inactiveBrush, r);
                graphics.DrawString(tabs[i], -1, &tabFont, r, &format, &textBrush);
            }
        }
        float contentY = 110;
        Font labelFont(&fontFamily, 18, FontStyleRegular, UnitPixel);
        Font valFont(&fontFamily, 18, FontStyleRegular, UnitPixel);
        StringFormat leftAlign;
        leftAlign.SetAlignment(StringAlignmentNear);

        if (g_CurrentTab == SettingsTab::System) {
             auto GetLangString = [](int index) {
                switch (index) {
                case 0: return L"Japanese"; case 1: return L"American English"; case 2: return L"French";
                case 3: return L"German"; case 4: return L"Italian"; case 5: return L"Spanish";
                case 6: return L"Chinese"; case 7: return L"Korean"; case 8: return L"Dutch";
                case 9: return L"Portuguese"; case 10: return L"Russian"; case 11: return L"Taiwanese";
                case 12: return L"British English"; case 13: return L"Canadian French"; case 14: return L"Latin American Spanish";
                case 15: return L"Simplified Chinese"; case 16: return L"Traditional Chinese"; case 17: return L"Brazilian Portuguese";
                default: return L"Unknown";
                }
            };
            struct SysItem { std::wstring label; std::wstring val; };
            std::vector<SysItem> items = {
                {L"Language", GetLangString((int)Settings::values.language_index.GetValue())},
                {L"Region", Settings::values.region_index.GetValue() == Settings::Region::Usa ? L"USA" : L"Other"},
                {L"Time Zone", L"Auto"},
                {L"Device Name", std::wstring(Settings::values.device_name.GetValue().begin(), Settings::values.device_name.GetValue().end())},
                {L"Custom RTC", Settings::values.custom_rtc_enabled.GetValue() ? L"Enabled" : L"Disabled"},
                {L"RNG Seed", L"00000000"},
                {L"Multicore CPU", Settings::values.use_multi_core.GetValue() ? L"Enabled" : L"Disabled"},
                {L"Memory Layout", Settings::values.memory_layout_mode.GetValue() == Settings::MemoryLayout::Memory_4Gb ? L"4GB" : L"6GB"},
            };
            for (size_t i = 0; i < items.size(); ++i) {
                RectF rowRect(40, contentY, (REAL)width - 80, 40);
                RectF labelRect(rowRect.X + 10, rowRect.Y + 10, 200, 20);
                RectF valRect(rowRect.X + 250, rowRect.Y + 10, 300, 20);
                if ((int)i == g_SelectedSettingIndex) {
                    SolidBrush b = g_IsEditingSetting ? SolidBrush(COLOR_EDITING) : SolidBrush(COLOR_HIGHLIGHT_BROWN);
                    graphics.FillRectangle(&b, rowRect);
                } 
                SolidBrush fieldBrush(COLOR_ITEM_BG);
                graphics.FillRectangle(&fieldBrush, valRect);
                std::wstring displayVal = items[i].val;
                if ((int)i == g_SelectedSettingIndex && g_IsEditingSetting) displayVal = L"< " + displayVal + L" >";
                graphics.DrawString(items[i].label.c_str(), -1, &labelFont, labelRect, &leftAlign, &textBrush);
                graphics.DrawString(displayVal.c_str(), -1, &valFont, valRect, &leftAlign, &textBrush);
                contentY += 50;
            }
        } else if (g_CurrentTab == SettingsTab::General) {
            const wchar_t* actions[] = {L"Install Prod Keys", L"Install Firmware", L"Add Game Directory", L"Install Update (NSP)", L"Install Update (XCI)"};
            for (int i = 0; i < 5; ++i) {
                RectF rowRect(40, contentY, (REAL)width - 80, 40);
                if ((int)i == g_SelectedSettingIndex) {
                    SolidBrush hlBrush(COLOR_HIGHLIGHT_BROWN);
                    graphics.FillRectangle(&hlBrush, rowRect);
                } else {
                    graphics.FillRectangle(&inactiveBrush, rowRect);
                }
                graphics.DrawString(actions[i], -1, &labelFont, rowRect, &format, &textBrush);
                contentY += 50;
            }
        }
    }
    Font hintFont(&fontFamily, 14, FontStyleRegular, UnitPixel);
    SolidBrush hintBrush(COLOR_TEXT_DIM);
    RectF fR(20, (REAL)height - 30, (REAL)width, 20);
    StringFormat fF;
    fF.SetAlignment(StringAlignmentNear);
    if (g_AppState == AppState::Settings)
        graphics.DrawString(L"LB/RB: Tab | A: Select | B: Back", -1, &hintFont, fR, &fF, &hintBrush);
    else
        graphics.DrawString(L"A: Play | Start: Settings", -1, &hintFont, fR, &fF, &hintBrush);
}

static void StartGame(HWND hwnd, const Game& game) {
    std::filesystem::path root = GetUserDirectory();

    // 1. Check for Keys (UI Check)
    std::filesystem::path keyPath = root / "keys/prod.keys";
    if (!fs::exists(keyPath)) {
        std::wstring msg = L"prod.keys MISSING!\nLocation:\n" + keyPath.wstring() + L"\n\nPlease use Settings > Install Prod Keys";
        MessageBoxW(hwnd, msg.c_str(), L"Missing Files", MB_ICONERROR);
        return;
    }

    // 2. Check for Firmware (UI Check)
    std::filesystem::path nandPath = root / "nand/system/Contents/registered";
    bool hasFirmware = false;
    if (fs::exists(nandPath)) {
        for (const auto& entry : fs::directory_iterator(nandPath)) {
            if (entry.path().extension() == ".nca") {
                hasFirmware = true;
                break;
            }
        }
    }
    if (!hasFirmware) {
        std::wstring msg = L"Firmware MISSING!\nLocation:\n" + nandPath.wstring() + L"\n\nFolder must contain .nca files.\nPlease use Settings > Install Firmware";
        MessageBoxW(hwnd, msg.c_str(), L"Missing Files", MB_ICONERROR);
        return;
    }

    try {
        if (!g_System) g_System = std::make_unique<Core::System>();
        if (!g_EmuWindow) g_EmuWindow = std::make_unique<XboxEmuWindow>(hwnd);

        // Core Configuration
        Settings::values.renderer_backend.SetValue(Settings::RendererBackend::D3D12);
        Settings::values.use_disk_shader_cache.SetValue(true);
        Settings::values.use_asynchronous_gpu_emulation.SetValue(true);
        
        g_System->Initialize();
        g_System->SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
        g_System->SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());

        Service::AM::FrontendAppletParameters params{};
        params.launch_type = Service::AM::LaunchType::FrontendInitiated;

        Core::SystemResultStatus load_result = g_System->Load(*g_EmuWindow, game.path.string(), params);

        if (load_result != Core::SystemResultStatus::Success) {
            std::string err = fmt::format("Boot Failed Error Code: {}", (int)load_result);
            std::wstring werr(err.begin(), err.end());
            MessageBoxW(hwnd, werr.c_str(), L"Boot Error", MB_OK);
            return;
        }

        g_System->GPU().Start();
        g_System->GetCpuManager().OnGpuReady();

        g_AppState = AppState::Running;
        InvalidateRect(hwnd, NULL, FALSE);
        g_System->Run();
    } catch (const std::exception& e) {
        std::string err = fmt::format("Crash: {}", e.what());
        std::wstring werr(err.begin(), err.end());
        MessageBoxW(hwnd, werr.c_str(), L"Critical Error", MB_OK | MB_ICONERROR);
    }
}

static void HandleInput(HWND hwnd) {
    ULONGLONG currentTime = GetTickCount64();
    XINPUT_STATE state;
    bool up = false, down = false, lb = false, rb = false, a_btn = false, b_btn = false, start = false;
    bool any_connected = false;

    for (DWORD i = 0; i < MAX_CONTROLLERS; ++i) {
        if (XInputGetState(i, &state) == ERROR_SUCCESS) {
            any_connected = true;
            short ly = state.Gamepad.sThumbLY;
            WORD btns = state.Gamepad.wButtons;
            if ((btns & XINPUT_GAMEPAD_DPAD_UP) || (ly > INPUT_DEADZONE)) up = true;
            if ((btns & XINPUT_GAMEPAD_DPAD_DOWN) || (ly < -INPUT_DEADZONE)) down = true;
            if (btns & XINPUT_GAMEPAD_LEFT_SHOULDER) lb = true;
            if (btns & XINPUT_GAMEPAD_RIGHT_SHOULDER) rb = true;
            if (btns & XINPUT_GAMEPAD_A) a_btn = true;
            if (btns & XINPUT_GAMEPAD_B) b_btn = true;
            if (btns & XINPUT_GAMEPAD_START) start = true;
        }
    }

    WORD currentMask = 0;
    if (up) currentMask |= 1;
    if (down) currentMask |= 2;
    if (lb) currentMask |= 4;
    if (rb) currentMask |= 8;
    if (a_btn) currentMask |= 16;
    if (b_btn) currentMask |= 32;
    if (start) currentMask |= 64;

    bool execute = false;
    if (currentMask != 0) {
        if (currentMask != g_LastInputMask) {
            execute = true;
            g_NextInputTime = currentTime + 400; 
        } else if (currentTime >= g_NextInputTime) {
            execute = true;
            g_NextInputTime = currentTime + 50; 
        }
    }
    g_LastInputMask = currentMask;

    if (!execute) return;

    if (any_connected) {
        if (g_AppState == AppState::GameList) {
            if (start) {
                g_AppState = AppState::Settings;
                g_CurrentTab = SettingsTab::General;
                g_SelectedSettingIndex = 0;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            if (!g_Games.empty()) {
                if (up && g_SelectedGameIndex > 0) { g_SelectedGameIndex--; InvalidateRect(hwnd, NULL, FALSE); }
                if (down && g_SelectedGameIndex < (int)g_Games.size() - 1) { g_SelectedGameIndex++; InvalidateRect(hwnd, NULL, FALSE); }
                if (a_btn && g_SelectedGameIndex >= 0) StartGame(hwnd, g_Games[g_SelectedGameIndex]);
            }
        } else if (g_AppState == AppState::Settings) {
            if (!g_IsEditingSetting) {
                if (b_btn || start) {
                    g_AppState = AppState::GameList;
                    SaveSettings(); 
                    InvalidateRect(hwnd, NULL, FALSE);
                }
                if (lb) {
                    int t = (int)g_CurrentTab - 1; if (t < 0) t = 4;
                    g_CurrentTab = (SettingsTab)t; g_SelectedSettingIndex = 0; InvalidateRect(hwnd, NULL, FALSE);
                }
                if (rb) {
                    int t = (int)g_CurrentTab + 1; if (t > 4) t = 0;
                    g_CurrentTab = (SettingsTab)t; g_SelectedSettingIndex = 0; InvalidateRect(hwnd, NULL, FALSE);
                }

                int limit = (g_CurrentTab == SettingsTab::System) ? 8 : (g_CurrentTab == SettingsTab::General ? 5 : 0);
                if (up && g_SelectedSettingIndex > 0) { g_SelectedSettingIndex--; InvalidateRect(hwnd, NULL, FALSE); }
                if (down && g_SelectedSettingIndex < limit - 1) { g_SelectedSettingIndex++; InvalidateRect(hwnd, NULL, FALSE); }

                if (a_btn) {
                    if (g_CurrentTab == SettingsTab::General) {
                        if (g_SelectedSettingIndex == 0) InstallFiles(hwnd, L"Select Keys Folder", "keys");
                        if (g_SelectedSettingIndex == 1) InstallFiles(hwnd, L"Select Firmware Folder", "nand/system/Contents/registered");
                        if (g_SelectedSettingIndex == 2) InstallFiles(hwnd, L"Add Game Directory", "");
                    } else if (g_CurrentTab == SettingsTab::System) {
                        if (g_SelectedSettingIndex == 0 || g_SelectedSettingIndex == 1 || g_SelectedSettingIndex == 4 || g_SelectedSettingIndex == 6 || g_SelectedSettingIndex == 7) {
                            g_IsEditingSetting = true; InvalidateRect(hwnd, NULL, FALSE);
                        }
                    }
                }
            } else {
                if (b_btn || a_btn) { g_IsEditingSetting = false; InvalidateRect(hwnd, NULL, FALSE); }
                if (up || down || lb || rb) {
                    switch (g_SelectedSettingIndex) {
                    case 0: { 
                        int lang = (int)Settings::values.language_index.GetValue();
                        lang = (lang + (up ? 1 : -1) + 18) % 18;
                        Settings::values.language_index.SetValue((Settings::Language)lang);
                        break; 
                    }
                    case 1: {
                        int reg = (int)Settings::values.region_index.GetValue();
                        reg = (reg + (up ? 1 : -1) + 6) % 6;
                        Settings::values.region_index.SetValue((Settings::Region)reg);
                        break;
                    }
                    case 4: Settings::values.custom_rtc_enabled.SetValue(!Settings::values.custom_rtc_enabled.GetValue()); break;
                    case 6: Settings::values.use_multi_core.SetValue(!Settings::values.use_multi_core.GetValue()); break;
                    case 7: {
                        auto c = Settings::values.memory_layout_mode.GetValue();
                        Settings::values.memory_layout_mode.SetValue(c == Settings::MemoryLayout::Memory_4Gb ? Settings::MemoryLayout::Memory_6Gb : Settings::MemoryLayout::Memory_4Gb);
                        break;
                    }
                    }
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
        }
    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBitmap = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        SelectObject(memDC, memBitmap);
        RenderUI(memDC, rc.right, rc.bottom);
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
        DeleteObject(memBitmap); DeleteDC(memDC); EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_DESTROY:
        SaveSettings();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    // CRITICAL: Force Env Vars to Writable Location
    std::filesystem::path userDir = GetUserDirectory();
    std::wstring userDirStr = userDir.wstring();
    
    SetEnvironmentVariableW(L"CITRON_DATA_DIR", userDirStr.c_str());
    SetEnvironmentVariableW(L"CITRON_HOME", userDirStr.c_str());
    SetEnvironmentVariableW(L"YUZU_DATA_DIR", userDirStr.c_str());
    SetEnvironmentVariableW(L"YUZU_HOME", userDirStr.c_str());
    SetEnvironmentVariableW(L"XDG_DATA_HOME", userDirStr.c_str());
    SetEnvironmentVariableW(L"XDG_CONFIG_HOME", userDirStr.c_str());

    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, NULL);
    EnforceMemoryLimit();

    const wchar_t CLASS_NAME[] = L"CitronXboxWindowClass";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, CLASS_NAME, L"Citron", WS_POPUP | WS_VISIBLE, 0, 0, 1920, 1080, NULL, NULL, hInstance, NULL);
    if (!hwnd) return 0;
    ShowWindow(hwnd, SW_MAXIMIZE);

    LoadSettings();
    ScanGames();

    MSG msg = {};
    while (true) {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) break;
            TranslateMessage(&msg); DispatchMessage(&msg);
        } else {
            HandleInput(hwnd);
            Sleep(16);
        }
    }
    GdiplusShutdown(g_gdiplusToken);
    CoUninitialize();
    return 0;
}