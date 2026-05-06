#pragma once

#include <string>
#include <vector>

#include "base/GkcDef.h"

// Loads a client viewer plugin and invokes its GKC _SA_UIMain entry point.
class ClientPluginLoader {
public:
    ClientPluginLoader();
    ~ClientPluginLoader();

    ClientPluginLoader(const ClientPluginLoader&) = delete;
    ClientPluginLoader& operator=(const ClientPluginLoader&) = delete;

    // Loads the viewer shared library and resolves `_SA_UIMain`.
    bool open(const std::string& plugin_path);

    // Runs the loaded plugin with the real GUI host and forwarded arguments.
    int run(const GKC::LcInterface<GKC::IUiHost>& lcHost,
            const std::vector<std::string>& args);

    const std::string& last_error() const { return last_error_; }

private:
    using EntryFn = int (*)(const GKC::LcInterface<GKC::IUiHost>&,
                            const GKC::ConstArray<GKC::ConstStringS>&);

    void close();

    void* handle_ = nullptr;
    EntryFn entry_ = nullptr;
    std::string last_error_;
};
