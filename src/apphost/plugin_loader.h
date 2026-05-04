#pragma once

#include "base/GkcDef.h"

#include <string>

// PluginLoader — loads a GKC SA plugin .so and runs it.
//
// The .so is a compiled business application module, not just an interface
// description.  It contains machine code for the plugin's GuiMain(),
// window classes, drawing handlers, and a plugin-local GKC::g_ui_host global.
//
// Convention (see docs/design/architecture.md §5.2):
//   - Plugin .so exports `extern "C" int _SA_UIMain(LcInterface<IUiHost>&,
//     ConstArray<ConstStringS>&)`, supplied by GKC's GkcGui.cpp shim.
//   - The shim assigns its argument to the plugin-local g_ui_host global
//     and invokes `program_entry_point::GuiMain(args)`.
//
// _SA_UIMain is the stable C ABI entry point that lets AppHost run any plugin
// without knowing the plugin's C++ class names.  AppHost only needs the symbol
// name and function signature:
//
//   dlopen("libdemo_app.so")
//   dlsym("_SA_UIMain")
//   entry_(apphost_fake_uihost, args)
//
// From there the plugin calls back into AppHost through the injected
// LcInterface<IUiHost>.
//
// open() does dlopen + dlsym. run() invokes the entry and blocks until the
// plugin returns (typically when GuiHelper::Quit() unwinds GuiHelper::Loop()).
class PluginLoader {
public:
    PluginLoader();
    ~PluginLoader();

    PluginLoader(const PluginLoader&)            = delete;
    PluginLoader& operator=(const PluginLoader&) = delete;

    // Loads `plugin_path` with RTLD_NOW | RTLD_LOCAL and resolves the
    // `_SA_UIMain` symbol.  Returns false on dlopen / dlsym failure;
    // last_error() then carries the dlerror text.
    bool open(const std::string& plugin_path);

    // Calls `_SA_UIMain(lcHost, empty_args)` and returns its exit code.
    // Returns -1 if the plugin was not opened.  Plugin args are not yet
    // forwarded in M4 (demo plugin does not read argv).
    int run(const GKC::LcInterface<GKC::IUiHost>& lcHost);

    // dlerror() text from the last failed open().
    const std::string& last_error() const { return last_error_; }

private:
    using EntryFn = int (*)(const GKC::LcInterface<GKC::IUiHost>&,
                            const GKC::ConstArray<GKC::ConstStringS>&);

    void*       handle_     = nullptr;
    EntryFn     entry_      = nullptr;
    std::string last_error_;
};
