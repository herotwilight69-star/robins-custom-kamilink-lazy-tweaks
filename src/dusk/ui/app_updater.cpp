#include "app_updater.hpp"

#include "dusk/hash.hpp"

#include <borealis/http.hpp>
#include <borealis/io.hpp>
#include <borealis/log.hpp>
#include <fmt/format.h>
#include <miniz.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#define WINDOWS_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace dusk::ui::app_update {
namespace {

constexpr borealis::Log AppUpdateLog{"dusk::ui::app_update"};
constexpr uint64_t MaxArchiveEntrySize = 512ull * 1024 * 1024;
constexpr uint64_t MaxArchiveSize = 2ull * 1024 * 1024 * 1024;

struct Runtime {
    std::atomic<State> state{State::Idle};
    std::atomic<bool> workerDone{false};
    std::mutex messageMutex;
    std::string message;
    borealis::Task<borealis::http::Result> download;
    std::thread worker;
    std::filesystem::path workDir;
    std::filesystem::path archivePath;
    std::string expectedDigest;

    ~Runtime() {
        if (worker.joinable()) {
            worker.join();
        }
    }
};

Runtime& runtime() {
    static Runtime value;
    return value;
}

void set_status(State state, std::string message) {
    auto& value = runtime();
    {
        std::lock_guard lock{value.messageMutex};
        value.message = std::move(message);
    }
    value.state.store(state, std::memory_order_release);
}

std::string lowercase(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool valid_digest(std::string_view digest) {
    constexpr std::string_view prefix = "sha256:";
    if (!digest.starts_with(prefix) || digest.size() != prefix.size() + 64) {
        return false;
    }
    return std::ranges::all_of(digest.substr(prefix.size()), [](unsigned char character) {
        return std::isxdigit(character) != 0;
    });
}

std::string sha256_file(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        throw std::runtime_error("Could not open the downloaded update package");
    }

    dusk::hash::Sha256 hash;
    std::array<uint8_t, 64 * 1024> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        const auto count = input.gcount();
        if (count > 0) {
            hash.update(std::span{buffer.data(), static_cast<size_t>(count)});
        }
    }
    if (!input.eof()) {
        throw std::runtime_error("Could not read the downloaded update package");
    }
    return hash.finish();
}

bool safe_archive_path(std::string_view name, std::filesystem::path& relative) {
    if (name.empty() || name.front() == '/' || name.front() == '\\' ||
        name.find('\\') != std::string_view::npos || name.find(':') != std::string_view::npos ||
        name.find('\0') != std::string_view::npos)
    {
        return false;
    }

    relative = std::filesystem::u8path(name);
    if (relative.is_absolute() || relative.has_root_name()) {
        return false;
    }
    for (const auto& component : relative) {
        if (component == ".." || component == ".") {
            return false;
        }
    }
    return !relative.empty();
}

struct ExtractContext {
    std::ofstream* output = nullptr;
};

struct ArchiveInput {
    borealis::io::RandomAccessFile file;

    static size_t read(void* opaque, mz_uint64 offset, void* buffer, size_t size) {
        auto& input = *static_cast<ArchiveInput*>(opaque);
        std::error_code error;
        const size_t count = input.file.read_at(
            offset, {static_cast<std::byte*>(buffer), size}, error);
        return error ? 0 : count;
    }
};

size_t write_extract_chunk(void* opaque, mz_uint64, const void* bytes, size_t size) {
    auto& context = *static_cast<ExtractContext*>(opaque);
    context.output->write(static_cast<const char*>(bytes), static_cast<std::streamsize>(size));
    return *context.output ? size : 0;
}

void extract_archive(
    const std::filesystem::path& archivePath, const std::filesystem::path& payloadPath) {
    auto opened = borealis::io::RandomAccessFile::open(archivePath);
    if (opened.status != borealis::io::Status::Ok) {
        throw std::runtime_error(opened.message);
    }
    ArchiveInput input{std::move(opened.file)};

    mz_zip_archive archive{};
    archive.m_pRead = ArchiveInput::read;
    archive.m_pIO_opaque = &input;
    if (!mz_zip_reader_init(&archive, input.file.size(), 0)) {
        throw std::runtime_error("The downloaded update is not a readable ZIP package");
    }
    const auto closeArchive = [](mz_zip_archive* archiveToClose) {
        mz_zip_reader_end(archiveToClose);
    };
    const std::unique_ptr<mz_zip_archive, decltype(closeArchive)> archiveGuard{
        &archive, closeArchive};

    const mz_uint count = mz_zip_reader_get_num_files(&archive);
    uint64_t totalUncompressed = 0;
    bool hasExecutable = false;
    for (mz_uint index = 0; index < count; ++index) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&archive, index, &stat)) {
            throw std::runtime_error("Could not inspect an entry in the update package");
        }
        if (mz_zip_reader_is_file_a_directory(&archive, index)) {
            continue;
        }
        if (stat.m_uncomp_size > MaxArchiveEntrySize ||
            totalUncompressed > MaxArchiveSize - stat.m_uncomp_size)
        {
            throw std::runtime_error("The update package exceeds the supported size limit");
        }
        totalUncompressed += stat.m_uncomp_size;

        std::filesystem::path relative;
        if (!safe_archive_path(stat.m_filename, relative)) {
            throw std::runtime_error("The update package contains an unsafe file path");
        }
        auto component = relative.begin();
        if (component != relative.end() && lowercase(component->string()) == "data") {
            continue;
        }
        if (relative.parent_path().empty() &&
            lowercase(relative.filename().string()) == "dusklight.exe")
        {
            hasExecutable = true;
        }

        const auto outputPath = payloadPath / relative;
        std::error_code error;
        std::filesystem::create_directories(outputPath.parent_path(), error);
        if (error) {
            throw std::runtime_error(
                fmt::format("Could not create update directory: {}", error.message()));
        }

        std::ofstream output{outputPath, std::ios::binary | std::ios::trunc};
        if (!output) {
            throw std::runtime_error("Could not write an extracted update file");
        }
        ExtractContext context{.output = &output};
        if (!mz_zip_reader_extract_to_callback(
                &archive, index, write_extract_chunk, &context, 0))
        {
            throw std::runtime_error("Could not extract an update package file");
        }
        output.close();
        if (!output) {
            throw std::runtime_error("Could not finish writing an update package file");
        }
    }
    if (!hasExecutable) {
        throw std::runtime_error("The update package does not contain Dusklight.exe");
    }
}

#if defined(_WIN32)
std::string utf8_path(const std::filesystem::path& path) {
    return borealis::io::fs_path_to_string(path);
}

std::string escape_powershell(std::string value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value) {
        escaped.push_back(character);
        if (character == '\'') {
            escaped.push_back('\'');
        }
    }
    return escaped;
}

std::filesystem::path current_executable_path() {
    std::wstring buffer(32768, L'\0');
    const DWORD length =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        throw std::runtime_error(fmt::format(
            "Could not resolve the running executable (Windows error {})", GetLastError()));
    }
    buffer.resize(length);
    return buffer;
}

void launch_installer(const std::filesystem::path& payloadPath,
    const std::filesystem::path& workDir, const std::filesystem::path& executablePath) {
    const auto installDir = executablePath.parent_path();
    const auto scriptPath = workDir / L"install-update.ps1";
    const std::string script =
        "\xEF\xBB\xBF$ErrorActionPreference = 'Stop'\n"
        "$processId = " + std::to_string(GetCurrentProcessId()) + "\n"
        "$stage = '" + escape_powershell(utf8_path(payloadPath)) + "'\n"
        "$install = '" + escape_powershell(utf8_path(installDir)) + "'\n"
        "try {\n"
        "  while (Get-Process -Id $processId -ErrorAction SilentlyContinue) { Start-Sleep -Milliseconds 250 }\n"
        "  Get-ChildItem -LiteralPath $stage -Force -Recurse | ForEach-Object {\n"
        "    $relative = $_.FullName.Substring($stage.Length).TrimStart([char[]]@('\\', '/'))\n"
        "    $destination = Join-Path $install $relative\n"
        "    if ($_.PSIsContainer) { New-Item -ItemType Directory -Path $destination -Force | Out-Null }\n"
        "    else {\n"
        "      $parent = Split-Path -Parent $destination\n"
        "      New-Item -ItemType Directory -Path $parent -Force | Out-Null\n"
        "      Copy-Item -LiteralPath $_.FullName -Destination $destination -Force\n"
        "    }\n"
        "  }\n"
        "  Start-Process -FilePath (Join-Path $install 'dusklight.exe')\n"
        "  Remove-Item -LiteralPath $stage -Recurse -Force\n"
        "  Remove-Item -LiteralPath (Join-Path (Split-Path -Parent $stage) 'update.zip') -Force\n"
        "} catch {\n"
        "  Add-Type -AssemblyName PresentationFramework\n"
        "  [System.Windows.MessageBox]::Show(('Dusklight could not finish installing the update: ' + $_.Exception.Message), 'Dusklight update failed') | Out-Null\n"
        "  exit 1\n"
        "}\n";

    std::ofstream scriptFile{scriptPath, std::ios::binary | std::ios::trunc};
    if (!scriptFile) {
        throw std::runtime_error("Could not create the update installer script");
    }
    scriptFile.write(script.data(), static_cast<std::streamsize>(script.size()));
    scriptFile.close();
    if (!scriptFile) {
        throw std::runtime_error("Could not write the update installer script");
    }

    std::wstring commandLine =
        L"powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"";
    commandLine += scriptPath.wstring();
    commandLine += L"\"";
    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startupInfo, &processInfo))
    {
        throw std::runtime_error(fmt::format(
            "Could not start the update installer (Windows error {})", GetLastError()));
    }
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
}
#endif

void prepare_update() {
#if defined(_WIN32)
    auto& value = runtime();
    if (!valid_digest(value.expectedDigest)) {
        throw std::runtime_error("GitHub did not provide a valid SHA-256 digest for this package");
    }
    const std::string expected =
        lowercase(value.expectedDigest.substr(std::string_view{"sha256:"}.size()));
    if (sha256_file(value.archivePath) != expected) {
        throw std::runtime_error("The update package failed SHA-256 verification");
    }

    const auto payloadPath = value.workDir / L"payload";
    std::filesystem::create_directories(payloadPath);
    extract_archive(value.archivePath, payloadPath);
    launch_installer(payloadPath, value.workDir, current_executable_path());
#else
    throw std::runtime_error("Automatic updates are only supported on Windows builds");
#endif
}

}  // namespace

bool begin(std::string_view url, std::string_view digest, std::string& error) {
#if defined(_WIN32)
    auto& value = runtime();
    const State currentState = value.state.load(std::memory_order_acquire);
    if (currentState == State::Downloading || currentState == State::Preparing ||
        currentState == State::Ready)
    {
        error = "An update is already in progress.";
        return false;
    }
    if (!url.starts_with("https://github.com/") || !valid_digest(digest)) {
        error = "The update release is missing a secure download URL or SHA-256 digest.";
        set_status(State::Failed, error);
        return false;
    }
    if (value.worker.joinable()) {
        value.worker.join();
    }

    try {
        const auto tempDir = std::filesystem::temp_directory_path();
        value.workDir =
            tempDir / fmt::format("DusklightUpdate-{}-{}", GetCurrentProcessId(), GetTickCount64());
        std::filesystem::create_directories(value.workDir);
        value.archivePath = value.workDir / L"update.zip";
        value.expectedDigest = std::string{digest};
        value.workerDone.store(false, std::memory_order_release);
        value.download = borealis::http::start({
            .url = std::string{url},
            .downloadTo = value.archivePath,
            .connectTimeout = std::chrono::seconds{30},
            .idleTimeout = std::chrono::seconds{90},
            .totalTimeout = std::chrono::minutes{30},
        });
        set_status(State::Downloading, "Downloading the custom Dusklight update...");
        return true;
    } catch (const std::exception& exception) {
        error = fmt::format("Could not start the update download: {}", exception.what());
    } catch (...) {
        error = "Could not start the update download because of an unknown error.";
    }
    set_status(State::Failed, error);
    return false;
#else
    (void)url;
    (void)digest;
    error = "Automatic updates are only supported on Windows builds.";
    set_status(State::Failed, error);
    return false;
#endif
}

void update() {
#if defined(_WIN32)
    auto& value = runtime();
    if (value.state.load(std::memory_order_acquire) == State::Preparing &&
        value.workerDone.load(std::memory_order_acquire) && value.worker.joinable())
    {
        value.worker.join();
    }
    if (!value.download || !value.download.ready()) {
        return;
    }

    try {
        const auto result = value.download.try_take();
        value.download = {};
        if (!result.has_value() || result->error != borealis::http::Error::None ||
            result->response.statusCode != 200)
        {
            const std::string detail = result.has_value() && !result->message.empty() ?
                                           result->message :
                                           "The download did not return HTTP 200.";
            set_status(State::Failed, fmt::format("Update download failed: {}", detail));
            AppUpdateLog.error("Update download failed: {}", detail);
            return;
        }

        set_status(State::Preparing, "Verifying and preparing the update...");
        value.worker = std::thread([&value] {
            try {
                prepare_update();
                set_status(State::Ready,
                    "Update staged. Close Dusklight to install it; the installer will reopen the app.");
            } catch (const std::exception& exception) {
                set_status(State::Failed,
                    fmt::format("Could not prepare the update: {}", exception.what()));
                AppUpdateLog.error("Could not prepare the update: {}", exception.what());
            } catch (...) {
                set_status(State::Failed, "Could not prepare the update because of an unknown error.");
                AppUpdateLog.error("Could not prepare the update because of an unknown error");
            }
            value.workerDone.store(true, std::memory_order_release);
        });
    } catch (const std::exception& exception) {
        value.download = {};
        set_status(State::Failed, fmt::format("Update download failed: {}", exception.what()));
        AppUpdateLog.error("Update download failed: {}", exception.what());
    } catch (...) {
        value.download = {};
        set_status(State::Failed, "Update download failed because of an unknown error.");
        AppUpdateLog.error("Update download failed because of an unknown error");
    }
#endif
}

void show_error(std::string message) {
    AppUpdateLog.error("{}", message);
    set_status(State::Failed, std::move(message));
}

State state() noexcept {
    return runtime().state.load(std::memory_order_acquire);
}

std::string message() {
    auto& value = runtime();
    std::lock_guard lock{value.messageMutex};
    return value.message;
}

}  // namespace dusk::ui::app_update
