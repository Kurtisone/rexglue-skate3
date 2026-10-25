/**
 * @file        ui/windowed_app_main_sdl.cpp
 * @brief       Entry point for windowed applications (SDL3 windowing)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context_sdl.h>

#if REX_PLATFORM_WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#endif

#include <SDL3/SDL_hints.h>

#if REX_PLATFORM_ANDROID
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <unwind.h>

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include <SDL3/SDL_system.h>

#include <rex/main_android.h>

#include <android/log.h>

#include <filesystem>
#include <fstream>
#include <iterator>

#if REX_HAS_ADRENOTOOLS
#include <adrenotools/driver.h>

namespace rex::ui::vulkan {
// rex/ui/vulkan/instance.h (not included: it needs the Vulkan headers).
void SetLoaderLibraryOverride(void* dlopen_handle);
}  // namespace rex::ui::vulkan
#endif
#endif

#if REX_PLATFORM_WIN32
REXCVAR_DEFINE_BOOL(win32_high_resolution_timer, true, "Win32",
                    "Requests high-resolution timer from the NT kernel");
REXCVAR_DEFINE_BOOL(win32_mmcss, true, "Win32",
                    "Opt in the Multimedia Class Scheduler Service (MMCSS) scheduling for "
                    "prioritized access to CPU resources");
#endif

namespace {

#if REX_PLATFORM_WIN32
void RequestWin32HighResolutionTimer() {
  HMODULE ntdll_module = GetModuleHandleW(L"ntdll.dll");
  if (!ntdll_module) {
    return;
  }

  // Xenia requests the maximum NT timer resolution directly rather than using
  // timeBeginPeriod(1), which is coarser and may override this setting in SDL.
  using NtQueryTimerResolutionFn = LONG(NTAPI*)(PULONG minimum_resolution,
                                                PULONG maximum_resolution,
                                                PULONG current_resolution);
  using NtSetTimerResolutionFn = LONG(NTAPI*)(ULONG desired_resolution,
                                              BOOLEAN set_resolution,
                                              PULONG current_resolution);
  auto nt_query_timer_resolution = reinterpret_cast<NtQueryTimerResolutionFn>(
      GetProcAddress(ntdll_module, "NtQueryTimerResolution"));
  auto nt_set_timer_resolution =
      reinterpret_cast<NtSetTimerResolutionFn>(GetProcAddress(ntdll_module, "NtSetTimerResolution"));
  if (!nt_query_timer_resolution || !nt_set_timer_resolution) {
    return;
  }

  ULONG minimum_resolution = 0;
  ULONG maximum_resolution = 0;
  ULONG current_resolution = 0;
  nt_query_timer_resolution(&minimum_resolution, &maximum_resolution, &current_resolution);
  nt_set_timer_resolution(maximum_resolution, TRUE, &current_resolution);
}

void RequestWin32MMCSS() {
  HMODULE dwmapi_module = LoadLibraryW(L"dwmapi.dll");
  if (!dwmapi_module) {
    return;
  }

  using DwmEnableMMCSSFn = HRESULT(STDAPICALLTYPE*)(BOOL enable_mmcss);
  auto dwm_enable_mmcss =
      reinterpret_cast<DwmEnableMMCSSFn>(GetProcAddress(dwmapi_module, "DwmEnableMMCSS"));
  if (dwm_enable_mmcss) {
    dwm_enable_mmcss(TRUE);
  }
  FreeLibrary(dwmapi_module);
}
#endif

int RunWindowedApp(int argc, char** argv) {
  auto remaining = rex::cvar::Init(argc, argv);
  rex::cvar::ApplyEnvironment();
  rex::InitLoggingEarly();

#if REX_PLATFORM_WIN32
  // Mirrors Xenia's InitializeWin32App timing setup: parse cvars first, then
  // request high-performance timing and compositor scheduling before windows
  // and render/audio/input subsystems start.
  if (REXCVAR_GET(win32_high_resolution_timer)) {
    RequestWin32HighResolutionTimer();
  }
  if (REXCVAR_GET(win32_mmcss)) {
    RequestWin32MMCSS();
  }
#endif
#if REX_PLATFORM_MAC
  // Use native macOS fullscreen so the menu bar and app switching behave like
  // other Mac apps.
  SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, "1");
  SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_MENU_VISIBILITY, "1");
#endif

  int result;
  {
    rex::ui::SDLWindowedAppContext app_context;
    if (!app_context.Initialize()) {
      return EXIT_FAILURE;
    }

#if REX_PLATFORM_WIN32
    // Apartment-threaded COM for shell dialogs.
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
      return EXIT_FAILURE;
    }
#endif

    std::unique_ptr<rex::ui::WindowedApp> app = rex::ui::GetWindowedAppCreator()(app_context);

    // Match remaining positional args to the app's expected options.
    const auto& option_names = app->GetPositionalOptions();
    std::map<std::string, std::string> parsed;
    size_t count = std::min(remaining.size(), option_names.size());
    for (size_t i = 0; i < count; ++i) {
      parsed[option_names[i]] = remaining[i];
    }
    app->SetParsedArguments(std::move(parsed));

    result = app->OnInitialize() ? app_context.RunMainMessageLoop() : EXIT_FAILURE;

    app->InvokeOnDestroy();
  }

#if REX_PLATFORM_WIN32
  CoUninitialize();
#endif

  return result;
}

#if REX_PLATFORM_WIN32
// Convert wide argv from CommandLineToArgvW to UTF-8 for cvar::Init.
std::vector<std::string> WideArgsToUtf8(int argc, wchar_t** wargv) {
  std::vector<std::string> args;
  args.reserve(static_cast<size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    std::wstring wide(wargv[i]);
    if (wide.empty()) {
      args.emplace_back();
      continue;
    }
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr,
                                   0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), size,
                        nullptr, nullptr);
    args.push_back(std::move(utf8));
  }
  return args;
}
#endif

}  // namespace

#if REX_PLATFORM_WIN32

int WINAPI wWinMain(HINSTANCE hinstance, HINSTANCE hinstance_prev, LPWSTR command_line,
                    int show_cmd) {
  (void)hinstance;
  (void)hinstance_prev;
  (void)command_line;
  (void)show_cmd;

  int wargc = 0;
  wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
  auto utf8_args = WideArgsToUtf8(wargc, wargv);
  LocalFree(wargv);

  std::vector<char*> argv_ptrs;
  argv_ptrs.reserve(utf8_args.size());
  for (auto& s : utf8_args) {
    argv_ptrs.push_back(s.data());
  }
  return RunWindowedApp(static_cast<int>(argv_ptrs.size()), argv_ptrs.data());
}

#elif REX_PLATFORM_ANDROID

namespace {

// Fatal signal report for devices without adb: crash.txt in the app folder
// lists the faulting address and each frame as library + offset, to
// symbolize against the unstripped libraries of the build. Then the previous
// handler (Android's debuggerd) still writes its tombstone.
int crash_fd = -1;
struct sigaction previous_crash_actions[NSIG];

void CrashWrite(const char* text) {
  if (crash_fd >= 0) {
    write(crash_fd, text, strlen(text));
  }
}

void CrashWriteHex(const char* label, uintptr_t value) {
  char buffer[64];
  snprintf(buffer, sizeof(buffer), "%s0x%016" PRIxPTR "\n", label, value);
  CrashWrite(buffer);
}

void CrashWriteFrame(int index, uintptr_t pc) {
  char buffer[512];
  Dl_info info;
  if (dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_fname) {
    const char* name = strrchr(info.dli_fname, '/');
    snprintf(buffer, sizeof(buffer), "#%02d pc 0x%016" PRIxPTR "  %s +0x%" PRIxPTR "  (%s)\n", index,
             pc, name ? name + 1 : info.dli_fname, pc - uintptr_t(info.dli_fbase),
             info.dli_sname ? info.dli_sname : "?");
  } else {
    snprintf(buffer, sizeof(buffer), "#%02d pc 0x%016" PRIxPTR "\n", index, pc);
  }
  CrashWrite(buffer);
}

struct BacktraceState {
  int count;
};

_Unwind_Reason_Code CrashUnwindFrame(_Unwind_Context* context, void* arg) {
  auto* state = static_cast<BacktraceState*>(arg);
  uintptr_t pc = _Unwind_GetIP(context);
  if (pc) {
    CrashWriteFrame(state->count, pc);
  }
  return ++state->count < 64 ? _URC_NO_REASON : _URC_END_OF_STACK;
}

void CrashHandler(int signal_number, siginfo_t* info, void* context) {
  char buffer[192];
  // When, and which build (the compile time of this file).
  time_t now = time(nullptr);
  struct tm local_time;
  char when[32] = "?";
  if (localtime_r(&now, &local_time)) {
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &local_time);
  }
  snprintf(buffer, sizeof(buffer), "\n%s, build " __DATE__ " " __TIME__
           ": fatal signal %d (code %d) in thread %d\n",
           when, signal_number, info ? info->si_code : 0, int(gettid()));
  CrashWrite(buffer);
  if (info) {
    CrashWriteHex("fault address ", uintptr_t(info->si_addr));
  }
#if defined(__aarch64__)
  if (context) {
    const auto& mcontext = static_cast<ucontext_t*>(context)->uc_mcontext;
    CrashWriteHex("pc ", uintptr_t(mcontext.pc));
    CrashWriteFrame(-1, uintptr_t(mcontext.pc));
    CrashWriteHex("lr ", uintptr_t(mcontext.regs[30]));
    CrashWriteFrame(-2, uintptr_t(mcontext.regs[30]));
    CrashWriteHex("sp ", uintptr_t(mcontext.sp));
  }
#endif
  CrashWrite("backtrace (from the handler):\n");
  BacktraceState state{0};
  _Unwind_Backtrace(CrashUnwindFrame, &state);
  if (crash_fd >= 0) {
    fsync(crash_fd);
  }
  // Hand over to the previous handler (debuggerd), then die as before.
  sigaction(signal_number, &previous_crash_actions[signal_number], nullptr);
  raise(signal_number);
}

void InstallCrashReporter(const char* folder) {
  std::string path = std::string(folder) + "/crash.txt";
  crash_fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  struct sigaction action = {};
  action.sa_sigaction = CrashHandler;
  action.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&action.sa_mask);
  for (int signal_number : {SIGSEGV, SIGBUS, SIGILL, SIGABRT, SIGFPE, SIGTRAP}) {
    sigaction(signal_number, &action, &previous_crash_actions[signal_number]);
  }
}

}  // namespace

// The app folder (game/, config, logs, user data): a skate1/ folder at the
// root of the internal storage or of a microSD card, readable with the "All
// files access" permission and reachable by any file manager or transfer, if
// it holds game/default.xex; else the app's own external files folder
// (Android/data/<package>/files), which needs no permission.
std::string FindAppRoot() {
  std::vector<std::string> candidates = {"/storage/emulated/0/skate1"};
  if (DIR* storage = opendir("/storage")) {
    while (dirent* entry = readdir(storage)) {
      std::string name = entry->d_name;
      if (name != "." && name != ".." && name != "emulated" && name != "self") {
        candidates.push_back("/storage/" + name + "/skate1");
      }
    }
    closedir(storage);
  }
  for (const std::string& candidate : candidates) {
    if (access((candidate + "/game/default.xex").c_str(), R_OK) == 0) {
      return candidate;
    }
  }
  const char* external = SDL_GetAndroidExternalStoragePath();
  return external ? external : "";
}

#if REX_HAS_ADRENOTOOLS
// A custom Vulkan driver (Turnip, a newer Qualcomm driver) unpacked into
// <app folder>/driver/: its files are copied into the app's private storage
// (code can't be loaded from shared storage) and opened through adrenotools.
// The outcome goes to <app folder>/driver_status.txt.
void LoadCustomVulkanDriver(const std::string& root) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path source = fs::path(root) / "driver";
  if (!fs::is_directory(source, ec)) {
    std::error_code remove_ec;
    fs::remove(fs::path(root) / "driver_status.txt", remove_ec);
    return;
  }
  std::string status;
  auto finish = [&](const std::string& message) {
    status += message + "\n";
    __android_log_print(ANDROID_LOG_INFO, "rexglue", "%s", message.c_str());
    FILE* file = fopen((fs::path(root) / "driver_status.txt").c_str(), "w");
    if (file) {
      fputs(status.c_str(), file);
      fclose(file);
    }
  };
  const char* internal = SDL_GetAndroidInternalStoragePath();
  if (!internal) {
    finish("No private storage folder for the driver");
    return;
  }
  const fs::path target = fs::path(internal) / "gpu_driver";
  fs::remove_all(target, ec);
  fs::create_directories(target, ec);
  // The driver's library: meta.json's "libraryName" (Turnip / AdrenoTools
  // packages), else the first .so with "vulkan" in its name.
  std::string library_name;
  for (const auto& entry : fs::directory_iterator(source, ec)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    fs::copy_file(entry.path(), target / entry.path().filename(),
                  fs::copy_options::overwrite_existing, ec);
    const std::string name = entry.path().filename().string();
    if (name == "meta.json") {
      std::ifstream meta(entry.path());
      std::string text((std::istreambuf_iterator<char>(meta)), std::istreambuf_iterator<char>());
      size_t key = text.find("\"libraryName\"");
      size_t open_quote = key == std::string::npos ? key : text.find('"', text.find(':', key));
      size_t close_quote =
          open_quote == std::string::npos ? open_quote : text.find('"', open_quote + 1);
      if (close_quote != std::string::npos) {
        library_name = text.substr(open_quote + 1, close_quote - open_quote - 1);
      }
    } else if (library_name.empty() && name.size() > 3 &&
               name.compare(name.size() - 3, 3, ".so") == 0 &&
               name.find("vulkan") != std::string::npos) {
      library_name = name;
    }
  }
  if (library_name.empty()) {
    finish("No driver library found in " + source.string() +
           " (expected meta.json or a *vulkan*.so)");
    return;
  }
  // The hooks sit next to this library, in the APK's native library folder.
  Dl_info info;
  if (!dladdr(reinterpret_cast<void*>(&LoadCustomVulkanDriver), &info) || !info.dli_fname) {
    finish("Could not find the native library folder");
    return;
  }
  const std::string hook_dir = fs::path(info.dli_fname).parent_path().string() + "/";
  const std::string driver_dir = target.string() + "/";
  void* handle = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, nullptr,
                                            hook_dir.c_str(), driver_dir.c_str(),
                                            library_name.c_str(), nullptr, nullptr);
  if (!handle) {
    finish("adrenotools could not load " + library_name + "; using the system driver");
    return;
  }
  rex::ui::vulkan::SetLoaderLibraryOverride(handle);
  finish("Loaded custom Vulkan driver " + library_name);
}
#endif  // REX_HAS_ADRENOTOOLS

// SDLActivity loads libmain.so and calls SDL_main on its own thread.
extern "C" __attribute__((visibility("default"))) int SDL_main(int argc, char* argv[]) {
  std::string root = FindAppRoot();
  if (!root.empty()) {
    setenv("REX_APP_ROOT", root.c_str(), 1);
    setenv("HOME", root.c_str(), 1);
    chdir(root.c_str());
    InstallCrashReporter(root.c_str());
#if REX_HAS_ADRENOTOOLS
    LoadCustomVulkanDriver(root);
#endif
  }
  rex::InitializeAndroid();
  int result = RunWindowedApp(argc, argv);
  rex::FlushLogging();
  std::_Exit(result);
}

#else

int main(int argc, char* argv[]) {
  int result = RunWindowedApp(argc, argv);
  // Guest threads that could not be stopped may still be running; global
  // static destruction under them can turn a clean quit into an abort or a
  // nonzero exit status. Flush the logs and end the process here.
  rex::FlushLogging();
  std::_Exit(result);
}

#endif
