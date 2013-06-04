// Copyright (c) 2013, Kenton Varda <temporal@gmail.com>
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// This file declares convenient macros for debug logging and error handling.  The macros make
// it excessively easy to extract useful context information from code.  Example:
//
//     KJ_ASSERT(a == b, a, b, "a and b must be the same.");
//
// On failure, this will throw an exception whose description looks like:
//
//     myfile.c++:43: bug in code: expected a == b; a = 14; b = 72; a and b must be the same.
//
// As you can see, all arguments after the first provide additional context.
//
// The macros available are:
//
// * `KJ_LOG(severity, ...)`:  Just writes a log message, to stderr by default (but you can
//   intercept messages by implementing an ExceptionCallback).  `severity` is `INFO`, `WARNING`,
//   `ERROR`, or `FATAL`.  By default, `INFO` logs are not written, but for command-line apps the
//   user should be able to pass a flag like `--verbose` to enable them.  Other log levels are
//   enabled by default.  Log messages -- like exceptions -- can be intercepted by registering an
//   ExceptionCallback.
//
// * `KJ_DBG(...)`:  Like `KJ_LOG`, but intended specifically for temporary log lines added while
//   debugging a particular problem.  Calls to `KJ_DBG` should always be deleted before committing
//   code.  It is suggested that you set up a pre-commit hook that checks for this.
//
// * `KJ_ASSERT(condition, ...)`:  Throws an exception if `condition` is false, or aborts if
//   exceptions are disabled.  This macro should be used to check for bugs in the surrounding code
//   and its dependencies, but NOT to check for invalid input.  The macro may be followed by a
//   brace-delimited code block; if so, the block will be executed in the case where the assertion
//   fails, before throwing the exception.  If control jumps out of the block (e.g. with "break",
//   "return", or "goto"), then the error is considered "recoverable" -- in this case, if
//   exceptions are disabled, execution will continue normally rather than aborting (but if
//   exceptions are enabled, an exception will still be thrown on exiting the block). A "break"
//   statement in particular will jump to the code immediately after the block (it does not break
//   any surrounding loop or switch).  Example:
//
//       KJ_ASSERT(value >= 0, "Value cannot be negative.", value) {
//         // Assertion failed.  Set value to zero to "recover".
//         value = 0;
//         // Don't abort if exceptions are disabled.  Continue normally.
//         // (Still throw an exception if they are enabled, though.)
//         break;
//       }
//       // When exceptions are disabled, we'll get here even if the assertion fails.
//       // Otherwise, we get here only if the assertion passes.
//
// * `KJ_REQUIRE(condition, ...)`:  Like `KJ_ASSERT` but used to check preconditions -- e.g. to
//   validate parameters passed from a caller.  A failure indicates that the caller is buggy.
//
// * `KJ_SYSCALL(code, ...)`:  Executes `code` assuming it makes a system call.  A negative result
//   is considered an error, with error code reported via `errno`.  EINTR is handled by retrying.
//   Other errors are handled by throwing an exception.  If you need to examine the return code,
//   assign it to a variable like so:
//
//       int fd;
//       KJ_SYSCALL(fd = open(filename, O_RDONLY), filename);
//
//   `KJ_SYSCALL` can be followed by a recovery block, just like `KJ_ASSERT`.
//
// * `KJ_CONTEXT(...)`:  Notes additional contextual information relevant to any exceptions thrown
//   from within the current scope.  That is, until control exits the block in which KJ_CONTEXT()
//   is used, if any exception is generated, it will contain the given information in its context
//   chain.  This is helpful because it can otherwise be very difficult to come up with error
//   messages that make sense within low-level helper code.  Note that the parameters to
//   KJ_CONTEXT() are only evaluated if an exception is thrown.  This implies that any variables
//   used must remain valid until the end of the scope.
//
// Notes:
// * Do not write expressions with side-effects in the message content part of the macro, as the
//   message will not necessarily be evaluated.
// * For every macro `FOO` above except `LOG`, there is also a `FAIL_FOO` macro used to report
//   failures that already happened.  For the macros that check a boolean condition, `FAIL_FOO`
//   omits the first parameter and behaves like it was `false`.  `FAIL_SYSCALL` and
//   `FAIL_RECOVERABLE_SYSCALL` take a string and an OS error number as the first two parameters.
//   The string should be the name of the failed system call.
// * For every macro `FOO` above, there is a `DFOO` version (or `RECOVERABLE_DFOO`) which is only
//   executed in debug mode.  When `NDEBUG` is defined, these macros expand to nothing.

#ifndef KJ_DEBUG_H_
#define KJ_DEBUG_H_

#include "string.h"
#include "exception.h"

namespace kj {

class Log {
  // Mostly-internal

public:
  enum class Severity {
    INFO,      // Information describing what the code is up to, which users may request to see
               // with a flag like `--verbose`.  Does not indicate a problem.  Not printed by
               // default; you must call setLogLevel(INFO) to enable.
    WARNING,   // A problem was detected but execution can continue with correct output.
    ERROR,     // Something is wrong, but execution can continue with garbage output.
    FATAL,     // Something went wrong, and execution cannot continue.
    DEBUG      // Temporary debug logging.  See KJ_DBG.

    // Make sure to update the stringifier if you add a new severity level.
  };

  static inline bool shouldLog(Severity severity) { return severity >= minSeverity; }
  // Returns whether messages of the given severity should be logged.

  static inline void setLogLevel(Severity severity) { minSeverity = severity; }
  // Set the minimum message severity which will be logged.

  template <typename... Params>
  static void log(const char* file, int line, Severity severity, const char* macroArgs,
                  Params&&... params);

  class Fault {
  public:
    template <typename... Params>
    Fault(const char* file, int line, Exception::Nature nature, int errorNumber,
          const char* condition, const char* macroArgs, Params&&... params);
    ~Fault() noexcept(false);

    void fatal() KJ_NORETURN;
    // Throw the exception.

  private:
    void init(const char* file, int line, Exception::Nature nature, int errorNumber,
              const char* condition, const char* macroArgs, ArrayPtr<String> argValues);

    Exception* exception;
  };

  class SyscallResult {
  public:
    inline SyscallResult(int errorNumber): errorNumber(errorNumber) {}
    inline operator void*() { return errorNumber == 0 ? this : nullptr; }
    inline int getErrorNumber() { return errorNumber; }

  private:
    int errorNumber;
  };

  template <typename Call>
  static SyscallResult syscall(Call&& call);

  class Context: public ExceptionCallback {
  public:
    Context();
    KJ_DISALLOW_COPY(Context);
    virtual ~Context();
    virtual void addTo(Exception& exception) = 0;

    virtual void onRecoverableException(Exception&& exception) override;
    virtual void onFatalException(Exception&& exception) override;
    virtual void logMessage(StringPtr text) override;

  private:
    ExceptionCallback& next;
    ScopedRegistration registration;
  };

  template <typename Func>
  class ContextImpl: public Context {
  public:
    inline ContextImpl(Func& func): func(func) {}
    KJ_DISALLOW_COPY(ContextImpl);

    void addTo(Exception& exception) override {
      func(exception);
    }
  private:
    Func& func;
  };

  template <typename... Params>
  static void addContextTo(Exception& exception, const char* file,
                           int line, const char* macroArgs, Params&&... params);

private:
  static Severity minSeverity;

  static void logInternal(const char* file, int line, Severity severity, const char* macroArgs,
                          ArrayPtr<String> argValues);
  static void addContextToInternal(Exception& exception, const char* file, int line,
                                   const char* macroArgs, ArrayPtr<String> argValues);

  static int getOsErrorNumber();
  // Get the error code of the last error (e.g. from errno).  Returns -1 on EINTR.
};

ArrayPtr<const char> KJ_STRINGIFY(Log::Severity severity);

#define KJ_LOG(severity, ...) \
  if (!::kj::Log::shouldLog(::kj::Log::Severity::severity)) {} else \
    ::kj::Log::log(__FILE__, __LINE__, ::kj::Log::Severity::severity, \
                          #__VA_ARGS__, __VA_ARGS__)

#define KJ_DBG(...) KJ_LOG(DEBUG, ##__VA_ARGS__)

#define _kJ_FAULT(nature, cond, ...) \
  if (KJ_EXPECT_TRUE(cond)) {} else \
    for (::kj::Log::Fault f(__FILE__, __LINE__, ::kj::Exception::Nature::nature, 0, \
                            #cond, #__VA_ARGS__, ##__VA_ARGS__);; f.fatal())

#define _kJ_FAIL_FAULT(nature, ...) \
  for (::kj::Log::Fault f(__FILE__, __LINE__, ::kj::Exception::Nature::nature, 0, \
                          nullptr, #__VA_ARGS__, ##__VA_ARGS__);; f.fatal())

#define KJ_ASSERT(...) _kJ_FAULT(LOCAL_BUG, ##__VA_ARGS__)
#define KJ_REQUIRE(...) _kJ_FAULT(PRECONDITION, ##__VA_ARGS__)

#define KJ_FAIL_ASSERT(...) _kJ_FAIL_FAULT(LOCAL_BUG, ##__VA_ARGS__)
#define KJ_FAIL_REQUIRE(...) _kJ_FAIL_FAULT(PRECONDITION, ##__VA_ARGS__)

#define KJ_SYSCALL(call, ...) \
  if (auto _kjSyscallResult = ::kj::Log::syscall([&](){return (call);})) {} else \
    for (::kj::Log::Fault f( \
             __FILE__, __LINE__, ::kj::Exception::Nature::OS_ERROR, \
             _kjSyscallResult.getErrorNumber(), #call, #__VA_ARGS__, ##__VA_ARGS__);; f.fatal())

#define FAIL_SYSCALL(code, errorNumber, ...) \
  for (::kj::Log::Fault f( \
           __FILE__, __LINE__, ::kj::Exception::Nature::OS_ERROR, \
           errorNumber, code, #__VA_ARGS__, ##__VA_ARGS__);; f.fatal())

#define KJ_CONTEXT(...) \
  auto _kjContextFunc = [&](::kj::Exception& exception) { \
        return ::kj::Log::addContextTo(exception, \
            __FILE__, __LINE__, #__VA_ARGS__, ##__VA_ARGS__); \
      }; \
  ::kj::Log::ContextImpl<decltype(_kjContextFunc)> _kjContext(_kjContextFunc)

#ifdef NDEBUG
#define KJ_DLOG(...) do {} while (false)
#define KJ_DASSERT(...) do {} while (false)
#define KJ_DREQUIRE(...) do {} while (false)
#else
#define KJ_DLOG LOG
#define KJ_DASSERT KJ_ASSERT
#define KJ_DREQUIRE KJ_REQUIRE
#endif

template <typename... Params>
void Log::log(const char* file, int line, Severity severity, const char* macroArgs,
              Params&&... params) {
  String argValues[sizeof...(Params)] = {str(params)...};
  logInternal(file, line, severity, macroArgs, arrayPtr(argValues, sizeof...(Params)));
}

template <typename... Params>
Log::Fault::Fault(const char* file, int line, Exception::Nature nature, int errorNumber,
                  const char* condition, const char* macroArgs, Params&&... params)
    : exception(nullptr) {
  String argValues[sizeof...(Params)] = {str(params)...};
  init(file, line, nature, errorNumber, condition, macroArgs,
       arrayPtr(argValues, sizeof...(Params)));
}

template <typename Call>
Log::SyscallResult Log::syscall(Call&& call) {
  while (call() < 0) {
    int errorNum = getOsErrorNumber();
    // getOsErrorNumber() returns -1 to indicate EINTR
    if (errorNum != -1) {
      return SyscallResult(errorNum);
    }
  }
  return SyscallResult(0);
}

template <typename... Params>
void Log::addContextTo(Exception& exception, const char* file, int line,
                       const char* macroArgs, Params&&... params) {
  String argValues[sizeof...(Params)] = {str(params)...};
  addContextToInternal(exception, file, line, macroArgs, arrayPtr(argValues, sizeof...(Params)));
}

}  // namespace kj

#endif  // KJ_DEBUG_H_