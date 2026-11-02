# Thin compressed texture compatibility (Windows)

D3D11 rejects BC textures whose base width or height is not a multiple of four.
Xbox textures such as the game's 256x2 DXT5 gym texture still need their exact
dimensions, mip chain and compressed lock representation. The D3D8 resource
bridge stores those textures as BGRA and decodes uploads using the existing
DXT decoder; block-aligned base textures retain native BC storage.

Run from an MSVC developer shell:

```powershell
cmake -S tests/d3d8_thin_dxt -B build/thin-dxt -G Ninja
cmake --build build/thin-dxt
ctest --test-dir build/thin-dxt --output-on-failure
```

The fixture uses WARP and synthetic blocks only. It exercises DXT1/3/5 at
256x2, 2x256, 1x1, 2x2, 6x5 and an aligned 8x8 control, including every mip,
GPU pixel/alpha readback, guest texture/surface locks, valid and invalid
compressed subrectangles, and a surface that outlives its parent texture handle.
The original resource implementation fails the first 256x2 allocation with
`E_INVALIDARG`; the fixed implementation passes all eighteen combinations.
