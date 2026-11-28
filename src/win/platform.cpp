// Windows: running commands, opening files, drives, Recycle Bin, file copy.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <cerrno>
#include <cstdlib>
#include <string>
#include <vector>

#include "../common.h"

static std::wstring winPathW(const std::string& p) {
  std::wstring w = u8to16(p);
  for (auto& c : w)
    if (c == L'/') c = L'\\';
  if (w.size() == 2 && w[1] == L':') w += L'\\';
  return w;
}

static std::string lastErrorText(DWORD e) {
  wchar_t* buf = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                 nullptr, e, 0, (LPWSTR)&buf, 0, nullptr);
  std::string s = buf ? u16to8(buf) : "error " + std::to_string(e);
  if (buf) LocalFree(buf);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == '.')) s.pop_back();
  return s;
}

int sysRun(const std::string& command, const std::string& cwd, const std::string& other) {
  const char* cs = getenv("COMSPEC");
  std::wstring shell = u8to16((cs && *cs) ? cs : "C:\\Windows\\System32\\cmd.exe");
  // cmd /S /C "<command>": /S strips exactly the outer quotes and keeps the rest verbatim
  std::wstring cl = L"\"" + shell + L"\" /S /C \"" + u8to16(command) + L"\"";
  SetEnvironmentVariableW(L"TWINC_OTHER", winPathW(other).c_str());
  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  std::wstring dir = winPathW(cwd);
  if (!CreateProcessW(shell.c_str(), &cl[0], nullptr, nullptr, TRUE, 0, nullptr, dir.c_str(), &si, &pi))
    return -1;
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return (int)code;
}

// 0 = opened, 1 = no application associated, -1 = no desktop (e.g. an SSH session)
int sysOpen(const std::string& path) {
  const char* ssh = getenv("SSH_CONNECTION");
  if (ssh && *ssh) return -1;
  std::wstring w = winPathW(path);
  SHELLEXECUTEINFOW sei{};
  sei.cbSize = sizeof sei;
  sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
  sei.lpFile = w.c_str();
  sei.nShow = SW_SHOWNORMAL;
  if (ShellExecuteExW(&sei)) return 0;
  DWORD e = GetLastError();
  return e == ERROR_NO_ASSOCIATION ? 1 : -1;
}

std::vector<std::string> sysDrives() {
  std::vector<std::string> r;
  DWORD m = GetLogicalDrives();
  for (int i = 0; i < 26; i++) {
    if (!(m & (1u << i))) continue;
    std::string d = std::string(1, (char)('A' + i)) + ":/";
    UINT t = GetDriveTypeW(winPathW(d).c_str());
    if (t == DRIVE_NO_ROOT_DIR) continue;
    r.push_back(d);
  }
  return r;
}

std::string sysVolumeInfo(const std::string& path, std::string& fsName, unsigned long long& total,
                          unsigned long long& avail) {
  std::wstring root = winPathW(path.substr(0, 3));
  wchar_t label[MAX_PATH + 1] = L"", fs[MAX_PATH + 1] = L"";
  GetVolumeInformationW(root.c_str(), label, MAX_PATH, nullptr, nullptr, nullptr, fs, MAX_PATH);
  fsName = u16to8(fs);
  ULARGE_INTEGER a{}, t{};
  if (GetDiskFreeSpaceExW(winPathW(path).c_str(), &a, &t, nullptr)) {
    total = t.QuadPart;
    avail = a.QuadPart;
  } else {
    total = avail = 0;
  }
  return u16to8(label);
}

bool sysTrash(const std::string& path, std::string& err) {
  std::wstring from = winPathW(path);
  from.push_back(L'\0');  // double-NUL terminated list
  SHFILEOPSTRUCTW op{};
  op.wFunc = FO_DELETE;
  op.pFrom = from.c_str();
  op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT | FOF_WANTNUKEWARNING;
  int r = SHFileOperationW(&op);
  if (r == 0 && !op.fAnyOperationsAborted) return true;
  err = r ? lastErrorText((DWORD)r) : "cancelled";
  return false;
}

// Read-only/system attributes block deletion on Windows; clear them for a retry.
int sysClearAttrs(const char* path) {
  return SetFileAttributesW(winPathW(path).c_str(), FILE_ATTRIBUTE_NORMAL) ? 0 : -1;
}

struct CopyCtx {
  bool (*progress)(unsigned long long, void*);
  void* ctx;
};

static DWORD CALLBACK copyProgress(LARGE_INTEGER, LARGE_INTEGER done, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD,
                                   HANDLE, HANDLE, LPVOID data) {
  CopyCtx* c = (CopyCtx*)data;
  return c->progress((unsigned long long)done.QuadPart, c->ctx) ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
}

int sysCopyFile(const std::string& src, const std::string& dst, bool (*progress)(unsigned long long, void*),
                void* ctx) {
  CopyCtx c{progress, ctx};
  BOOL cancel = FALSE;
  if (CopyFileExW(winPathW(src).c_str(), winPathW(dst).c_str(), copyProgress, &c, &cancel,
                  COPY_FILE_FAIL_IF_EXISTS))
    return 0;
  DWORD e = GetLastError();
  switch (e) {
    case ERROR_REQUEST_ABORTED: errno = ECANCELED; break;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: errno = EEXIST; break;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: errno = EACCES; break;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: errno = ENOSPC; break;
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: errno = ENOENT; break;
    default: errno = EIO;
  }
  return -1;
}
#endif
