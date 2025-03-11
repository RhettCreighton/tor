# Built-in Web Service for Tor Hidden Services

This feature allows Tor to serve content directly from within the Tor process without requiring an external web server. It's useful for simple static content where setting up a separate web server would be overkill.

## How It Works

When a client connects to a hidden service with a virtual port that has a built-in handler registered, Tor will:
1. Intercept the connection before it attempts to connect to a real port
2. Send a response directly from the Tor process
3. Close the connection properly

This happens entirely within the Tor process - no actual network connections are made.

## Implementation Details

The implementation consists of several components:

1. **Registration System**: Built-in service handlers are registered with specific virtual ports
2. **Connection Interception**: In `connection_exit_connect()`, connections to handlers are intercepted
3. **Direct Response**: Response data is sent directly through the Tor circuit
4. **Clean Cleanup**: Connections are properly closed

## Configuration

To use the built-in web service, configure your hidden service normally but point the virtual ports to any non-zero local port:

```
HiddenServiceDir /path/to/hidden_service
HiddenServicePort 80 127.0.0.1:1
```

The built-in handlers will automatically be used for connections to these ports.

## Default Handlers

By default, Tor includes a simple "Hello World" handler for ports 80 and 443. This handler returns a basic HTML page informing the user that the page is served directly from Tor.

## Test Configuration

To test the built-in web service, use the included test configuration:

```
./src/app/tor -f test-onion-builtin.torrc
```

This will start a Tor instance with a hidden service using the built-in handlers for ports 80 and 443.

## Extension Possibilities

The system is designed to be extensible. Future versions could add:

1. More handler types for different content types
2. Configuration options for handlers
3. User-customizable content
4. Support for more complex request handling