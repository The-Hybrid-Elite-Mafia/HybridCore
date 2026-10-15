# Steinberg ASIO SDK — drop zone

The ASIO SDK is Steinberg's licensed code and is not committed to this repo.

1. Download the ASIO SDK from Steinberg's developer portal: https://www.steinberg.net/developers/
2. Extract the zip into this folder. Either layout works:
   - `external/asiosdk/common/asio.h`
   - `external/asiosdk/asiosdk_<version>/common/asio.h`
3. Re-run configure. CMake prints `[HybridCore] ASIO SDK: <path>` when it is found.

To make a missing SDK a hard error, configure with the `rig-asio` preset.
