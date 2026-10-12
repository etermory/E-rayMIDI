#!/usr/bin/env bash
# Cross-build on macOS/Linux, or build with a Bash shell on Windows.
# Usage: ZIG=/path/to/zig RC=/path/to/llvm-rc bash build.sh /path/to/platform-tools
set -euo pipefail
source_dir="$(cd "$(dirname "$0")" && pwd)"
repo_dir="$(cd "$source_dir/../.." && pwd)"
tools_dir="$(cd "${1:?Pass the extracted Windows platform-tools directory}" && pwd)"
out_dir="$repo_dir/build/windows-native"
mkdir -p "$out_dir"
zig_bin="${ZIG:-zig}"
for name in adb.exe AdbWinApi.dll AdbWinUsbApi.dll NOTICE.txt; do
    test -f "$tools_dir/$name" || { echo "Missing $tools_dir/$name" >&2; exit 1; }
done
# Hash all bundled components so a new build never overwrites an older running ADB.
adb_id=$(cat "$tools_dir/adb.exe" "$tools_dir/AdbWinApi.dll" "$tools_dir/AdbWinUsbApi.dll" | shasum -a 256 | cut -c 1-16)
cat > "$out_dir/resources.rc" <<EOF
101 RCDATA "$tools_dir/adb.exe"
102 RCDATA "$tools_dir/AdbWinApi.dll"
103 RCDATA "$tools_dir/AdbWinUsbApi.dll"
104 RCDATA "$tools_dir/NOTICE.txt"
EOF
if test -n "${RC:-}"; then
    (cd "$out_dir" && "$RC" /fo resources.res resources.rc)
else
    (cd "$out_dir" && "$zig_bin" rc /fo resources.res resources.rc)
fi
"$zig_bin" c++ -target x86_64-windows-gnu -std=c++20 -O2 -static -municode \
    -Wall -Wextra -Werror "-DERAY_ADB_ID=L\"$adb_id\"" \
    "$source_dir/receiver.cpp" "$out_dir/resources.res" \
    -lole32 -luuid -lws2_32 -o "$repo_dir/build/E-rayMIDI-Receiver.exe"
cp "$tools_dir/NOTICE.txt" "$out_dir/ADB-NOTICE.txt"
printf '%s\n' "$repo_dir/build/E-rayMIDI-Receiver.exe"
