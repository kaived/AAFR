#pragma once

// A child process with piped stdin, stdout and stderr. Plain C++17, no Qt.
//
// Used by the V0.7.2 model-worker client. The AAFR runtime is deliberately
// Qt-free and calls recommendation providers on its own worker thread, so the
// worker is driven through native pipes instead of QProcess (which is tied to
// the thread and event loop that created it).
//
// Windows: CreateProcessW with an explicit inherited-handle list (only the
// three pipe ends reach the child), CREATE_NO_WINDOW, and a Job Object that
// kills the worker when AAFR exits and caps its committed memory.
// POSIX: fork/execv with pipes and an address-space rlimit.
//
// Thread safety: Write*/Read* may be called from different threads (one
// writer, one reader per stream). Kill() unblocks readers by ending the
// process; Close() must only run after the readers have returned.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aafr::model {

struct ProcessSpec {
  std::string executable;  // Absolute path; no PATH search.
  std::vector<std::string> args;
  std::string working_directory;  // Empty: inherit.
  std::uint64_t memory_limit_bytes{0};  // 0: no limit.
};

struct ProcessResources {
  bool available{false};
  std::uint64_t working_set_bytes{0};
  std::uint64_t peak_working_set_bytes{0};
  std::uint64_t private_bytes{0};
  double cpu_user_ms{0.0};
  double cpu_kernel_ms{0.0};
};

class ChildProcess {
 public:
  ChildProcess();
  ~ChildProcess();
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  // False with a reason when the executable cannot be started.
  bool Start(const ProcessSpec& spec, std::string* error);
  bool Started() const;
  // Writes all bytes to the child's stdin; false when the pipe is closed.
  bool Write(const std::string& data);
  void CloseStdin();
  // Blocking reads; 0 at end of stream or on error.
  std::size_t ReadStdout(char* buffer, std::size_t size);
  std::size_t ReadStderr(char* buffer, std::size_t size);
  // Exit code once the process has ended, waiting at most timeout_ms.
  std::optional<int> Wait(int timeout_ms);
  bool Running() const;
  // Terminates the process (and its job on Windows).
  void Kill();
  // Releases every handle. Call after Kill()/Wait() and after readers returned.
  void Close();
  std::uint64_t Pid() const;
  ProcessResources Resources() const;

 private:
  struct Impl;
  Impl* impl_;
};

// Quotes one argument for a Windows command line (CommandLineToArgvW rules).
std::string QuoteWindowsArgument(const std::string& arg);

}  // namespace aafr::model
