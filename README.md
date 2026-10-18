<div align="center">
  <img src="res/logo.png" alt="Logo" width="640">

  <p align="center">
    <a href="https://twilitrealm.dev">Official Website</a>
    •
    <a href="https://discord.gg/6NpMhefCK9">Discord</a>
  </p>
</div>

# Overview

Dusklight is a reverse-engineered reimplementation of Twilight Princess.

It aims to be as accurate as possible to the original while also providing new options, enhancements, and tools to customize your experience.

## Robin's Custom Kamilink build

This is an unofficial public build maintained by Robin. It adds custom compatibility APIs and services and tracks upstream Dusklight through a scheduled GitHub Actions workflow that opens reviewable sync pull requests. Publishing a `v*-robin-*` tag builds Windows AMD64, Linux, Android, and iOS prerelease assets. The Android update control links to its arm64 APK, while the Windows updater installs the matching Windows package and preserves the `data` folder.

Android release APKs are signed by the release workflow for direct installation. The persistent PKCS#12 release keystore is stored outside the repository. Configure these repository Actions secrets before publishing a tag: `ANDROID_KEYSTORE_BASE64` (the keystore file encoded as one-line base64), `ANDROID_KEYSTORE_PASSWORD`, `ANDROID_KEY_ALIAS` (`dusklight-release`), and `ANDROID_KEY_PASSWORD`. For this build, the local keystore and password are under `%LOCALAPPDATA%\Dusklight\AndroidReleaseSigning`. To set the secrets from PowerShell, pipe `android-release.p12.base64` and `password.txt` from that directory to `gh secret set ANDROID_KEYSTORE_BASE64` and the two password secret commands; set `ANDROID_KEY_ALIAS` to `dusklight-release`. Keep the keystore and its passwords private and backed up; every Android release must use this same signing key so users can install updates over prior releases. A new key cannot update an installation signed with a different key. Tag releases fail rather than publish an unsigned APK if these secrets are missing.

> [!IMPORTANT]
> Dusklight's official website is https://twilitrealm.dev/, any other website is not affiliated and may be promoting AI-generated misinformation.

# Setup

> [!IMPORTANT]
> Dusklight does *not* provide any copyrighted assets. You must provide your own copy of the original game.

> [!IMPORTANT]
> At a minimum, Dusklight requires a GPU with support for D3D12, Vulkan 1.1+, or Metal. For older devices, best-effort support is provided for D3D11 and OpenGL ES (Android), but will not achieve full accuracy or performance. Your experience with specific hardware, operating systems, and drivers may vary.

### 1. Dump your game

You must dump your own copy of the game. Please see [this article](https://wiki.dolphin-emu.org/index.php?title=Ripping_Games) for instructions. After dumping, you can use a program like [Dolphin](https://dolphin-emu.org/) or [nodtool](https://github.com/encounter/nod/releases) to convert the `.iso` to `.rvz` to save space.

Dusklight currently supports all commercial discs except for Wii's Korean release.

> [!NOTE]
> Dusklight is based on the [Twilight Princess decompilation](https://github.com/zeldaret/tp), which is currently only matching for GameCube. As a result, even when playing Dusklight with a Wii disc, you will be presented with the GameCube version's HUD and certain other specificities.

### 2. Install Dusklight

Visit the [official installation guide](https://twilitrealm.dev/install/) for full instructions.

# Building

If you'd like to build Dusklight from source, please read the [build instructions](docs/building.md).

Pull requests are welcomed! Note that we do not accept contributions that are primarily AI-generated and will close your PR if we suspect as much. Please also see the [code conventions](docs/code-conventions.md).

# Credits

Special thanks to the [TP decompilation](https://github.com/zeldaret/tp) team, the GC/Wii decompilation community, the [Aurora](https://github.com/encounter/aurora) developers, the [TP speedrunning community](https://zsrtp.link), and all [contributors](https://github.com/TwilitRealm/dusklight/graphs/contributors).

<br/>
<div align="center">
    <a href="https://github.com/encounter/aurora">
        <img src="assets/aurora-powered.png" alt="Powered by Aurora" width="800">
    </a>
</div>
