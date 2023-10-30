# i2x
Image viewer specialized for large PNG collections generated with Stable Diffusion.

## Build & Run
A C compiler and development packages for X11, Xi, GL, GLX must be installed.
The DejaVu Sans font must be installed at /usr/share/fonts/TTF/DejaVuSans.ttf (WIP).

```bash
OPTIMIZE=1 ./build
./i2x <files and directories>
```

Press F1 for GUI help.
Invoke ./i2x --help for command-line help.

## Features
- Handle tens of thousands of images.
- Show Stable Diffusion generation metadata contained in PNGs.  i2x intends to support stable-diffusion-webui and ComfyUI.
- Search images by prompt and other parameters.

Here's a video showing a search over 35k images:
![Search](demo/search.gif)

## Known Limitations
- Linux-only.
- Image loading not security-hardened.  Only use this program on trusted files.
- XInput2 handling might be glitchy.  If you encounter unexpected mouse inputs (particularly when scrolling), XInput2 can be disabled by setting this environment variable:  I2X_DISABLE_XINPUT2= ./i2x <paths>
- Partial Unicode support.  No fallback fonts.
- Leaks memory in some circumstances. (WIP)
- Reloading of images could be better. (WIP)

WIP items might get fixed at some point.
