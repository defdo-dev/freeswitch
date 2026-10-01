# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

FreeSWITCH is a Software Defined Telecom Stack enabling the digital transformation from proprietary telecom switches to a versatile software implementation. This is a large C/C++ codebase with an extensive modular architecture.

## Build System

### Primary Build Commands
- `./bootstrap.sh` - Initialize autotools and bootstrap the build system
- `./configure` - Configure the build (use `./configure --help` for options)
- `make` - Build the core FreeSWITCH library and configured modules
- `make install` - Install FreeSWITCH to the configured prefix
- `make modules` - Build only the modules
- `make core` - Build only the core library

### Module Management
- Module configuration is in `build/modules.conf.in` (copied to `modules.conf` on first build)
- Modules are enabled/disabled by commenting/uncommenting lines in `modules.conf`
- Use `make mod_<name>` to build a specific module
- Use `make mod_<name>-install` to install a specific module

### Useful Build Targets
- `make clean` - Clean build artifacts
- `make update` - Pull latest changes from git (requires git repo)
- `make current` - Update, build, and reinstall everything
- `make reconf` - Reconfigure after changes to configure.ac

## Testing

### Unit Tests
- Tests are located in `tests/unit/`
- Run tests with: `make -C tests/unit run-tests.sh`
- The test runner supports chunking for parallel execution: `./run-tests.sh [chunks] [chunk_number]`
- Individual tests can be listed with: `make print_tests`

### Module Tests
- Many modules include their own test directories
- Module-specific tests are integrated into the main test framework

## Architecture

### Core Components
- **src/** - Core FreeSWITCH library source code
- **src/include/** - Header files defining the core API
- **src/mod/** - All loadable modules organized by category

### Module Categories
- **applications/** - Call control and application modules (commands, conference, voicemail, etc.)
- **codecs/** - Audio/video codec implementations (G.729, Opus, AMR, etc.)
- **endpoints/** - Protocol endpoints (SIP/Sofia, Loopback, RTMP, etc.)
- **event_handlers/** - Event processing modules (CDR, event socket, etc.)
- **formats/** - File format handlers (WAV, PNG, stream formats, etc.)
- **languages/** - Scripting language interfaces (Lua, Python, etc.)
- **loggers/** - Logging implementations (console, file, syslog, etc.)
- **say/** - Text-to-speech modules for different languages
- **xml_int/** - XML interface modules (CURL, RPC, etc.)

### Key Libraries
- **libs/apr/** - Apache Portable Runtime (APR) library
- **libs/esl/** - Event Socket Library for external communication
- **libs/srtp/** - Secure RTP implementation
- **libs/libvpx/** - VP8/VP9 video codec library
- **libs/libyuv/** - YUV video processing library

### Configuration
- **conf/** - Sample configurations for different deployment scenarios
  - `vanilla/` - Default configuration with common modules
  - `minimal/` - Minimal configuration for testing
  - `sbc/` - Session Border Controller configuration

## Development Workflow

### Module Development
- Use `src/mod/applications/mod_skel` as a template for new modules
- Modules must be added to `build/modules.conf.in` to be built
- Each module has its own Makefile.am for build configuration

### Core Development
- Core functionality is in `src/switch_*.c` files
- Headers in `src/include/switch_*.h` define the public API
- Key entry points: `src/switch.c` (main), `src/switch_core.c` (core functions)

### Dependencies
- Check existing modules before adding new dependencies
- Libraries are typically included in `libs/` directory
- Use autotools macros in `configure.ac` for optional dependencies

## Debugging

### Core Dumps
Enable core dumps for debugging crashes:
```bash
sysctl -w kernel.core_pattern=/tmp/core.%t_%e_s%s
sysctl -w fs.suid_dumpable=1
ulimit -c unlimited
freeswitch -core
```

### Debugging Tools
- Use `gdb` with core files or running processes
- Debug scripts available in `debian/scripts/`
- Valgrind and other memory debugging tools are supported

## Erlang Integration (mod_erlang_event)

### Enhanced OTP 25+ Support
The Erlang event handler has been enhanced with adaptive protocol support:
- **Adaptive Protocol Negotiation**: Automatically detects local ei library capabilities
- **Legacy Fallback**: Gracefully falls back to compatibility mode for older ei libraries
- **Runtime Detection**: Uses dlsym() to detect available ei functions at runtime
- **No Undefined Symbols**: Safe compilation regardless of ei library version

### Configuration Options
- `adaptive-protocol`: Enable automatic protocol detection (default: true)
- `force-compat-mode`: Force legacy mode for all connections (default: false)
- `protocol-timeout`: Timeout for protocol detection in ms (default: 5000)
- `debug-protocol`: Enable detailed protocol logging (default: false)

### OTP Version Support
- **Legacy ei libraries**: Automatically detected and uses standard ei_x_encode_atom()
- **Modern ei libraries**: Uses ei_x_encode_atom_utf8() when available for better UTF-8 support
- **Mixed Environments**: Runtime detection ensures compatibility across different ei versions

### Technical Implementation
- **Dynamic Symbol Resolution**: Uses dlsym() to check for ei_x_encode_atom_utf8 availability
- **Function Pointer Caching**: One-time capability detection with cached results
- **Graceful Degradation**: Falls back to standard atom encoding when UTF-8 encoding unavailable
- **Comprehensive Logging**: Reports detected capabilities for troubleshooting

### Troubleshooting
If you encounter "undefined symbol: ei_x_encode_atom_utf8" errors:
1. The enhanced module now detects this automatically and uses fallbacks
2. Check logs for capability detection messages
3. Use `force-compat-mode=true` if needed for problematic environments

## Important Notes

- This is a telecom/VoIP system with security implications - be careful with network-facing code
- The modular architecture means most functionality can be enabled/disabled at build time
- Configuration files use XML format extensively
- The codebase supports multiple platforms (Linux, Windows, macOS, BSD)
- Threading and real-time audio processing require careful consideration of performance
- Erlang integration now supports modern OTP versions through adaptive protocol negotiation