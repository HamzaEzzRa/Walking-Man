# Walking Man, a music player mod for Death Stranding
**Walking Man** is a native in-game music player mod for **Death Stranding**. This DLL-based mod hooks directly into the game's internal audio and UI systems to add a seamless music player interaction to the game's interface.

# Prerequisites
- Git
- Visual Studio 2022 (Community/Professional)
- Windows 10/11 x64
- vcpkg (for dependency management)

# External Dependencies
Make sure the following is installed:
- directxtk_desktop_2019 (with NuGet)
- imgui (with vcpkg)

The following libraries should be included in your Linker input settings:
- imgui.lib
- xinput.lib
- d3d11.lib
- d3d12.lib
- d3dcompiler.lib

# Screenshots
![Showcase_Play 1](https://staticdelivery.nexusmods.com/mods/3392/images/93/93-1753308425-786883420.png)

![Showcase_Controls_1](https://staticdelivery.nexusmods.com/mods/3392/images/93/93-1753306701-1719850712.png)

# License
This project is licensed under the GPL-3.0 License. See the LICENSE file for details.

# Credits
- Special thanks to [techiew/DirectXHook](https://github.com/techiew/DirectXHook).
- Built using MinHook, ImGui, and lots of time.

# Disclaimer
This project was made through clean-room reverse engineering and does not distribute any copyrighted or licensed game assets. It is an unofficial mod, and is not affiliated with Sony, 505 Games, Kojima Productions, or the Walkman® brand.
