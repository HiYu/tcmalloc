# Copyright 2019 The TCMalloc Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

workspace(name = "com_google_tcmalloc")

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

# Load a recent version of skylib in case our dependencies have obsolete
# versions. This is needed for bazel 6 compatibility.
http_archive(
    name = "bazel_skylib", # 2022-09-01
    urls = ["https://github.com/bazelbuild/bazel-skylib/archive/refs/tags/1.3.0.zip"],
    strip_prefix = "bazel-skylib-1.3.0",
    sha256 = "4756ab3ec46d94d99e5ed685d2d24aece484015e45af303eb3a11cab3cdc2e71",
)

# Abseil
http_archive(
    name = "com_google_absl",
    urls = ["https://github.com/abseil/abseil-cpp/archive/a3020c763c12bd16bbf00804abe853afa5778174.zip"],
    strip_prefix = "abseil-cpp-a3020c763c12bd16bbf00804abe853afa5778174",
    sha256 = "0b0cd6cacd754b88cfc098e8a87a6e00f1ea5833ebf0c34738dfb9357345b13c",
)

# GoogleTest/GoogleMock framework. Used by most unit-tests.
http_archive(
    name = "com_google_googletest",  # 2023-06-15T14:52:03Z
    urls = ["https://github.com/google/googletest/archive/18fa6a4db32a30675c0b19bf72f8b5f693d21a23.zip"],
    strip_prefix = "googletest-18fa6a4db32a30675c0b19bf72f8b5f693d21a23",
    sha256 = "35908733d9ea615be95517f6e15aea46191b2670d791b17ac841944272093ed2",
)

# RE2
http_archive(
    name = "com_googlesource_code_re2",
    sha256 = "ef516fb84824a597c4d5d0d6d330daedb18363b5a99eda87d027e6bdd9cba299",
    strip_prefix = "re2-03da4fc0857c285e3a26782f6bc8931c4c950df4",
    urls = ["https://github.com/google/re2/archive/03da4fc0857c285e3a26782f6bc8931c4c950df4.tar.gz"],
)

# Google benchmark.
http_archive(
    name = "com_github_google_benchmark",
    urls = ["https://github.com/google/benchmark/archive/0baacde3618ca617da95375e0af13ce1baadea47.zip"],
    strip_prefix = "benchmark-0baacde3618ca617da95375e0af13ce1baadea47",
    sha256 = "62e2f2e6d8a744d67e4bbc212fcfd06647080de4253c97ad5c6749e09faf2cb0",
)

# C++ rules for Bazel.
http_archive(
    name = "rules_cc",  # 2021-05-14T14:51:14Z
    urls = ["https://github.com/bazelbuild/rules_cc/archive/68cb652a71e7e7e2858c50593e5a9e3b94e5b9a9.zip"],
    strip_prefix = "rules_cc-68cb652a71e7e7e2858c50593e5a9e3b94e5b9a9",
    sha256 = "1e19e9a3bc3d4ee91d7fcad00653485ee6c798efbbf9588d40b34cbfbded143d",
)

# Python rules
#
# This is explicitly added to work around
# https://github.com/bazelbuild/rules_fuzzing/issues/207
# and https://github.com/google/tcmalloc/issues/127
http_archive(
    name = "rules_python",
    urls = ["https://github.com/bazelbuild/rules_python/archive/refs/tags/0.11.0.tar.gz"],
    sha256 = "c03246c11efd49266e8e41e12931090b613e12a59e6f55ba2efd29a7cb8b4258",
    strip_prefix = "rules_python-0.11.0",
)

# Proto rules for Bazel and Protobuf
http_archive(
    name = "com_google_protobuf",
    urls = ["https://github.com/protocolbuffers/protobuf/archive/13d559beb6967033a467a7517c35d8ad970f8afb.zip"],
    strip_prefix = "protobuf-13d559beb6967033a467a7517c35d8ad970f8afb",
    sha256 = "9ca59193fcfe52c54e4c2b4584770acd1a6528fc35efad363f8513c224490c50",
)
load("@com_google_protobuf//:protobuf_deps.bzl", "protobuf_deps")
protobuf_deps()

http_archive(
    name = "rules_proto",
    sha256 = "66bfdf8782796239d3875d37e7de19b1d94301e8972b3cbd2446b332429b4df1",
    strip_prefix = "rules_proto-4.0.0",
    urls = [
        "https://mirror.bazel.build/github.com/bazelbuild/rules_proto/archive/refs/tags/4.0.0.tar.gz",
        "https://github.com/bazelbuild/rules_proto/archive/refs/tags/4.0.0.tar.gz",
    ],
)

load("@rules_proto//proto:repositories.bzl", "rules_proto_dependencies", "rules_proto_toolchains")
rules_proto_dependencies()
rules_proto_toolchains()

# Fuzzing
http_archive(
    name = "rules_fuzzing",
    sha256 = "a5734cb42b1b69395c57e0bbd32ade394d5c3d6afbfe782b24816a96da24660d",
    strip_prefix = "rules_fuzzing-0.1.1",
    urls = ["https://github.com/bazelbuild/rules_fuzzing/archive/v0.1.1.zip"],
)

# Protobuf
load("@rules_fuzzing//fuzzing:repositories.bzl", "rules_fuzzing_dependencies")

rules_fuzzing_dependencies()

load("@rules_fuzzing//fuzzing:init.bzl", "rules_fuzzing_init")

rules_fuzzing_init()
