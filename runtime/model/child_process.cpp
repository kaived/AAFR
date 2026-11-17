#include "runtime/model/child_process.h"

#include <mutex>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2
#endif
#include <psapi.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <thread>
#endif

namespace aafr::model {

std::string QuoteWindowsArgument(const std::string& arg) {
  if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) return arg;
  std::string out = "\"";
  std::size_t backslashes = 0;
  for (char c : arg) {
    if (c == '\\') {
      ++backslashes;
    } else if (c == '"') {
      out.append(backslashes * 2 + 1, '\\');
      out.push_back('"');
      backslashes = 0;
    } else {
      out.append(backslashes, '\\');
      out.push_back(c);
      backslashes = 0;
    }
  }
  out.append(backslashes * 2, '\\');
  out.push_back('"');
  return out;
}

#ifdef _WIN32

namespace {
std::wstring Widen(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::string LastError(const char* what) {
  return std::string(what) + " failed (Windows error " + std::to_string(GetLastError()) + ")";
}

void CloseIf(HANDLE& h) {
  if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
  h = nullptr;
}
}  // namespace

struct ChildProcess::Impl {
  HANDLE process{nullptr};
  HANDLE job{nullptr};
  HANDLE in_write{nullptr};
  HANDLE out_read{nullptr};
  HANDLE err_read{nullptr};
  DWORD pid{0};
  std::mutex write_mutex;
};

ChildProcess::ChildProcess() : impl_(new Impl) {}

ChildProcess::~ChildProcess() {
  Kill();
  Close();
  delete impl_;
}

bool ChildProcess::Start(const ProcessSpec& spec, std::string* error) {
  if (impl_->process) {
    if (error) *error = "process already started";
    return false;
  }
  const DWORD attributes = GetFileAttributesW(Widen(spec.executable).c_str());
  if (spec.executable.empty() || attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
    if (error) *error = "executable not found: " + spec.executable;
    return false;
  }
  SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE in_read = nullptr, out_write = nullptr, err_write = nullptr;
  if (!CreatePipe(&in_read, &impl_->in_write, &sa, 0) || !CreatePipe(&impl_->out_read, &out_write, &sa, 0) ||
      !CreatePipe(&impl_->err_read, &err_write, &sa, 0)) {
    if (error) *error = LastError("CreatePipe");
    CloseIf(in_read);
    CloseIf(out_write);
    CloseIf(err_write);
    Close();
    return false;
  }
  // The parent's ends must not leak into the child.
  SetHandleInformation(impl_->in_write, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(impl_->out_read, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(impl_->err_read, HANDLE_FLAG_INHERIT, 0);

  // Only the three child ends are inherited, even if another thread creates
  // inheritable handles concurrently.
  HANDLE inherit[3] = {in_read, out_write, err_write};
  SIZE_T attr_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
  std::vector<unsigned char> attr_buffer(attr_size);
  auto* attr_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buffer.data());
  bool attr_ok = InitializeProcThreadAttributeList(attr_list, 1, 0, &attr_size) &&
                 UpdateProcThreadAttribute(attr_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit),
                                           nullptr, nullptr);
  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof(si);
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdInput = in_read;
  si.StartupInfo.hStdOutput = out_write;
  si.StartupInfo.hStdError = err_write;
  si.lpAttributeList = attr_ok ? attr_list : nullptr;

  std::string command = QuoteWindowsArgument(spec.executable);
  for (const std::string& a : spec.args) command += " " + QuoteWindowsArgument(a);
  std::wstring wcommand = Widen(command);
  const std::wstring wexe = Widen(spec.executable);
  const std::wstring wcwd = Widen(spec.working_directory);
  PROCESS_INFORMATION pi{};
  const DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT |
                      (attr_ok ? EXTENDED_STARTUPINFO_PRESENT : 0);
  const BOOL created = CreateProcessW(wexe.c_str(), wcommand.data(), nullptr, nullptr, TRUE, flags, nullptr,
                                      wcwd.empty() ? nullptr : wcwd.c_str(), &si.StartupInfo, &pi);
  const DWORD create_error = GetLastError();
  if (attr_ok) DeleteProcThreadAttributeList(attr_list);
  CloseIf(in_read);
  CloseIf(out_write);
  CloseIf(err_write);
  if (!created) {
    if (error) *error = "CreateProcess failed (Windows error " + std::to_string(create_error) + ")";
    Close();
    return false;
  }
  impl_->process = pi.hProcess;
  impl_->pid = pi.dwProcessId;
  // Job: the worker dies with AAFR and cannot commit more than the limit.
  impl_->job = CreateJobObjectW(nullptr, nullptr);
  if (impl_->job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    if (spec.memory_limit_bytes > 0) {
      limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
      limits.ProcessMemoryLimit = static_cast<SIZE_T>(spec.memory_limit_bytes);
    }
    SetInformationJobObject(impl_->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    AssignProcessToJobObject(impl_->job, impl_->process);
  }
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  return true;
}

bool ChildProcess::Started() const { return impl_->process != nullptr; }

bool ChildProcess::Write(const std::string& data) {
  std::lock_guard<std::mutex> lock(impl_->write_mutex);
  if (!impl_->in_write) return false;
  std::size_t done = 0;
  while (done < data.size()) {
    DWORD written = 0;
    if (!WriteFile(impl_->in_write, data.data() + done, static_cast<DWORD>(data.size() - done), &written, nullptr) ||
        written == 0) {
      return false;
    }
    done += written;
  }
  return true;
}

void ChildProcess::CloseStdin() {
  std::lock_guard<std::mutex> lock(impl_->write_mutex);
  CloseIf(impl_->in_write);
}

namespace {
std::size_t ReadPipe(HANDLE h, char* buffer, std::size_t size) {
  if (!h) return 0;
  DWORD got = 0;
  if (!ReadFile(h, buffer, static_cast<DWORD>(size), &got, nullptr)) return 0;
  return got;
}
}  // namespace

std::size_t ChildProcess::ReadStdout(char* buffer, std::size_t size) { return ReadPipe(impl_->out_read, buffer, size); }
std::size_t ChildProcess::ReadStderr(char* buffer, std::size_t size) { return ReadPipe(impl_->err_read, buffer, size); }

std::optional<int> ChildProcess::Wait(int timeout_ms) {
  if (!impl_->process) return std::nullopt;
  if (WaitForSingleObject(impl_->process, timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms)) != WAIT_OBJECT_0) {
    return std::nullopt;
  }
  DWORD code = 0;
  GetExitCodeProcess(impl_->process, &code);
  return static_cast<int>(code);
}

bool ChildProcess::Running() const {
  return impl_->process && WaitForSingleObject(impl_->process, 0) == WAIT_TIMEOUT;
}

void ChildProcess::Kill() {
  if (!impl_->process) return;
  if (impl_->job) TerminateJobObject(impl_->job, 1);
  if (WaitForSingleObject(impl_->process, 0) == WAIT_TIMEOUT) TerminateProcess(impl_->process, 1);
  WaitForSingleObject(impl_->process, 5000);
}

void ChildProcess::Close() {
  {
    std::lock_guard<std::mutex> lock(impl_->write_mutex);
    CloseIf(impl_->in_write);
  }
  CloseIf(impl_->out_read);
  CloseIf(impl_->err_read);
  CloseIf(impl_->process);
  CloseIf(impl_->job);
  impl_->pid = 0;
}

std::uint64_t ChildProcess::Pid() const { return impl_->pid; }

ProcessResources ChildProcess::Resources() const {
  ProcessResources r;
  if (!impl_->process) return r;
  // On Windows a venv's python.exe is a launcher that starts the real
  // interpreter as its own child; both are in the job, so account the job.
  if (impl_->job) {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION acct{};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION ext{};
    if (QueryInformationJobObject(impl_->job, JobObjectBasicAccountingInformation, &acct, sizeof(acct), nullptr) &&
        QueryInformationJobObject(impl_->job, JobObjectExtendedLimitInformation, &ext, sizeof(ext), nullptr)) {
      r.available = true;
      r.cpu_user_ms = static_cast<double>(acct.TotalUserTime.QuadPart) / 10000.0;
      r.cpu_kernel_ms = static_cast<double>(acct.TotalKernelTime.QuadPart) / 10000.0;
      r.peak_working_set_bytes = ext.PeakJobMemoryUsed;  // Peak committed memory of the job.
      struct {
        JOBOBJECT_BASIC_PROCESS_ID_LIST list;
        ULONG_PTR more[63];
      } ids{};
      if (QueryInformationJobObject(impl_->job, JobObjectBasicProcessIdList, &ids, sizeof(ids), nullptr)) {
        for (DWORD i = 0; i < ids.list.NumberOfProcessIdsInList; ++i) {
          HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(ids.list.ProcessIdList[i]));
          if (!h) continue;
          PROCESS_MEMORY_COUNTERS_EX pc{};
          if (GetProcessMemoryInfo(h, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pc), sizeof(pc))) {
            r.working_set_bytes += pc.WorkingSetSize;
            r.private_bytes += pc.PrivateUsage;
          }
          CloseHandle(h);
        }
      }
      return r;
    }
  }
  PROCESS_MEMORY_COUNTERS_EX counters{};
  if (GetProcessMemoryInfo(impl_->process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
    r.available = true;
    r.working_set_bytes = counters.WorkingSetSize;
    r.peak_working_set_bytes = counters.PeakWorkingSetSize;
    r.private_bytes = counters.PrivateUsage;
  }
  FILETIME created, exited, kernel, user;
  if (GetProcessTimes(impl_->process, &created, &exited, &kernel, &user)) {
    const auto ms = [](const FILETIME& f) {
      return static_cast<double>((static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 10000.0;
    };
    r.cpu_user_ms = ms(user);
    r.cpu_kernel_ms = ms(kernel);
  }
  return r;
}

#else  // POSIX

struct ChildProcess::Impl {
  pid_t pid{-1};
  int in_write{-1};
  int out_read{-1};
  int err_read{-1};
  bool exited{false};
  int exit_code{0};
  std::mutex write_mutex;
};

ChildProcess::ChildProcess() : impl_(new Impl) {}

ChildProcess::~ChildProcess() {
  Kill();
  Close();
  delete impl_;
}

bool ChildProcess::Start(const ProcessSpec& spec, std::string* error) {
  if (impl_->pid > 0) {
    if (error) *error = "process already started";
    return false;
  }
  if (spec.executable.empty() || access(spec.executable.c_str(), X_OK) != 0) {
    if (error) *error = "executable not found: " + spec.executable;
    return false;
  }
  int in[2], out[2], err[2];
  if (pipe(in) != 0 || pipe(out) != 0 || pipe(err) != 0) {
    if (error) *error = "pipe failed";
    return false;
  }
  const pid_t pid = fork();
  if (pid < 0) {
    if (error) *error = "fork failed";
    return false;
  }
  if (pid == 0) {
    dup2(in[0], 0);
    dup2(out[1], 1);
    dup2(err[1], 2);
    for (int fd : {in[0], in[1], out[0], out[1], err[0], err[1]}) close(fd);
    if (spec.memory_limit_bytes > 0) {
      rlimit lim{static_cast<rlim_t>(spec.memory_limit_bytes), static_cast<rlim_t>(spec.memory_limit_bytes)};
      setrlimit(RLIMIT_AS, &lim);
    }
    if (!spec.working_directory.empty() && chdir(spec.working_directory.c_str()) != 0) _exit(127);
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(spec.executable.c_str()));
    for (const auto& a : spec.args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    execv(spec.executable.c_str(), argv.data());
    _exit(127);
  }
  close(in[0]);
  close(out[1]);
  close(err[1]);
  impl_->pid = pid;
  impl_->in_write = in[1];
  impl_->out_read = out[0];
  impl_->err_read = err[0];
  signal(SIGPIPE, SIG_IGN);
  return true;
}

bool ChildProcess::Started() const { return impl_->pid > 0; }

bool ChildProcess::Write(const std::string& data) {
  std::lock_guard<std::mutex> lock(impl_->write_mutex);
  if (impl_->in_write < 0) return false;
  std::size_t done = 0;
  while (done < data.size()) {
    const ssize_t n = write(impl_->in_write, data.data() + done, data.size() - done);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return false;
    }
    done += static_cast<std::size_t>(n);
  }
  return true;
}

void ChildProcess::CloseStdin() {
  std::lock_guard<std::mutex> lock(impl_->write_mutex);
  if (impl_->in_write >= 0) close(impl_->in_write);
  impl_->in_write = -1;
}

namespace {
std::size_t ReadFd(int fd, char* buffer, std::size_t size) {
  if (fd < 0) return 0;
  while (true) {
    const ssize_t n = read(fd, buffer, size);
    if (n < 0 && errno == EINTR) continue;
    return n > 0 ? static_cast<std::size_t>(n) : 0;
  }
}
}  // namespace

std::size_t ChildProcess::ReadStdout(char* buffer, std::size_t size) { return ReadFd(impl_->out_read, buffer, size); }
std::size_t ChildProcess::ReadStderr(char* buffer, std::size_t size) { return ReadFd(impl_->err_read, buffer, size); }

std::optional<int> ChildProcess::Wait(int timeout_ms) {
  if (impl_->pid <= 0) return std::nullopt;
  if (impl_->exited) return impl_->exit_code;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms < 0 ? 1 << 30 : timeout_ms);
  while (true) {
    int status = 0;
    const pid_t r = waitpid(impl_->pid, &status, WNOHANG);
    if (r == impl_->pid) {
      impl_->exited = true;
      impl_->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
      return impl_->exit_code;
    }
    if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

bool ChildProcess::Running() const {
  if (impl_->pid <= 0 || impl_->exited) return false;
  return kill(impl_->pid, 0) == 0 && !const_cast<ChildProcess*>(this)->Wait(0);
}

void ChildProcess::Kill() {
  if (impl_->pid <= 0 || impl_->exited) return;
  kill(impl_->pid, SIGKILL);
  Wait(5000);
}

void ChildProcess::Close() {
  {
    std::lock_guard<std::mutex> lock(impl_->write_mutex);
    if (impl_->in_write >= 0) close(impl_->in_write);
    impl_->in_write = -1;
  }
  if (impl_->out_read >= 0) close(impl_->out_read);
  if (impl_->err_read >= 0) close(impl_->err_read);
  impl_->out_read = impl_->err_read = -1;
  impl_->pid = -1;
  impl_->exited = false;
}

std::uint64_t ChildProcess::Pid() const { return impl_->pid > 0 ? static_cast<std::uint64_t>(impl_->pid) : 0; }

ProcessResources ChildProcess::Resources() const { return {}; }

#endif

}  // namespace aafr::model
