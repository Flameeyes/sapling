/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This software may be used and distributed according to the terms of the
 * GNU General Public License version 2.
 */

/* The code in this test is a little hard to follow.
 * Here's a quick primer!
 *
 * The StartupLogger class encapsulates a channel between a parent
 * and child process pair that is used in the main project to allow
 * the parent to daemonize its child, but for the parent to linger
 * long enough to report the status of the child initialization.
 *
 * It works by spawning a new copy of itself and passing some command
 * line arguments to allow the child to realize that it is should
 * report back to its parent.
 *
 * This test verifies the behavior of the channel between the two
 * processes and thus needs to be able to spawn a copy of itself.
 *
 * Because we want the behavior of the spawned child to vary based
 * on the test we have a custom `main()` function that will look
 * at any command line arguments that spill over from the
 * gflags-registered arguments; the convention is that the first
 * argument names a function that will be called in a child process,
 * and that a second optional argument can be passed down to it.
 */

#include "eden/fs/service/StartupLogger.h"

#include <folly/Exception.h>
#include <folly/File.h>
#include <folly/FileUtil.h>
#include <folly/experimental/TestUtil.h>
#include <folly/init/Init.h>
#include <folly/logging/xlog.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <signal.h>
#include <sysexits.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include "eden/fs/utils/FileUtils.h"
#include "eden/fs/utils/SpawnedProcess.h"

using namespace facebook::eden;
using namespace std::chrono_literals;
using folly::StringPiece;
using folly::test::TemporaryFile;
using std::string;
using testing::ContainsRegex;
using testing::HasSubstr;
using testing::Not;
using namespace folly::string_piece_literals;

namespace facebook {
namespace eden {

namespace {

// Copy the original command line arguments for use with
// the StartupLogger::spawn method
std::vector<std::string> originalCommandLine;

struct FunctionResult {
  std::string standardOutput;
  std::string standardError;
  ProcessStatus returnCode;
};

FunctionResult runFunctionInSeparateProcess(folly::StringPiece functionName);
FunctionResult runFunctionInSeparateProcess(
    folly::StringPiece functionName,
    std::vector<std::string> arguments);
bool isReadablePipeBroken(FileDescriptor& fd);
bool isWritablePipeBroken(FileDescriptor& fd);
bool fileExists(folly::fs::path);

class StartupLoggerTestBase : public ::testing::Test {
 protected:
  AbsolutePath logPath() {
    return AbsolutePath(logFile_.path().string());
  }
  string readLogContents() {
    return readFile(logPath()).value();
  }

  TemporaryFile logFile_{"eden_test_log"};
};
} // namespace

class DaemonStartupLoggerTest : public StartupLoggerTestBase {
 protected:
  // Wrappers simply to allow our tests to access private DaemonStartupLogger
  // methods
  FileDescriptor createPipe(DaemonStartupLogger& logger) {
    Pipe pipe;
    logger.pipe_ = std::move(pipe.write);
    return std::move(pipe.read);
  }
  void closePipe(DaemonStartupLogger& logger) {
    logger.pipe_.close();
  }
  DaemonStartupLogger::ParentResult waitForChildStatus(
      DaemonStartupLogger& logger,
      FileDescriptor& readPipe,
      SpawnedProcess& childProc,
      StringPiece logPath) {
    return logger.waitForChildStatus(readPipe, childProc, logPath);
  }

  DaemonStartupLogger::ParentResult spawnInChild(folly::StringPiece name) {
    DaemonStartupLogger logger{};
    auto args = originalCommandLine;
    args.push_back(name.str());
    args.push_back(logPath().stringPiece().str());
    auto [proc, pipe] =
        logger.spawnImpl(logPath().stringPiece(), nullptr, args);
    auto result =
        logger.waitForChildStatus(pipe, proc, logPath().stringPiece());
    proc.kill();
    proc.wait();
    return result;
  }
};

namespace {
void crashWithNoResult(const std::string& logPath) {
  DaemonStartupLogger logger{};
  logger.initClient(
      logPath,
      FileDescriptor(FLAGS_startupLoggerFd, FileDescriptor::FDType::Pipe));
  fprintf(stderr, "this message should go to the log\n");
  fflush(stderr);
  kill(getpid(), SIGKILL);
  // Wait until we get killed.
  while (true) {
    /* sleep override */ std::this_thread::sleep_for(30s);
  }
}

TEST_F(DaemonStartupLoggerTest, crashWithNoResult) {
  auto result = spawnInChild("crashWithNoResult");

  EXPECT_EQ(EX_SOFTWARE, result.exitCode);
  EXPECT_EQ(
      folly::to<string>(
          "error: edenfs crashed with status killed by signal ",
          SIGKILL,
          " before it finished initializing\n"
          "Check the edenfs log file at ",
          logPath(),
          " for more details"),
      result.errorMessage);

  // Verify that the log message from the child went to the log file
  EXPECT_EQ("this message should go to the log\n", readLogContents());
}

TEST_F(DaemonStartupLoggerTest, successWritesStartedMessageToStandardError) {
  auto result = runFunctionInSeparateProcess(
      "successWritesStartedMessageToStandardErrorDaemonChild");
  EXPECT_THAT(
      result.standardError, ContainsRegex("Started edenfs \\(pid [0-9]+\\)"));
  EXPECT_THAT(result.standardError, HasSubstr("Logs available at "));
}

void successWritesStartedMessageToStandardErrorDaemonChild() {
  auto logFile = TemporaryFile{"eden_test_log"};
  auto logger = daemonizeIfRequested(
      logFile.path().string(), nullptr, originalCommandLine);
  logger->success();
  exit(0);
}

TEST_F(
    DaemonStartupLoggerTest,
    programExitsUnsuccessfullyIfLogFileIsInaccessible) {
  auto result = runFunctionInSeparateProcess(
      "programExitsUnsuccessfullyIfLogFileIsInaccessibleChild");
  EXPECT_THAT(
      result.standardError,
      ContainsRegex("error opening log file .*/file\\.txt"));
  EXPECT_THAT(result.standardError, HasSubstr("Not a directory"));
  EXPECT_EQ(
      folly::to<std::string>("exited with status ", EX_IOERR),
      result.returnCode.str());
}

void programExitsUnsuccessfullyIfLogFileIsInaccessibleChild() {
  auto logFile = TemporaryFile{"eden_test_log"};
  auto badLogFilePath = logFile.path() / "file.txt";
  auto logger = daemonizeIfRequested(
      badLogFilePath.string(), nullptr, originalCommandLine);
  logger->success();
  exit(0);
}

void exitWithNoResult(const std::string& logPath) {
  DaemonStartupLogger logger{};
  logger.initClient(
      logPath,
      FileDescriptor(FLAGS_startupLoggerFd, FileDescriptor::FDType::Pipe));
  _exit(19);
}

TEST_F(DaemonStartupLoggerTest, exitWithNoResult) {
  // Fork a child that exits unsuccessfully
  auto result = spawnInChild("exitWithNoResult");

  EXPECT_EQ(19, result.exitCode);
  EXPECT_EQ(
      folly::to<string>(
          "error: edenfs exited with status 19 before it finished initializing\n"
          "Check the edenfs log file at ",
          logPath(),
          " for more details"),
      result.errorMessage);
}

void exitSuccessfullyWithNoResult(const std::string& logPath) {
  DaemonStartupLogger logger{};
  logger.initClient(
      logPath,
      FileDescriptor(FLAGS_startupLoggerFd, FileDescriptor::FDType::Pipe));
  _exit(0);
}

TEST_F(DaemonStartupLoggerTest, exitSuccessfullyWithNoResult) {
  // Fork a child that exits successfully
  auto result = spawnInChild("exitSuccessfullyWithNoResult");

  // The parent process should be EX_SOFTWARE in this case
  EXPECT_EQ(EX_SOFTWARE, result.exitCode);
  EXPECT_EQ(
      folly::to<string>(
          "error: edenfs exited with status 0 before it finished initializing\n"
          "Check the edenfs log file at ",
          logPath(),
          " for more details"),
      result.errorMessage);
}

void destroyLoggerWhileDaemonIsStillRunning(const std::string& logPath) {
  DaemonStartupLogger logger{};
  logger.initClient(
      logPath,
      FileDescriptor(FLAGS_startupLoggerFd, FileDescriptor::FDType::Pipe));

  // Destroy the DaemonStartupLogger object to force it to close its pipes
  // without sending a result.
  std::optional<DaemonStartupLogger> optLogger(std::move(logger));
  optLogger.reset();

  /* sleep override */ std::this_thread::sleep_for(30s);
}

TEST_F(DaemonStartupLoggerTest, destroyLoggerWhileDaemonIsStillRunning) {
  auto result = spawnInChild("destroyLoggerWhileDaemonIsStillRunning");

  EXPECT_EQ(EX_SOFTWARE, result.exitCode);
  EXPECT_EQ(
      folly::to<std::string>(
          "error: edenfs is still running but "
          "did not report its initialization status\n"
          "Check the edenfs log file at ",
          logPath(),
          " for more details"),
      result.errorMessage);
}

TEST_F(DaemonStartupLoggerTest, closePipeWithWaitError) {
  // Call waitForChildStatus() with our own pid.
  // wait() will return an error trying to wait on ourself.
  DaemonStartupLogger logger;
  auto readPipe = createPipe(logger);
  closePipe(logger);
  auto selfProc = SpawnedProcess::fromExistingProcess(getpid());
  auto result =
      waitForChildStatus(logger, readPipe, selfProc, "/var/log/edenfs.log");

  EXPECT_EQ(EX_SOFTWARE, result.exitCode);
  EXPECT_EQ(
      "error: edenfs exited with status 0 before it finished initializing\n"
      "Check the edenfs log file at /var/log/edenfs.log for more details",
      result.errorMessage);
}

void success(const std::string& logPath) {
  DaemonStartupLogger logger{};
  logger.initClient(
      logPath,
      FileDescriptor(FLAGS_startupLoggerFd, FileDescriptor::FDType::Pipe));
  logger.success();
}

TEST_F(DaemonStartupLoggerTest, success) {
  auto result = spawnInChild("success");
  EXPECT_EQ(0, result.exitCode);
  EXPECT_EQ("", result.errorMessage);
}

void failure(const std::string& logPath) {
  DaemonStartupLogger logger{};
  logger.initClient(
      logPath,
      FileDescriptor(FLAGS_startupLoggerFd, FileDescriptor::FDType::Pipe));
  logger.exitUnsuccessfully(3, "example failure for tests");
}

TEST_F(DaemonStartupLoggerTest, failure) {
  auto result = spawnInChild("failure");
  EXPECT_EQ(3, result.exitCode);
  EXPECT_EQ("", result.errorMessage);
  EXPECT_THAT(readLogContents(), HasSubstr("example failure for tests"));
}

TEST_F(DaemonStartupLoggerTest, daemonClosesStandardFileDescriptors) {
  SpawnedProcess::Options opts;
  opts.pipeStdin();
  opts.pipeStdout();
  opts.pipeStderr();
  auto process = SpawnedProcess{{{
                                    executablePath().stringPiece().str(),
                                    "daemonClosesStandardFileDescriptorsChild",
                                }},
                                std::move(opts)};

  auto stdinFd = process.stdinFd();
  auto stdoutFd = process.stdoutFd();
  auto stderrFd = process.stderrFd();
  SCOPE_EXIT {
    process.wait();
  };
  stdinFd.setNonBlock();
  stdoutFd.setNonBlock();
  stderrFd.setNonBlock();

  // FIXME(strager): wait() could technically deadlock if the child is blocked
  // on writing to stdout or stderr.
  auto returnCode = process.waitTimeout(10s);
  EXPECT_EQ("exited with status 0", returnCode.str());

  auto expectReadablePipeIsBroken = [](FileDescriptor& fd,
                                       folly::StringPiece name) {
    EXPECT_TRUE(isReadablePipeBroken(fd))
        << "Daemon should have closed its " << name
        << " file descriptor (parent fd " << fd.systemHandle()
        << "), but it did not.";
  };
  auto expectWritablePipeIsBroken = [](FileDescriptor& fd,
                                       folly::StringPiece name) {
    EXPECT_TRUE(isWritablePipeBroken(fd))
        << "Daemon should have closed its " << name
        << " file descriptor (parent fd " << fd.systemHandle()
        << "), but it did not.";
  };

  expectWritablePipeIsBroken(stdinFd, "stdin");
  expectReadablePipeIsBroken(stdoutFd, "stdout");
  expectReadablePipeIsBroken(stderrFd, "stderr");

  // NOTE(strager): The daemon process should eventually exit automatically, so
  // we don't need to explicitly kill it.
}

void daemonClosesStandardFileDescriptorsChild() {
  auto logFile = TemporaryFile{"eden_test_log"};
  auto logger = daemonizeIfRequested(
      logFile.path().string(), nullptr, originalCommandLine);
  logger->success();
  std::this_thread::sleep_for(30s);
  exit(1);
}

TEST(ForegroundStartupLoggerTest, loggedMessagesAreWrittenToStandardError) {
  auto result = runFunctionInSeparateProcess(
      "loggedMessagesAreWrittenToStandardErrorChild");
  EXPECT_THAT(result.standardOutput, Not(HasSubstr("warn message")));
  EXPECT_THAT(result.standardError, HasSubstr("warn message"));
}

void loggedMessagesAreWrittenToStandardErrorChild() {
  auto logger = ForegroundStartupLogger{};
  logger.warn("warn message");
}

TEST(ForegroundStartupLoggerTest, exitUnsuccessfullyMakesProcessExitWithCode) {
  auto result = runFunctionInSeparateProcess(
      "exitUnsuccessfullyMakesProcessExitWithCodeChild");
  EXPECT_EQ("exited with status 42", result.returnCode.str());
}

void exitUnsuccessfullyMakesProcessExitWithCodeChild() {
  auto logger = ForegroundStartupLogger{};
  logger.exitUnsuccessfully(42, "intentionally exiting");
}

TEST(ForegroundStartupLoggerTest, xlogsAfterSuccessAreWrittenToStandardError) {
  auto result = runFunctionInSeparateProcess(
      "xlogsAfterSuccessAreWrittenToStandardErrorChild");
  EXPECT_THAT(result.standardError, HasSubstr("test error message with xlog"));
}

void xlogsAfterSuccessAreWrittenToStandardErrorChild() {
  auto logger = ForegroundStartupLogger{};
  logger.success();
  XLOG(ERR) << "test error message with xlog";
}

TEST(ForegroundStartupLoggerTest, successWritesStartedMessageToStandardError) {
  auto result = runFunctionInSeparateProcess(
      "successWritesStartedMessageToStandardErrorForegroundChild");
  EXPECT_THAT(
      result.standardError,
      ContainsRegex("Started edenfs \\(pid [0-9]+\\)\n$"));
}

void successWritesStartedMessageToStandardErrorForegroundChild() {
  auto logger = ForegroundStartupLogger{};
  logger.success();
}

class FileStartupLoggerTest : public StartupLoggerTestBase {};

TEST_F(FileStartupLoggerTest, loggerCreatesFileIfMissing) {
  auto tempDir = folly::test::TemporaryDirectory();
  auto logPath = tempDir.path() / "startup.log";
  ASSERT_FALSE(fileExists(logPath));
  auto logger = FileStartupLogger{logPath.string()};
  EXPECT_TRUE(fileExists(logPath));
}

TEST_F(FileStartupLoggerTest, loggingWritesMessagesToFile) {
  auto logger = FileStartupLogger{logPath().stringPiece()};
  logger.log("hello world");
  logger.warn("warning message");
  EXPECT_EQ("hello world\nwarning message\n", readLogContents());
}

TEST_F(FileStartupLoggerTest, loggingAppendsToFileIfItAlreadyExists) {
  writeFile(logPath(), "existing line\n"_sp).throwIfFailed();
  auto logger = FileStartupLogger{logPath().stringPiece()};
  logger.log("new line");
  EXPECT_EQ("existing line\nnew line\n", readLogContents());
}

TEST_F(FileStartupLoggerTest, successWritesMessageToFile) {
  auto logger = FileStartupLogger{logPath().stringPiece()};
  logger.success();
  EXPECT_EQ(
      folly::to<std::string>("Started edenfs (pid ", getpid(), ")\n"),
      readLogContents());
}

TEST_F(FileStartupLoggerTest, exitUnsuccessfullyWritesMessageAndKillsProcess) {
  auto result = runFunctionInSeparateProcess(
      "exitUnsuccessfullyWritesMessageAndKillsProcessChild",
      std::vector<std::string>{logPath().value()});
  EXPECT_EQ("exited with status 3", result.returnCode.str());
  EXPECT_EQ("error message\n", readLogContents());
}

void exitUnsuccessfullyWritesMessageAndKillsProcessChild(std::string logPath) {
  auto logger = FileStartupLogger{logPath};
  logger.exitUnsuccessfully(3, "error message");
}

FunctionResult runFunctionInSeparateProcess(folly::StringPiece functionName) {
  return runFunctionInSeparateProcess(functionName, std::vector<std::string>{});
}

FunctionResult runFunctionInSeparateProcess(
    folly::StringPiece functionName,
    std::vector<std::string> arguments) {
  auto execPath = executablePath();
  auto command = std::vector<std::string>{
      execPath.stringPiece().str(),
      functionName.str(),
  };
  command.insert(command.end(), arguments.begin(), arguments.end());

  SpawnedProcess::Options opts;
  opts.pipeStdout();
  opts.pipeStderr();
  auto process = SpawnedProcess{command, std::move(opts)};
  SCOPE_FAIL {
    process.stdinFd();
    process.wait();
  };
  auto [out, err] = process.communicate();
  auto returnCode = process.wait();
  return FunctionResult{out, err, returnCode};
}

// This function implements a basic lookup "table" that can call
// a function defined in this file.
[[noreturn]] void runFunctionInCurrentProcess(
    folly::StringPiece functionName,
    std::vector<std::string> arguments) {
  auto checkFunction = [&](folly::StringPiece name, auto function) {
    if (functionName == name) {
      if constexpr (std::is_invocable_v<decltype(function)>) {
        function();
      } else if constexpr (std::is_invocable_v<
                               decltype(function),
                               std::string>) {
        auto argument = std::string{arguments.at(0)};
        function(std::move(argument));
      } else {
        XLOG(FATAL) << "Unsupported function type";
      }
      std::exit(0);
    }
  };
  // CHECK_FUNCTION defines a lookup table entry
#define CHECK_FUNCTION(name) checkFunction(#name, name)
  CHECK_FUNCTION(daemonClosesStandardFileDescriptorsChild);
  CHECK_FUNCTION(exitUnsuccessfullyMakesProcessExitWithCodeChild);
  CHECK_FUNCTION(exitUnsuccessfullyWritesMessageAndKillsProcessChild);
  CHECK_FUNCTION(loggedMessagesAreWrittenToStandardErrorChild);
  CHECK_FUNCTION(programExitsUnsuccessfullyIfLogFileIsInaccessibleChild);
  CHECK_FUNCTION(successWritesStartedMessageToStandardErrorDaemonChild);
  CHECK_FUNCTION(successWritesStartedMessageToStandardErrorForegroundChild);
  CHECK_FUNCTION(xlogsAfterSuccessAreWrittenToStandardErrorChild);
  CHECK_FUNCTION(crashWithNoResult);
  CHECK_FUNCTION(exitWithNoResult);
  CHECK_FUNCTION(exitSuccessfullyWithNoResult);
  CHECK_FUNCTION(destroyLoggerWhileDaemonIsStillRunning);
  CHECK_FUNCTION(success);
  CHECK_FUNCTION(failure);
#undef CHECK_FUNCTION
  std::fprintf(
      stderr,
      "error: unknown function: %s\n",
      std::string{functionName}.c_str());
  std::exit(2);
}

bool isReadablePipeBroken(FileDescriptor& fd) {
  while (true) {
    char buffer[PIPE_BUF];
    auto result = fd.readNoInt(buffer, sizeof(buffer));
    result.throwIfFailed();
    if (result.value() == 0) {
      return true;
    }
  }
}

bool isWritablePipeBroken(FileDescriptor& fd) {
  const char buffer[1] = {0};
  auto result = fd.writeNoInt(buffer, sizeof(buffer));
  if (auto exc = result.tryGetExceptionObject<std::system_error>()) {
    return exc->code() == std::error_code(EPIPE, std::generic_category());
  }
  result.throwIfFailed();
  return false;
}

bool fileExists(folly::fs::path path) {
  auto status = folly::fs::status(path);
  return folly::fs::exists(status) && folly::fs::is_regular_file(status);
}
} // namespace
} // namespace eden
} // namespace facebook

int main(int argc, char** argv) {
  originalCommandLine = {argv, argv + argc};
  ::testing::InitGoogleTest(&argc, argv);
  auto removeFlags = true;
  auto initGuard = folly::Init(&argc, &argv, removeFlags);
  // If we arguments left over then they are (probably) generated by
  // calls to DaemonStartupLoggerTest::spawnInChild that need to
  // be mapped back to functions defined in this module.
  if (argc >= 2) {
    auto functionName = folly::StringPiece{argv[1]};
    auto arguments = std::vector<std::string>{&argv[2], &argv[argc]};
    runFunctionInCurrentProcess(functionName, std::move(arguments));
  }
  return RUN_ALL_TESTS();
}
