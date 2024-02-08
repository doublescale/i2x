# i2x
Image viewer specialized for large PNG collections generated with Stable Diffusion.

## Build & Run
A C compiler and development packages for X11, Xi, GL, GLX must be installed.

On Ubuntu,

```bash
sudo apt install gcc libx11-dev libxi-dev libgl-dev libglx-dev
```

Then, to build and run (try it on the test/ directory in this repo):

```bash
./build
./i2x <files and directories>
```

Press F1 for GUI help.
Invoke ./i2x --help for command-line help.

## Features
- Handle tens of thousands of images.
- Fast. Should start up in <0.5s.  Flip between loaded images instantly.
- Limited support for JPEG, BMP, and some other image formats.
- Automatic refresh on file changes.
- Show Stable Diffusion generation metadata contained in PNGs.  i2x intends to support stable-diffusion-webui and ComfyUI.
- Search images by prompt and other parameters.

Here's a video showing a search over 35k images:
![Search Demo](demo/search.gif)

## Known Limitations
- Linux-only.
- Image loading not security-hardened.  Only use this program on trusted files.
- XInput2 handling might be glitchy.  If you encounter unexpected mouse inputs (particularly when scrolling), XInput2 can be disabled by setting this environment variable:  I2X_DISABLE_XINPUT2= ./i2x <paths>
- Partial Unicode support.  No fallback fonts.
- Refreshing loses filtered view. (WIP)

WIP items might get fixed at some point.
