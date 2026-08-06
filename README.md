<div align="center">

<img src="extras/banner.png" alt="Cut the Rope 2 banner" width="35%">

</div>
<h1 align="center">Cut the Rope 2 - Nintendo Switch port</h1>

This is a wrapper/port of the Android ARM64 version of Cut the Rope 2 (`1.47.1`). <br/>
It loads the original game's ARM64 library and runs it as-is inside a minimal Android compatibility environment.

### How to install

You will need a legally obtained copy of Cut the Rope 2 `1.47.1`.

Create `/switch/ctr2_nx/` on your SD card, copy `ctr2_nx.nro` there, then extract the complete contents of the APK into the directory.

```text
/switch/ctr2_nx/
  ctr2_nx.nro
  lib/
    arm64-v8a/
      libctr2.so
  assets/
  res/
```

On first boot, the port validates the game data, locates `libctr2.so`, preserves the MP4 movies, and removes Android files that are not needed on Nintendo Switch.

### Notes

This will not work in applet/album mode. Use a game override (hold R while launching a title) or a forwarder so the game receives the full application memory pool and the required code-memory permissions.

### How to build

You need devkitA64 plus the following devkitPro packages:

* `switch-mesa`
* `switch-libdrm_nouveau`
* `switch-sdl2`
* `switch-zlib`
* `switch-ffmpeg`
* `switch-libvorbisidec`
* `switch-libogg`

From an MSYS2 environment with devkitPro mounted at `/opt/devkitpro`:

```bash
./build.sh clean -j4
```

The resulting release build is `ctr2_nx.nro`.

### Credits

* TheOfficialFloW, Andy Nguyen, and fgsfds for the Android shared-object loader lineage
* devkitPro and libnx contributors for the Nintendo Switch homebrew toolchain

### Support

If you enjoy my work and want to support me:

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/D1D1P2MOG)

### Legal

This project is not affiliated with ZeptoLab. Cut the Rope 2 and all related assets are trademarks or property of their respective owners. All rights reserved.
No assets or program code from Cut the Rope 2 are included in this repository. We do not condone piracy and encourage users to legally obtain the original game.
