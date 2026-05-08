#include <cstdio>
#include <string>
#include <vector>

#include "base/GkcDef.h"
#include "../../third_party/GKC/util/private/include/ui/UIDef.h"

#include "src/client/client_log.h"
#include "src/client/plugin_loader.h"

#include "base/SysDef.h"

namespace {

std::string to_std_string(const GKC::ConstStringS& s) {
    std::string out;
    out.reserve(static_cast<size_t>(s.GetLength()));
    for (uintptr i = 0; i < s.GetLength(); ++i) {
        out.push_back(static_cast<char>(s.GetAddress()[i]));
    }
    return out;
}

bool starts_with(const std::string& s, const char* prefix) {
    const std::string p(prefix);
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

std::string default_plugin_path() {
#if defined(_WIN32)
    return "client_viewer.dll";
#else
    return "libclient_viewer.so";
#endif
}

}  // namespace

namespace GKC {

class ProgramEntryPoint {
public:
    static int UIMain(const ConstArray<ConstStringS>& args) {
        client_log_line("client-main: UIMain entered args=" +
                        std::to_string(static_cast<size_t>(args.GetCount())));
        if (!GKC::g_win_ui_host.Initialize()) {
            client_log_line("client-main: g_win_ui_host.Initialize failed");
            return 101;
        }
        client_log_line("client-main: g_win_ui_host initialized");

        std::vector<std::string> forwarded;
        forwarded.reserve(args.GetCount());
        std::string plugin_path = default_plugin_path();

        for (uintptr i = 1; i < args.GetCount(); ++i) {
            std::string arg = to_std_string(args[i]);
            if (starts_with(arg, "--plugin=")) {
                plugin_path = arg.substr(std::string("--plugin=").size());
            }
            client_log_line("client-main: arg " + arg);
            forwarded.push_back(std::move(arg));
        }
        client_log_line("client-main: plugin_path=" + plugin_path);

        ClientPluginLoader loader;
        if (!loader.open(plugin_path)) {
            std::fprintf(stderr, "client: %s\n", loader.last_error().c_str());
            client_log_line("client-main: plugin open failed: " +
                            loader.last_error());
            return 102;
        }
        client_log_line("client-main: plugin opened");

        const int rc = loader.run(
            LcInterface<IUiHost>(&GKC::g_win_ui_host,
                                 RefPtr<IUiHost>(GKC::g_ui_host_interface)),
            forwarded);
        client_log_line("client-main: plugin returned rc=" +
                        std::to_string(rc));
        return rc;
    }
};

}  // namespace GKC

#include "base/GkcDef.cpp"
#include "base/SysDef.cpp"
#include "../../third_party/GKC/util/private/include/ui/GkcUIMain.cpp"
