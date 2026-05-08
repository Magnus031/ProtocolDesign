#include "src/client/plugin_loader.h"

#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

ClientPluginLoader::ClientPluginLoader() = default;

ClientPluginLoader::~ClientPluginLoader() {
    close();
}

bool ClientPluginLoader::open(const std::string& plugin_path) {
    close();
    last_error_.clear();

#if defined(_WIN32)
    HMODULE h = LoadLibraryA(plugin_path.c_str());
    if (h == nullptr) {
        last_error_ = "LoadLibrary failed: " + plugin_path;
        return false;
    }
    FARPROC sym = GetProcAddress(h, "_SA_UIMain");
    if (sym == nullptr) {
        last_error_ = "GetProcAddress(_SA_UIMain) failed";
        FreeLibrary(h);
        return false;
    }
    handle_ = h;
    entry_ = reinterpret_cast<EntryFn>(sym);
#else
    void* h = dlopen(plugin_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (h == nullptr) {
        const char* err = dlerror();
        last_error_ = err != nullptr ? err : "dlopen failed";
        return false;
    }
    dlerror();
    void* sym = dlsym(h, "_SA_UIMain");
    const char* err = dlerror();
    if (err != nullptr) {
        last_error_ = err;
        dlclose(h);
        return false;
    }
    handle_ = h;
    entry_ = reinterpret_cast<EntryFn>(sym);
#endif
    return true;
}

int ClientPluginLoader::run(const GKC::LcInterface<GKC::IUiHost>& lcHost,
                            const std::vector<std::string>& args) {
    if (entry_ == nullptr) return -1;

    std::vector<GKC::ConstStringS> gkc_args;
    gkc_args.reserve(args.size());
#if defined(_WIN32)
    std::vector<std::wstring> wide_args;
    wide_args.reserve(args.size());
    for (const std::string& arg : args) {
        wide_args.emplace_back(arg.begin(), arg.end());
        gkc_args.emplace_back(wide_args.back().c_str(), wide_args.back().size());
    }
#else
    for (const std::string& arg : args) {
        gkc_args.emplace_back(arg.c_str(), arg.size());
    }
#endif

    GKC::ConstArray<GKC::ConstStringS> arg_array;
    if (!gkc_args.empty()) {
        arg_array = GKC::ConstArray<GKC::ConstStringS>(
            gkc_args.data(), gkc_args.size());
    }

    return entry_(lcHost, arg_array);
}

void ClientPluginLoader::close() {
    if (handle_ == nullptr) return;
#if defined(_WIN32)
    FreeLibrary(static_cast<HMODULE>(handle_));
#else
    dlclose(handle_);
#endif
    handle_ = nullptr;
    entry_ = nullptr;
}
