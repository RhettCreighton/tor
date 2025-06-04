# Tor with Dynamic Onion Host (Dynhost) Feature

This is Rhett Creighton's fork of Tor that includes the **Dynamic Onion Host (dynhost)** feature. 
This feature enables Tor to host onion services internally without binding to any external ports, 
with all service logic embedded directly in the Tor binary itself.

Original Tor protects your privacy on the internet by hiding the connection between
your Internet address and the services you use.

## What Makes This Fork Special?

The **dynhost** feature demonstrates that Tor can be more than just a privacy tool - it can be a platform for hosting services directly within the Tor binary. No external dependencies, no open ports, just pure onion-routed services.

## Quick Start

### Build from source:

```bash
git clone git@github.com:RhettCreighton/tor.git
cd tor
./autogen.sh
./configure
make
```

### Run Tor with dynhost:

```bash
./src/app/tor
```

The dynhost service will automatically start and create an ephemeral .onion address.
Look for this line in the output:

```
[notice] Dynamic onion host ephemeral service created with address: [address].onion
```

### Access the dynhost service:

Using curl through Tor's SOCKS proxy:
```bash
curl --socks5-hostname 127.0.0.1:9050 http://[address].onion/
```

Or use Tor Browser and navigate directly to the .onion address.

## Development

For Tor development documentation, see [doc/HACKING/](./doc/HACKING).
For dynhost implementation details, see [CLAUDE.md](./CLAUDE.md).

## Dynamic Onion Host (Dynhost) Feature

This fork includes the **Dynamic Onion Host (dynhost)** feature, which enables Tor to host onion services internally without binding to any external ports. All service logic is embedded directly in the Tor binary itself.

### What is Dynhost?

Dynhost allows Tor to:
- Host web services accessible only through .onion addresses
- Run without any external web server or port bindings
- Demonstrate fully self-contained onion services

### Quick Start with Dynhost

1. Build and run Tor:
   ```bash
   ./autogen.sh
   ./configure
   make
   ./src/app/tor
   ```

2. Look for the onion address in the logs:
   ```
   [notice] Dynamic onion host ephemeral service created with address: [address].onion
   ```

3. Access the service using Tor Browser or curl:
   ```bash
   curl --socks5-hostname 127.0.0.1:9050 http://[address].onion/
   ```

### Available Demos

- **Main Menu** (`/`) - Shows available demos
- **Time Server** (`/time`) - Displays current time with auto-refresh
- **Calculator** (`/calculator`) - Adds 100 to any number you enter
- **MVC Blog** (`/blog`) - Full-featured blog application with posts and comments

### NEW: MVC Framework

The dynhost feature now includes a Rails-like MVC framework for building web applications:
- Models with validations and relationships
- Controllers with RESTful actions
- Views with template rendering
- Router with URL pattern matching
- In-memory data storage

The blog demo showcases the MVC framework capabilities.

For complete documentation about the dynhost implementation, see [CLAUDE.md](./CLAUDE.md).

## Licensing

This is a dual-licensed project:

### Apache License 2.0 (Primary - New Work)
All new contributions and the Dynamic Onion Host (dynhost) feature are licensed under **Apache License 2.0**.  
See [LICENSE](./LICENSE) for the full text.

This includes:
- All files in `src/feature/dynhost/`
- All modifications for dynhost integration
- Documentation updates (CLAUDE.md, README.md changes)
- Test scripts and new tooling

### BSD 3-Clause License (Original Tor)
The original Tor code remains under **BSD 3-Clause License**.  
See [LICENSE-BSD](./LICENSE-BSD) for the full text.

### Third-Party Components
The `src/ext/` directory contains third-party libraries with their own licenses.  
See [THIRD-PARTY-LICENSES](./THIRD-PARTY-LICENSES) for details.

### Important Notes
- New contributions to this fork should be Apache 2.0
- We respect and maintain BSD 3-Clause for all original Tor code
- Both licenses are permissive and compatible
- See [NOTICE](./NOTICE) for full attribution

## Repository

- **This Fork**: https://github.com/RhettCreighton/tor
- **Original Tor**: https://gitlab.torproject.org/tpo/core/tor

## Resources

For information about the original Tor project:
- Home page: https://www.torproject.org/
- Documentation: https://support.torproject.org/ 
