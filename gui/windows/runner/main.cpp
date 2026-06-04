#include <flutter/dart_project.h>
#include <flutter/flutter_view_controller.h>
#include <windows.h>

#include <cstdlib>
#include <string>

#include "flutter_window.h"
#include "utils.h"

namespace {

bool FileExists(const std::wstring& path) {
  const DWORD attrs = GetFileAttributesW(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring ParentPath(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos || slash == 0) {
    return L"";
  }
  return path.substr(0, slash);
}

std::wstring FindProjectRoot(std::wstring dir) {
  while (!dir.empty()) {
    if (FileExists(dir + L"\\AGENTS.md") && FileExists(dir + L"\\CMakeLists.txt")) {
      return dir;
    }
    const std::wstring parent = ParentPath(dir);
    if (parent.empty() || parent == dir) {
      break;
    }
    dir = parent;
  }

  wchar_t cwd[MAX_PATH] = {};
  GetCurrentDirectoryW(MAX_PATH, cwd);
  return cwd;
}

std::wstring ExecutableDir() {
  wchar_t exe_path[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  return ParentPath(exe_path);
}

std::wstring GuiLogPath() {
  wchar_t cwd[MAX_PATH] = {};
  GetCurrentDirectoryW(MAX_PATH, cwd);
  const std::wstring cwd_root = FindProjectRoot(cwd);
  if (FileExists(cwd_root + L"\\AGENTS.md")) {
    return cwd_root + L"\\dlss-compositor-gui.log";
  }
  return FindProjectRoot(ExecutableDir()) + L"\\dlss-compositor-gui.log";
}

void ResetGuiLog() {
#if !defined(_DEBUG)
  if (std::getenv("DLSS_COMPOSITOR_GUI_LOG") == nullptr) {
    return;
  }
#endif
  const std::wstring path = GuiLogPath();
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return;
  }
  const char line[] = "[native:startup] GUI log reset\n";
  DWORD written = 0;
  WriteFile(file, line, static_cast<DWORD>(sizeof(line) - 1), &written, nullptr);
  CloseHandle(file);
}

}  // namespace

int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE prev,
                      _In_ wchar_t *command_line, _In_ int show_command) {
  ResetGuiLog();

  // Attach to console when present (e.g., 'flutter run') or create a
  // new console when running with a debugger.
  if (!::AttachConsole(ATTACH_PARENT_PROCESS) && ::IsDebuggerPresent()) {
    CreateAndAttachConsole();
  }

  // Initialize COM, so that it is available for use in the library and/or
  // plugins.
  ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  flutter::DartProject project(L"data");

  std::vector<std::string> command_line_arguments =
      GetCommandLineArguments();

  project.set_dart_entrypoint_arguments(std::move(command_line_arguments));

  FlutterWindow window(project);
  Win32Window::Point origin(10, 10);
  Win32Window::Size size(1280, 720);
  if (!window.Create(L"dlss_compositor_gui", origin, size)) {
    return EXIT_FAILURE;
  }
  window.SetQuitOnClose(true);

  ::MSG msg;
  while (::GetMessage(&msg, nullptr, 0, 0)) {
    ::TranslateMessage(&msg);
    ::DispatchMessage(&msg);
  }

  ::CoUninitialize();
  return EXIT_SUCCESS;
}
