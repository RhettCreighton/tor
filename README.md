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

For complete documentation about the dynhost implementation, see [CLAUDE.md](./CLAUDE.md).

## Licensing

This is a combined work:
- Original Tor code: BSD 3-clause license (see [LICENSE](./LICENSE))
- Dynhost additions: Apache 2.0 license (see [LICENSE.dynhost](./LICENSE.dynhost))

See [NOTICE](./NOTICE) for full attribution.

## Repository

- **This Fork**: https://github.com/RhettCreighton/tor
- **Original Tor**: https://gitlab.torproject.org/tpo/core/tor

## Resources

For information about the original Tor project:
- Home page: https://www.torproject.org/
- Documentation: https://support.torproject.org/ 
