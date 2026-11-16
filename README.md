# AirPlay Monitor

OBS Studio plugin that mirrors Program or Preview to an AirPlay receiver. Open it from **Tools → AirPlay Monitor**. The stream is independent of OBS Start Streaming. It runs on macOS, Windows, and Linux.

The mirror uses the public AirPlay screen-mirroring sequence: mDNS browse for `_airplay._tcp`, an RTSP session, and 128-byte mirror packets. Video is encoded by one of OBS's own H.264 or HEVC encoders (hardware first: VideoToolbox, NVENC, AMF, Quick Sync, VA-API; otherwise x264). It is HEVC Main when the receiver advertises multi-codec screen mirroring and an HEVC encoder exists, otherwise H.264 High, with B-frames off. Audio, when enabled, is the main mix (track 1) as 48 kHz AAC-ELD, including while the picture is Preview. Every receiver gets the same requests and packets, modeled on a Mac mirroring to a TV. Only what the receiver advertises changes them: FairPlay, a pairing code, and HEVC.

## Limits

Receivers that advertise a one-time code show that code only after Connect asks them to. A Mac used as the receiver shows that code on the Mac. Leave the code field empty, press Connect, and wait for the code. Then type it and press Connect again. The plugin keeps that connection open, runs pair-setup, and encrypts the control connection. A receiver that does not offer FairPlay uses the pairing key for the picture. A receiver that advertises FairPlay still completes FairPlay SAP. Either way the video is encrypted. The code is not saved and is not written to the log. A wrong code stops the session. Disconnect, then connect again so the TV can show a new code.

The FairPlay SAP code lives in `third_party/fairplay-sap`, which is LGPL-3.0-or-later. Building it into the plugin makes the combined plugin GPL-3.0-or-later. See [NOTICE](NOTICE).

Receivers that do not ask for a code keep the original unauthenticated sequence. When one of those publishes an RSA `pk`, the stream key is wrapped with RSA-OAEP and the video payload is AES-CTR. Otherwise that mirror is unencrypted. A receiver that answers `401`, `403`, or `470` before pairing stops instead of hanging.

Glass-to-glass delay is mostly the receiver. The plugin keeps one raw frame, does not reorder encodes, and drops queued packets instead of buffering. Expect about 80–200 ms on a receiver that displays frames as they arrive.

The macOS preset builds arm64 for macOS 12 and later. Windows needs 64-bit Windows 10 or later. Linux builds against the system OBS and Qt 6.

Tested for picture and main-mix audio on a Sharp Roku TV (model 7203X) and on a Mac as the receiver, sending from macOS. Apple TV and other AirPlay receivers have not been verified. Sending from Windows and Linux has not been verified yet.

Audio stays in step with the picture by timing each packet from when OBS captured it. A receiver may still play its sound slightly early or late. Audio sync offset corrects that.

## Audio on Windows and Linux

AirPlay audio is AAC-ELD. macOS has that encoder built in. On Windows and Linux the plugin uses the fdk-aac library if it is installed. The plugin does not include fdk-aac because its license does not allow shipping it inside a GPL plugin. Without fdk-aac, Send audio is greyed out and the mirror sends picture only. Video does not need fdk-aac.

The plugin needs fdk-aac version 0.1 or 2.x with its encoder. It turns off SBR, so a build without HE-AAC (such as Fedora's `fdk-aac-free`) is enough. After installing it, restart OBS. Send audio is no longer greyed out once the plugin finds the library.

### Linux

Install the package that provides `libfdk-aac.so.2`:

| Distribution | Command |
| --- | --- |
| Ubuntu 22.04 and newer | `sudo add-apt-repository multiverse && sudo apt update && sudo apt install libfdk-aac2` |
| Debian 12 and newer | Add `non-free` to the components in `/etc/apt/sources.list` (or `/etc/apt/sources.list.d/debian.sources`), then `sudo apt update && sudo apt install libfdk-aac2` |
| Fedora | `sudo dnf install fdk-aac-free` |
| Arch Linux | `sudo pacman -S libfdk-aac` |

On another distribution, install whichever package provides `libfdk-aac.so.2` (or `libfdk-aac.so.1`). To check that the library is visible:

```sh
ldconfig -p | grep libfdk-aac
```

OBS installed as a Flatpak cannot see libraries installed on the system, so this applies to OBS from your distribution's packages or the OBS PPA.

### Windows

There is no official Windows download of fdk-aac. The simplest source is MSYS2:

1. Install MSYS2 from [msys2.org](https://www.msys2.org/).
2. Open the **MSYS2 UCRT64** shell and run:

   ```sh
   pacman -S mingw-w64-ucrt-x86_64-fdk-aac
   ```

3. The library is `C:\msys64\ucrt64\bin\libfdk-aac-2.dll`. List the other DLLs it needs:

   ```sh
   ldd /ucrt64/bin/libfdk-aac-2.dll | grep ucrt64
   ```

4. Copy `libfdk-aac-2.dll`, and every DLL that `ldd` listed under `/ucrt64/bin`, into one of these folders:
   - the folder that holds `obs-airplay-monitor.dll`. That is usually `C:\ProgramData\obs-studio\plugins\obs-airplay-monitor\bin\64bit\`, or `C:\Program Files\obs-studio\obs-plugins\64bit\` for an older install.
   - the folder that holds `obs64.exe`, usually `C:\Program Files\obs-studio\bin\64bit\`.

The plugin looks in its own folder first, then next to `obs64.exe`, then on `PATH`.

A build from [vcpkg](https://vcpkg.io/) also works: `vcpkg install fdk-aac:x64-windows`, then copy `fdk-aac.dll` from `installed\x64-windows\bin\` to one of the folders above.

## Build

Every platform needs CMake 3.28 or newer and a C++20 compiler. The repository already contains the two libraries the plugin builds in:

- `third_party/tweetnacl` is TweetNaCl 20140427 (public domain). `src/airplay/nacl_random.c` supplies `randombytes`.
- `third_party/fairplay-sap` is the FairPlay SAP sender from [objevovat/fairplay-sap-core-airplay2-sender-authentication-handshake](https://github.com/objevovat/fairplay-sap-core-airplay2-sender-authentication-handshake): its C port, plus a C++ port of its Go packages. It is licensed LGPL-3.0-or-later. See [NOTICE](NOTICE).

### macOS

Install Xcode, including the macOS SDK.

The first `cmake --preset macos` downloads the OBS build dependencies into `.deps/` and builds `libobs` and `obs-frontend-api` there. `.gitignore` leaves `.deps/` out of the tree. Versions and SHA-256 hashes are in [buildspec.json](buildspec.json). CMake skips an archive that is already present under `.deps/`, so these files can be downloaded by hand and placed there:

| Piece | Archive |
| --- | --- |
| OBS Studio 31.1.1 sources | https://github.com/obsproject/obs-studio/archive/refs/tags/31.1.1.tar.gz |
| Prebuilt obs-deps 2025-07-11 | https://github.com/obsproject/obs-deps/releases/download/2025-07-11/macos-deps-2025-07-11-universal.tar.xz |
| Prebuilt Qt 6 2025-07-11 | https://github.com/obsproject/obs-deps/releases/download/2025-07-11/macos-deps-qt6-2025-07-11-universal.tar.xz |

Save them as `.deps/31.1.1.tar.gz`, `.deps/macos-deps-2025-07-11-universal.tar.xz`, and `.deps/macos-deps-qt6-2025-07-11-universal.tar.xz`. A network connection is required for that first configure when those files are absent. Later configures reuse `.deps/`.

```sh
cmake --preset macos
cmake --build --preset macos
```

The product is `obs-airplay-monitor.plugin` under `build_macos/RelWithDebInfo`. Copy it to:

```text
~/Library/Application Support/obs-studio/plugins/
```

macOS will ask OBS for local-network permission the first time discovery runs. Allow it, or receivers stay empty.

### Windows

Install:

Install Visual Studio 2022 with the **Desktop development with C++** workload and the Windows 11 SDK (10.0.22621).

Like macOS, the first configure downloads the OBS sources, obs-deps, and Qt for Windows into `.deps/`. From a Developer PowerShell:

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64
cmake --install build_x64 --config RelWithDebInfo --prefix "$env:ProgramData\obs-studio\plugins"
```

The install step needs an administrator shell. It creates `obs-airplay-monitor\bin\64bit\` holding `obs-airplay-monitor.dll`, and `obs-airplay-monitor\data\` holding the translations. Windows Firewall may ask to allow OBS on private networks the first time you connect. Allow it, or the receiver cannot reach the timing port.

### Linux

On Ubuntu 24.04 or newer, with OBS from the [OBS PPA](https://launchpad.net/~obsproject/+archive/ubuntu/obs-studio):

```sh
sudo apt install cmake ninja-build g++ pkg-config qt6-base-dev qt6-base-private-dev libgl-dev obs-studio
```

The PPA's `obs-studio` package includes the OBS headers. With Ubuntu's own `obs-studio` package, also install `libobs-dev`. Then:

```sh
cmake --preset ubuntu-x86_64
cmake --build --preset ubuntu-x86_64
mkdir -p ~/.config/obs-studio/plugins/obs-airplay-monitor/bin/64bit
cp build_x86_64/obs-airplay-monitor.so ~/.config/obs-studio/plugins/obs-airplay-monitor/bin/64bit/
cp -r data ~/.config/obs-studio/plugins/obs-airplay-monitor/
```

If a firewall such as `ufw` is on, allow incoming traffic from the local network. Receivers answer discovery from their own addresses, and they connect back to OBS for timing, so neither counts as a reply to traffic OBS sent. With `ufw`, for example, run `sudo ufw allow from 192.168.1.0/24`, using your own network's range.

### Tests

The protocol tests run on macOS, because they check the plugin's plist, crypto, and pairing code against CoreFoundation and Security. They also check the FairPlay port against output of the original Go module. They compile the vendored TweetNaCl and FairPlay code and do not need OBS or Qt:

```sh
sh tests/run.sh
```

## Settings

The receiver list shows the name, model, address, and whether the device advertises screen mirroring. Connect sends Program or Preview. Video encoder `Automatic` tries the hardware encoders first and falls back to software. Choosing an encoder tries that one first. Send audio applies on the next connect. Bitrate `Automatic` follows the picture size (about 12 Mbps at 1080p60 H.264, 0.6× for HEVC, clamped to 1.5–40 Mbps). Audio sync offset runs from −100 to 500 ms, and applies while connected. A lower value plays the sound earlier. Closing the window hides it and leaves the mirror running. Disconnect stops it.

Saved settings are the last receiver id, Program or Preview, the audio checkbox, the audio sync offset, the bitrate, and the video encoder. The pairing code is not saved. After a TV accepts a code, the plugin keeps its pairing identity and skips the code on the next connection to that TV.

## License

Source files other than the FairPlay module are GPL-2.0-or-later, the same license as OBS Studio. The plugin binary builds in `third_party/fairplay-sap` (LGPL-3.0-or-later) and is distributed as GPL-3.0-or-later. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
