# ToLiss MCDU External Controller

X-Plane plugin for ToLiss Airbus aircraft (A319/A320/A321/A330) that provides UDP broadcasting and WebUI access to MCDU display data.

## Features

- **UDP Broadcasting**: Real-time MCDU screen data broadcast via UDP for external hardware displays
- **WebUI Access**: Browser-based MCDU display with Server-Sent Events (SSE) for live updates
- **Configurable**: In-sim menu to configure UDP target IP/port and WebUI settings
- **Multi-platform**: Windows, macOS, and Linux support

## Installation

1. Download the latest release for your platform from [Releases](https://github.com/cg8-5712/MCDU-X-Plane-Plugin/releases)
2. Extract the archive
3. Copy the `MCDU-5712` folder to your ToLiss aircraft's `plugins` directory:
   ```
   X-Plane 12/Aircraft/ToLiss A321/plugins/MCDU-5712/
   ```
4. Restart X-Plane

## Usage

### Plugin Menu

Access the plugin menu via **Plugins > ToLiss MCDU**:

- **UDP Target**: Set destination IP and port for UDP broadcast (default: `192.168.1.255:7001`)
- **WebUI**: Toggle WebUI server on/off
- **WebUI Port**: Set HTTP server port (default: `8080`)

### WebUI Access

When WebUI is enabled, open your browser and navigate to:
```
http://localhost:8080
```

The MCDU display will update in real-time as you interact with the aircraft's MCDU.

### UDP Protocol

The plugin broadcasts MCDU screen data as UDP packets (1018 bytes):
- Header: `MC` (2 bytes)
- Reserved: 2 bytes
- Frame counter: 4 bytes (little-endian)
- Cell data: 14 rows × 24 columns × 3 bytes (char, color, font)

## Building from Source

### Prerequisites

- CMake 3.15+
- C++17 compiler (MSVC 2019+, GCC 8+, Clang 7+)
- X-Plane SDK (included as submodule)

### Build Steps

```bash
git clone --recursive https://github.com/cg8-5712/MCDU-X-Plane-Plugin.git
cd MCDU
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Output: `dist/Release/win.xpl` (or `mac.xpl`, `lin.xpl`)

## License

MIT License - see [LICENSE](LICENSE) file for details.

## Credits

- X-Plane SDK by Laminar Research
- cpp-httplib by yhirose
- ToLiss Airbus simulation by ToLiss
