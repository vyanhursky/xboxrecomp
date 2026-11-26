# Raw GPU draw bridge fixtures

These tests contain no game data. The four portable tests cover planning bounds,
source/native record padding, executor ordering, packet batching, mutable guest
buffers and native-rejection fallback. Windows adds 288 WARP comparisons of
native vertex inputs against an independent CPU conversion reference.

```sh
cmake -S tests/nv2a_raw_draw -B build/raw-draw -DCMAKE_BUILD_TYPE=Release
cmake --build build/raw-draw --config Release
ctest --test-dir build/raw-draw -C Release --output-on-failure
```

On Linux or macOS, the optional offscreen Vulkan test links the actual backend.
It needs Vulkan headers/loader, shaderc and Python 3. No SDL window is required.

```sh
cmake -S tests/nv2a_raw_draw -B build/raw-vulkan -DCMAKE_BUILD_TYPE=Release \
  -DXBOXRECOMP_RAW_VULKAN_TEST=ON
cmake --build build/raw-vulkan
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
  ctest --test-dir build/raw-vulkan --output-on-failure
```

Install the Khronos validation layer before using that environment variable.
For software-only Linux testing, select the installed Lavapipe ICD through
`VK_ICD_FILENAMES`. Software-driver success does not qualify hardware gameplay,
performance or MoltenVK. The Vulkan tests compare actual target pixels, separate
stream layouts, packed formats, MSAA, ring wrapping and exhausted-cache fallback.
The wrapper rejects validation errors even if the executable exits successfully.

For portable memory/undefined-behavior checking with GCC or Clang:

```sh
cmake -S tests/nv2a_raw_draw -B build/raw-asan -DCMAKE_BUILD_TYPE=Debug \
  '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
  '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined'
cmake --build build/raw-asan
ctest --test-dir build/raw-asan --output-on-failure
```
