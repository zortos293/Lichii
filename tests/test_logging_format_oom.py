"""Source-extracted smoke test for FormatLogMessage's noexcept OOM contract.

Run: python3 tests/test_logging_format_oom.py [--cxx g++|clang++]
No game resources, Vulkan, or actual low-memory condition required.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    code = (ROOT / "src/common/logging.cpp").read_text(encoding="utf-8")
    start = code.index("std::string FormatLogMessage(const Entry& entry) noexcept {")
    stop = code.index("\n}\n", start) + 2
    fn = code[start:stop]
    if "fmt::format(" not in fn:
        raise SystemExit("formatter changed: update the test's fault-injection point")
    fn = fn.replace("fmt::format(", "ProbeFormat(", 1)
    prefix = r'''
#include <chrono>
#include <cstdint>
#include <new>
#include <string>
#include <cstdio>
struct Entry {
    const char* filename;
    std::chrono::microseconds timestamp{};
    int log_class = 0, log_level = 0, line_num = 3;
    const char* function = "unit", *message = "message";
};
const char* GetLogClassName(int) { return "Core"; }
const char* GetLevelName(int) { return "Warning"; }
static bool force_oom = false;
template <class... Args>
std::string ProbeFormat(const char*, Args&&...) {
    if (force_oom) throw std::bad_alloc{};
    return "formatted";
}
'''
    suffix = r'''
int main() {
    Entry e{"log.cpp"};
    if (FormatLogMessage(e) != "formatted") return 1;
    force_oom = true;
    if (!FormatLogMessage(e).empty()) return 2;
    e.filename = nullptr;
    if (!FormatLogMessage(e).empty()) return 3;
    std::puts("PASS: normal format, injected bad_alloc, missing filename");
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as d:
        cpp = Path(d) / "format.cpp"
        exe = Path(d) / "format"
        cpp.write_text(prefix + fn + suffix, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)

if __name__ == "__main__":
    main()
