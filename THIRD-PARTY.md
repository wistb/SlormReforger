# Third-party code

Source files kept in this repo, unmodified, so it builds from a clean clone. To check or update one, compare it with the upstream path at the version given.

| Here | Upstream | Version | License |
|---|---|---|---|
| `include/Aurie/shared.hpp` | [Aurie](https://github.com/AurieFramework/Aurie) `Aurie/source/framework/shared.hpp` | tag `v2.0.2` (`5c4839e`) | AGPL-3.0 |
| `include/YYToolkit/` | [YYToolkit](https://github.com/AurieFramework/YYToolkit) `YYToolkit/source/YYTK/Shared/` | `experimental` branch at `d5cc007` | AGPL-3.0 |
| `include/FunctionWrapper/FunctionWrapper.hpp` | YYToolkit `YYToolkit/include/FunctionWrapper/` | `experimental` branch at `d5cc007` | AGPL-3.0 |
| `vendor/imgui/` | [Dear ImGui](https://github.com/ocornut/imgui), core files plus the DX11 and Win32 backends | tag `v1.91.9b` | MIT |

The YYToolkit headers must match the YYToolkit build the mod runs on. Release v5.0.0c was built from the `experimental` branch, and its headers are not the ones on the default branch.

Not kept in the repo: the Aurie and YYToolkit DLLs that ship in the release zip. `./build.sh package` downloads the official builds and checks each against a pinned SHA-256; see `build.sh` for the links and hashes.
