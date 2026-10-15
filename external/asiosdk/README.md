# Steinberg ASIO SDK — drop zone (never committed)

The ASIO SDK is Steinberg's code, dual-licensed GPLv3 or Steinberg proprietary.
It is kept out of this repo; `.gitignore` blocks everything here except this file.

## Install (one command, from the repo folder)
```
powershell -ExecutionPolicy Bypass -File tools\fetch-asio-sdk.ps1
```
Downloads ASIO SDK 2.3.4 from download.steinberg.net, checks its SHA-512, applies
the asiolist.cpp delete[] fix, and installs it to `external/asiosdk/sdk/`.

Then build the ASIO-enabled version:
```
cmake --preset rig-asio
cmake --build --preset rig-asio-release
```
