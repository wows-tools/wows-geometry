#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <filesystem>
#include <string>
#include <sys/stat.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <unistd.h>
#endif

static bool directory_exists(const std::filesystem::path &p) {
    struct stat st;
    return stat(p.u8string().c_str(), &st) == 0 && (st.st_mode & S_IFDIR) != 0;
}

static std::filesystem::path get_executable_dir() {
#if defined(_WIN32)
    std::wstring buffer;
    DWORD length = 0;
    do {
        buffer.resize(length == 0 ? MAX_PATH : length);
        length = GetModuleFileNameW(NULL, buffer.data(), static_cast<DWORD>(buffer.size()));
    } while (length != 0 && length == buffer.size());
    if (length == 0)
        return {};
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
#else
    char buffer[4096];
    ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length <= 0)
        return {};
    buffer[length] = '\0';
    return std::filesystem::path(buffer).parent_path();
#endif
}

static std::filesystem::path find_python_home() {
    std::filesystem::path exe_dir = get_executable_dir();
    if (exe_dir.empty())
        return {};

    const std::filesystem::path candidates[] = {
        exe_dir,
        exe_dir / "python3",
        exe_dir / "python",
        exe_dir / "python312",
    };

    for (auto const &candidate : candidates) {
        if (directory_exists(candidate / "Lib") && directory_exists(candidate / "DLLs"))
            return candidate;
    }
    return {};
}

void wows_setup_python_home() {
    if (Py_IsInitialized())
        return;

    std::filesystem::path home = find_python_home();
    if (home.empty())
        return;

    wchar_t *whome = Py_DecodeLocale(home.u8string().c_str(), nullptr);
    if (!whome)
        return;

    Py_SetPythonHome(whome);
    PyMem_RawFree(whome);
}
