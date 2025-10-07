# DTech Engine

[![License: GPL-3.0](https://img.shields.io/badge/License-GPL--3.0-blue.svg)](https://opensource.org/licenses/GPL-3.0)

DTech is a modern fork of the idTech4 engine, originally powering classics like Doom 3 and Quake 4. It combines the best of upstream projects like RBDOOM-3-BFG (for advanced rendering) and Dhewm3 (for cross-platform stability) to create a lightweight, extensible platform tailored for indie developers and modders. With a focus on immersive FPS, horror, and cooperative experiences, DTech revitalizes idTech4 for 2025-era hardware while maintaining full backward compatibility with original assets and mods.

**Website:** [http://demod.ltd/DTech.html](http://demod.ltd/DTech.html)  
**Developed by:** DeMoD LLC and Asher LeRoy  
**License:** GPL-3.0  

## Features

DTech builds on idTech4's proven foundation with modern enhancements:

- **Rendering and Visuals**:
  - OpenGL 3.3 core profile with Physically Based Rendering (PBR), Global Illumination, bloom, HDR, and experimental soft shadows.
  - Deferred renderer optimized for atmospheric environments in horror or sci-fi games.
  - Support for 4K/120 FPS on mid-range hardware; compatible with TrenchBroom for level mapping.

- **Cross-Platform Support**:
  - Powered by SDL2 and 64-bit architecture for seamless operation on Windows, Linux, macOS, Steam Deck, Android, and iOS.
  - Native controller mapping, haptic feedback, and optimizations for low-end devices like Raspberry Pi.

- **Asset Management with StreamDB**:
  - Rust-based, paged key-value store replacing legacy PK4 ZIPs.
  - Features reverse-trie prefix searches for fast asset lookups, automatic recovery, sub-second queries (up to 100MB/s in Quick Mode), deduplication, versioning, and hot-reloading.
  - Supports hybrid PK4/.sdb handling for mod compatibility and pluggable backends (e.g., cloud or encrypted storage).

- **Audio System**:
  - Custom DSP interface leveraging TMS320C6657 hardware for ray-traced sound propagation and FX chains (e.g., stochastic reflections with 20,000 rays/frame at <20ms latency).
  - Material-aware audio with binaural HRTF, Ambisonics decoding, 1GB DDR3 buffering, up to 64 concurrent voices, and real-time effects like occlusion-aware echoes.

- **Multiplayer and Modding with DeMoD Communications Framework (DCF)**:
  - Handshakeless P2P networking with self-healing redundancy, RTT-based clustering (<50ms groups), and AUTO mode for AI-optimized lobbies.
  - Dynamic asset syncing, plugin extensibility, and <1% overhead for up to 16-player sessions.
  - Cross-language bindings (C++, Rust, JS) for scripting, Cvar-driven tuning (e.g., `net_dcf_mode "p2p"`), and custom protocols.

- **Additional Core Features**:
  - Full backward compatibility with Doom 3/Quake 4 assets and mods (e.g., The Dark Mod).
  - Hooks for Vulkan backends, physics upgrades (e.g., Bullet integration), and AI enhancements.
  - Lightweight binaries (<50MB) with modular contributions (e.g., DSP plugins).

DTech excels in niche applications like cyberpunk horror or cooperative FPS, offering a balance of performance and extensibility compared to engines like Godot, Unreal, Unity, or O3DE.

## History

DTech originated from id Software's 2011 open-source release of idTech4, which spawned community forks addressing the engine's aging aspects. By merging RBDOOM-3-BFG's rendering innovations (e.g., PBR, GI) with Dhewm3's portability (e.g., SDL2, 64-bit), DTech creates a unified fork for modern development. Developed by DeMoD LLC and Asher LeRoy, it emphasizes OSS collaboration under GPL-3.0, inviting forks and contributions to evolve idTech4 beyond its 2004 roots.

## Installation

### Pre-Built Binaries
1. Download the latest release from [GitHub Releases](https://github.com/demod-llc/DTech/releases) or the [official website](http://demod.ltd/DTech.html).
2. Extract the archive to a directory (e.g., `C:\DTech` on Windows or `~/DTech` on Linux/macOS).
3. Copy your Doom 3/Quake 4 base assets (e.g., `base/` folder with `.pk4` files) into the installation directory.
4. Run the executable:
   - Windows: `DTech.exe +set fs_game mymod`
   - Linux/macOS: `./DTech +set fs_game mymod`

For Steam Deck/Android/iOS, use platform-specific builds from the website.

### Requirements
- **OS**: Windows 10+, Linux (Ubuntu 20.04+), macOS 11+, Android 8+, iOS 14+.
- **Hardware**: Mid-range GPU (e.g., GTX 1050 or equivalent) for 1080p/60 FPS; integrated graphics for low-res ports.
- **Dependencies**: SDL2 (bundled); optional: Vulkan drivers, TMS320C6657 DSP hardware for advanced audio.

## Building from Source

1. **Clone the Repository**:
   ```
   git clone https://github.com/demod-llc/DTech.git
   cd DTech
   ```

2. **Install Dependencies**:
   - CMake 3.10+ for build system.
   - Rust/Cargo for StreamDB (via submodule: `git submodule update --init`).
   - SDL2 development libraries.
   - Optional: Vulkan SDK, Bullet Physics.

3. **Build StreamDB (Rust Component)**:
   ```
   cd neo/streamdb_ffi
   cargo build --release
   ```

4. **Configure and Build**:
   ```
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build --config Release
   ```

5. **Run**:
   ```
   ./build/DTech +set fs_game base
   ```

For detailed platform-specific instructions, see [BUILD.md](BUILD.md).

## Usage

### Running Games/Mods
- Launch with base game: `DTech +map game/mp/dm2`
- Mod override: `DTech +set fs_game mymod +map mymap`
- Enable advanced features: `r_usePBR 1; s_useDSP 1; net_dcf_mode p2p`
- Build assets: `buildSdb mymod mymod.sdb` (packs loose files into `.sdb`).

### Mod Development
- Use TrenchBroom for mapping.
- Edit scripts/entities in `.def`/`.script` files; hot-reload with `reloadDecls`.
- For custom networking: Extend DCF plugins (C++/Rust bindings).
- Test addons: `fs_searchAddons 1; reloadEngine`

See [DOCS.md](DOCS.md) for API details.

## Contributing

We welcome contributions! Fork the repo, create a branch, and submit a pull request. Focus areas:
- Vulkan backend implementation.
- Additional DSP effects.
- StreamDB backend extensions (e.g., S3 for cloud assets).

Follow the [Code of Conduct](CODE_OF_CONDUCT.md). Sponsors unlock priority features (e.g., Vulkan).

## License

DTech is licensed under the GPL-3.0 License. See [LICENSE](LICENSE) for details.

## Acknowledgments

- id Software for open-sourcing idTech4.
- RBDOOM-3-BFG and Dhewm3 teams for upstream inspirations.
- Rust community for crates like `cxx`, `md4`, and `zip`.
- Mod communities (e.g., The Dark Mod) for testing and feedback.
