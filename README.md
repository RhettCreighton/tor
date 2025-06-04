Tor protects your privacy on the internet by hiding the connection between
your Internet address and the services you use. We believe Tor is reasonably
secure, but please ensure you read the instructions and configure it properly.

## Build

To build Tor from source:

```
./configure
make
make install
```

To build Tor from a just-cloned git repository:

```
./autogen.sh
./configure
make
make install
```

## Releases

The tarballs, checksums and signatures can be found here: https://dist.torproject.org

- Checksum: `<tarball-name>.sha256sum`
- Signatures: `<tarball-name>.sha256sum.asc`

### Schedule

You can find our release schedule here:

- https://gitlab.torproject.org/tpo/core/team/-/wikis/NetworkTeam/CoreTorReleases

### Keys that CAN sign a release

The following keys are the maintainers of this repository. One or many of
these keys can sign the releases, do NOT expect them all:

- Alexander Færøy:
    [514102454D0A87DB0767A1EBBE6A0531C18A9179](https://keys.openpgp.org/vks/v1/by-fingerprint/1C1BC007A9F607AA8152C040BEA7B180B1491921)
- David Goulet:
    [B74417EDDF22AC9F9E90F49142E86A2A11F48D36](https://keys.openpgp.org/vks/v1/by-fingerprint/B74417EDDF22AC9F9E90F49142E86A2A11F48D36)
- Nick Mathewson:
    [2133BC600AB133E1D826D173FE43009C4607B1FB](https://keys.openpgp.org/vks/v1/by-fingerprint/2133BC600AB133E1D826D173FE43009C4607B1FB)

## Development

See our hacking documentation in [doc/HACKING/](./doc/HACKING).

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

## Resources

Home page:

- https://www.torproject.org/

Download new versions:

- https://www.torproject.org/download/tor

How to verify Tor source:

- https://support.torproject.org/little-t-tor/

Documentation and Frequently Asked Questions:

- https://support.torproject.org/

How to run a Tor relay:

- https://community.torproject.org/relay/ 
