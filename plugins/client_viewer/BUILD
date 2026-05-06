cc_binary(
    name = "client_viewer.dll",
    srcs = ["client_viewer.cpp"],
    deps = [
        "//src/client:client_runtime",
        "//src/client:pixel_renderer",
        "//src/common:input_event",
        "//third_party:GkcSys",
        "//third_party:gkc_plugin_runtime",
    ],
    linkshared = True,
    linkopts = select({
        "@platforms//os:windows": ["ws2_32.lib"],
        "//conditions:default": [],
    }),
    copts = ["-std=c++17"],
    defines = select({
        "@platforms//os:windows": ["SHARED_LIBRARY"],
        "//conditions:default": [],
    }),
    visibility = ["//visibility:public"],
)

alias(
    name = "client_viewer",
    actual = ":client_viewer.dll",
    visibility = ["//visibility:public"],
)
