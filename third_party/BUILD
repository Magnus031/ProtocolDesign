# use to manage the GKC Lib
load("@rules_foreign_cc//foreign_cc:defs.bzl", "cmake")

filegroup(
    name = "gkc_all_srcs",
    srcs = glob(["GKC/**"]), # 注意这里指向了 GKC 子目录
)

cmake(
    name = "gkc_lib",
    cache_entries = {
        "CMAKE_BUILD_TYPE": "Debug",
    },
    lib_source = ":gkc_all_srcs",
    out_lib_dir = "main/assemblies",
    out_shared_libs = ["libGkcSys.so"],
    visibility = ["//visibility:public"],
)