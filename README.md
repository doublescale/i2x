# i2x
Image viewer specialized for PNGs generated with Stable Diffusion.

## Build & Run
```bash
OPTIMIZE=1 ./build
./i2x <files and directories>
```

## Features
- Handle tens of thousands of images in a directory.
- Show Stable Diffusion generation metadata contained in PNGs.  i2x intends to support stable-diffusion-webui and ComfyUI.
- Search images by prompt.

## Known Limitations
- Linux-only.
- Image loading not security-hardened.  Only use this program on trusted files.
- XInput2 handling might be glitchy.  If you encounter unexpected mouse inputs (particularly when scrolling), please exit the program send me the input-log file at /tmp/i2x-debug.log before starting another instance of i2x.  XInput2 (required for smooth scrolling) can be disabled by setting this environment variable:  I2X_DISABLE_XINPUT2= ./i2x <paths>
- Partial Unicode support.  No fallback fonts.
- Leaks memory in some circumstances. (WIP)
- Reloading of images could be better. (WIP)
- Parsing metadata is done up-front, synchronously.  Opening many uncached images may take a while. (WIP)
- Developer accepts donations: <TODO>

WIP items will probably get fixed at some point.