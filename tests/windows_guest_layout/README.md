# Windows guest layout placement

Game-free regression for the startup address collision reported in Def Jam
issue #72. A free RAM mapping must not be accepted if any translated device
window or physical-memory view cannot be placed.

```sh
cmake -S tests/windows_guest_layout -B build/windows-guest-layout -DCMAKE_BUILD_TYPE=Release
cmake --build build/windows-guest-layout --config Release
ctest --test-dir build/windows-guest-layout -C Release --output-on-failure
```

Runs against real Win32 mappings on Windows. On macOS/Linux the same Windows
allocator is exercised through the runtime's Win32 compatibility primitives.
The latter verifies placement, rollback, aliases and error handling but cannot
replace a native Windows run or the reporting player's retest.

Coverage: a foreign mapping blocks MCPX while RAM is free; all 34 placement
operations fail individually with no leaked views; a placement collision is
retried; exhaustion and reservation failure publish no usable layout; every
RAM mirror aliases its base; tiled and physical views alias one another while
remaining separate from the image; AC97 is accessible at its translated 64-bit
address; teardown allows another layout; 128 MB RAM skips mirrors that overlap
hardware windows.
