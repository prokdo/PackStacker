#include "testing/process.hpp"

#include "core/filesystem.hpp"
#include "testing/fixtures.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iterator>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <algorithm>
#include <cstddef>
#include <memory>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#endif

namespace PackStacker::Testing
{
    namespace
    {
#ifdef _WIN32
        struct CloseHandle
        {
            void operator()(void* handle) const
            {
                if (::CloseHandle(handle) == 0)
                {
                    std::terminate();
                }
            }
        };
        using Handle = std::unique_ptr<void, CloseHandle>;

        Handle outputHandle(const std::filesystem::path& path)
        {
            auto security = SECURITY_ATTRIBUTES{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
            auto* handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
                                       CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE)
            {
                throw std::runtime_error("Cannot create process output");
            }
            return Handle(handle);
        }

        std::wstring quoted(std::wstring_view argument)
        {
            auto result = std::wstring{L"\""};
            auto slashes = std::size_t{0};
            for (const auto character : argument)
            {
                if (character == L'\\')
                {
                    ++slashes;
                    continue;
                }
                result.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
                slashes = 0;
                result.push_back(character);
            }
            result.append(slashes * 2, L'\\');
            result.push_back(L'"');
            return result;
        }

        struct FreeEnvironment
        {
            void operator()(wchar_t* block) const
            {
                if (FreeEnvironmentStringsW(block) == 0)
                {
                    std::terminate();
                }
            }
        };

        bool environmentName(std::wstring_view value, std::wstring_view name)
        {
            const auto key = value.substr(0, value.find(L'='));
            return std::ranges::equal(key, name, {},
                                      [](wchar_t character)
                                      {
                                          return character >= L'a' && character <= L'z'
                                                     ? static_cast<wchar_t>(character - L'a' + L'A')
                                                     : character;
                                      });
        }

        std::wstring childEnvironment()
        {
            const auto block = std::unique_ptr<wchar_t, FreeEnvironment>(GetEnvironmentStringsW());
            if (!block)
            {
                throw std::runtime_error("Cannot read process environment");
            }
            auto entries = std::vector<std::wstring>{};
            const auto* entry = block.get();
            while (*entry != L'\0')
            {
                const auto value = std::wstring_view(entry);
                if (!environmentName(value, L"DISPLAY") &&
                    !environmentName(value, L"WAYLAND_DISPLAY") &&
                    !environmentName(value, L"QT_QPA_PLATFORM"))
                {
                    entries.emplace_back(value);
                }
                entry = std::next(entry, static_cast<std::ptrdiff_t>(value.size() + 1));
            }
            entries.emplace_back(L"QT_QPA_PLATFORM=packstacker-test-no-platform");
            std::ranges::sort(entries,
                              [](const auto& first, const auto& second)
                              {
                                  const auto order = CompareStringOrdinal(first.c_str(), -1,
                                                                          second.c_str(), -1, TRUE);
                                  if (order == 0)
                                  {
                                      throw std::runtime_error("Cannot sort process environment");
                                  }
                                  return order == CSTR_LESS_THAN;
                              });
            auto result = std::wstring{};
            for (const auto& value : entries)
            {
                result.append(value);
                result.push_back(L'\0');
            }
            result.push_back(L'\0');
            return result;
        }

        int32_t execute(std::string_view executable, const std::vector<std::string>& arguments,
                        const std::filesystem::path& directory, const std::filesystem::path& output,
                        const std::filesystem::path& error)
        {
            const auto application = std::filesystem::absolute(PathFromUtf8(executable)).wstring();
            auto command = quoted(application);
            for (const auto& argument : arguments)
            {
                command += L' ' + quoted(PathFromUtf8(argument).native());
            }
            auto standardOutput = outputHandle(output);
            auto standardError = outputHandle(error);
            auto startup = STARTUPINFOW{};
            startup.cb = sizeof(startup);
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdOutput = standardOutput.get();
            startup.hStdError = standardError.get();
            auto security = SECURITY_ATTRIBUTES{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
            auto* input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (input == INVALID_HANDLE_VALUE)
            {
                throw std::runtime_error("Cannot open process input");
            }
            auto standardInput = Handle(input);
            startup.hStdInput = standardInput.get();
            auto information = PROCESS_INFORMATION{};
            auto environment = childEnvironment();
            if (CreateProcessW(application.c_str(), command.data(), nullptr, nullptr, TRUE,
                               CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, environment.data(),
                               directory.c_str(), &startup, &information) == 0)
            {
                throw std::runtime_error("CLI did not start");
            }
            auto process = Handle(information.hProcess);
            const auto thread = Handle(information.hThread);
            const auto waited = WaitForSingleObject(process.get(), 20000);
            if (waited != WAIT_OBJECT_0)
            {
                if (TerminateProcess(process.get(), 1) == 0 ||
                    WaitForSingleObject(process.get(), INFINITE) != WAIT_OBJECT_0)
                {
                    throw std::runtime_error("Cannot stop timed out CLI");
                }
                throw std::runtime_error("CLI timed out or wait failed");
            }
            auto code = DWORD{};
            if (GetExitCodeProcess(process.get(), &code) == 0)
            {
                throw std::runtime_error("Cannot read CLI exit code");
            }
            return static_cast<int32_t>(code);
        }
#else
        class Descriptor
        {
        public:
            explicit Descriptor(const std::filesystem::path& path)
                : value(::creat(path.c_str(), 0600))
            {
                if (value < 0)
                {
                    throw std::runtime_error("Cannot create process output");
                }
            }
            Descriptor(const Descriptor&) = delete;
            Descriptor& operator=(const Descriptor&) = delete;
            Descriptor(Descriptor&&) = delete;
            Descriptor& operator=(Descriptor&&) = delete;
            ~Descriptor()
            {
                if (::close(value) != 0)
                {
                    std::terminate();
                }
            }
            int32_t Get() const
            {
                return value;
            }

        private:
            int32_t value;
        };

        std::vector<std::string> childEnvironment()
        {
            auto result = std::vector<std::string>{};
            const auto entries =
                std::ranges::subrange(environ, std::unreachable_sentinel) |
                std::views::take_while([](const char* entry) { return entry != nullptr; });
            for (const auto* entry : entries)
            {
                const auto value = std::string_view(entry);
                if (!value.starts_with("DISPLAY=") && !value.starts_with("WAYLAND_DISPLAY=") &&
                    !value.starts_with("QT_QPA_PLATFORM="))
                {
                    result.emplace_back(value);
                }
            }
            result.emplace_back("QT_QPA_PLATFORM=packstacker-test-no-platform");
            return result;
        }

        int32_t waitForChild(pid_t child)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
            auto status = int{};
            for (;;)
            {
                const auto waited = ::waitpid(child, &status, WNOHANG);
                if (waited == child)
                {
                    break;
                }
                if (waited < 0 && errno != EINTR)
                {
                    throw std::runtime_error("Cannot wait for CLI");
                }
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    if (::kill(child, SIGKILL) != 0)
                    {
                        throw std::runtime_error("Cannot stop timed out CLI");
                    }
                    while (::waitpid(child, &status, 0) < 0)
                    {
                        if (errno != EINTR)
                        {
                            throw std::runtime_error("Cannot reap timed out CLI");
                        }
                    }
                    throw std::runtime_error("CLI timed out");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
            }
            if (!WIFEXITED(status))
            {
                throw std::runtime_error("CLI crashed");
            }
            return WEXITSTATUS(status);
        }

        int32_t execute(std::string_view executable, const std::vector<std::string>& arguments,
                        const std::filesystem::path& directory, const std::filesystem::path& output,
                        const std::filesystem::path& error)
        {
            const auto application = std::filesystem::absolute(PathFromUtf8(executable)).string();
            auto values = std::vector<std::string>{application};
            values.insert(values.end(), arguments.begin(), arguments.end());
            auto pointers = std::vector<char*>{};
            for (auto& value : values)
            {
                pointers.push_back(value.data());
            }
            pointers.push_back(nullptr);
            auto environment = childEnvironment();
            auto environmentPointers = std::vector<char*>{};
            for (auto& value : environment)
            {
                environmentPointers.push_back(value.data());
            }
            environmentPointers.push_back(nullptr);
            const auto standardOutput = Descriptor(output);
            const auto standardError = Descriptor(error);
            const auto child = ::fork();
            if (child < 0)
            {
                throw std::runtime_error("CLI did not start");
            }
            if (child == 0)
            {
                if (::chdir(directory.c_str()) != 0 ||
                    ::dup2(standardOutput.Get(), STDOUT_FILENO) < 0 ||
                    ::dup2(standardError.Get(), STDERR_FILENO) < 0 ||
                    ::close(standardOutput.Get()) != 0 || ::close(standardError.Get()) != 0)
                {
                    ::_exit(126);
                }
                ::execve(application.c_str(), pointers.data(), environmentPointers.data());
                ::_exit(127);
            }
            return waitForChild(child);
        }
#endif
    }

    Invocation Run(std::string_view executable, const std::vector<std::string>& arguments,
                   std::string_view directory)
    {
        const auto temporary = TemporaryDirectory();
        const auto output = temporary.Path() / "stdout";
        const auto error = temporary.Path() / "stderr";
        const auto code = execute(executable, arguments, PathFromUtf8(directory), output, error);
        return {
            .Code = code,
            .Output = Read(PathToUtf8(output)),
            .Error = Read(PathToUtf8(error)),
        };
    }
}
