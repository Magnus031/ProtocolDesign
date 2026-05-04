# demo_app: example SA UI plugin loaded by AppHost via dlopen + _SA_UIMain.
#
# IMPORTANT: this plugin .so depends on //third_party:gkc_plugin_runtime
# (which exposes GKC's headers and the GkcGui.cpp / GkcSAMain.cpp / GkcDef.cpp
# textual sources) — those are #include'd at the bottom of demo_app.cpp so the
# plugin gets its own GKC::g_ui_host storage and exports an extern "C"
# _SA_UIMain symbol.  AppHost itself MUST NOT link gkc_plugin_runtime.

cc_binary(
    name = "libdemo_app.so",
    srcs = ["demo_app.cpp"],
    deps = [
        "//third_party:gkc_plugin_runtime",
        "//third_party:GkcSys",
    ],
    linkshared = True,
    copts = ["-std=c++17"],
    visibility = ["//visibility:public"],
)
