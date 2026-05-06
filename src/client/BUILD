cc_library(
    name = "pixel_renderer",
    srcs = ["pixel_renderer.cpp"],
    hdrs = ["pixel_renderer.h"],
    deps = [
        "//third_party:GkcSys",
    ],
    copts = ["-std=c++17"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "pixel_renderer_test",
    srcs = ["pixel_renderer_test.cpp"],
    deps = [
        ":pixel_renderer",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
    copts = ["-std=c++17"],
)

cc_library(
    name = "client_runtime",
    srcs = ["client.cpp"],
    hdrs = ["client.h"],
    deps = [
        ":pixel_renderer",
        "//src/protocol:message_parser",
        "//src/protocol:protocol",
    ],
    linkopts = select({
        "@platforms//os:windows": ["ws2_32.lib"],
        "//conditions:default": [],
    }),
    copts = ["-std=c++17"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "plugin_loader",
    srcs = ["plugin_loader.cpp"],
    hdrs = ["plugin_loader.h"],
    deps = [
        "//third_party:GkcSys",
    ],
    linkopts = select({
        "@platforms//os:linux": ["-ldl"],
        "//conditions:default": [],
    }),
    copts = ["-std=c++17"],
    visibility = ["//visibility:public"],
)

cc_binary(
    name = "client",
    srcs = ["main.cpp"],
    deps = [
        ":plugin_loader",
        "//third_party:GkcSys",
        "//third_party:gkc_uihost_runtime",
    ],
    linkopts = select({
        "@platforms//os:windows": [
            "/SUBSYSTEM:WINDOWS",
            "/ENTRY:wWinMainCRTStartup",
            "user32.lib",
            "gdi32.lib",
            "shell32.lib",
            "ole32.lib",
        ],
        "//conditions:default": [],
    }),
    copts = ["-std=c++17"],
    visibility = ["//visibility:public"],
)
