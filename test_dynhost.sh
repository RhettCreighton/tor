#!/bin/bash
# Simple test script for dynhost feature

echo "=== Tor Dynhost Feature Test ==="
echo

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m' # No Color

# Check if tor binary exists
if [ ! -f "./src/app/tor" ]; then
    echo -e "${RED}Error: Tor binary not found. Run 'make' first.${NC}"
    exit 1
fi

# Function to test endpoint
test_endpoint() {
    local endpoint=$1
    local onion_addr=$2
    echo -n "Testing $endpoint... "
    
    if curl -s --socks5-hostname 127.0.0.1:9050 "http://${onion_addr}.onion${endpoint}" > /dev/null 2>&1; then
        echo -e "${GREEN}OK${NC}"
        return 0
    else
        echo -e "${RED}FAILED${NC}"
        return 1
    fi
}

# Start Tor in background
echo "Starting Tor..."
./src/app/tor --SocksPort 9050 --ControlPort 9051 --DataDirectory /tmp/tor-dynhost-test --Log "notice stdout" > tor.log 2>&1 &
TOR_PID=$!

# Wait for Tor to start and find onion address
echo "Waiting for Tor to bootstrap and create dynhost service..."
COUNTER=0
while [ $COUNTER -lt 30 ]; do
    if grep -q "Dynamic onion host ephemeral service created with address" tor.log 2>/dev/null; then
        ONION_ADDR=$(grep "Dynamic onion host ephemeral service created with address" tor.log | tail -1 | grep -oE "[a-z0-9]{56}")
        if [ ! -z "$ONION_ADDR" ]; then
            echo -e "${GREEN}Found onion address: ${ONION_ADDR}.onion${NC}"
            break
        fi
    fi
    sleep 1
    COUNTER=$((COUNTER + 1))
done

if [ -z "$ONION_ADDR" ]; then
    echo -e "${RED}Failed to find onion address after 30 seconds${NC}"
    kill $TOR_PID 2>/dev/null
    exit 1
fi

echo
echo "Running tests..."

# Test endpoints
test_endpoint "/" "$ONION_ADDR"
test_endpoint "/time" "$ONION_ADDR"
test_endpoint "/calculator" "$ONION_ADDR"
test_endpoint "/blog" "$ONION_ADDR"
test_endpoint "/blog/new" "$ONION_ADDR"

# Test POST to calculator
echo -n "Testing calculator POST... "
RESULT=$(curl -s --socks5-hostname 127.0.0.1:9050 -X POST -d "number=42" "http://${ONION_ADDR}.onion/calculator" 2>/dev/null | grep -o "100 + 42 = 142")
if [ "$RESULT" = "100 + 42 = 142" ]; then
    echo -e "${GREEN}OK${NC}"
else
    echo -e "${RED}FAILED${NC}"
fi

# Test XSS protection
echo -n "Testing XSS protection... "
curl -s --socks5-hostname 127.0.0.1:9050 -X POST \
    -d "title=Test<script>alert('XSS')</script>&author=Test&content=Test" \
    "http://${ONION_ADDR}.onion/blog/create" > /dev/null 2>&1
BLOG_CONTENT=$(curl -s --socks5-hostname 127.0.0.1:9050 "http://${ONION_ADDR}.onion/blog" 2>/dev/null)
if echo "$BLOG_CONTENT" | grep -q "&lt;script&gt;"; then
    echo -e "${GREEN}OK (XSS prevented)${NC}"
else
    echo -e "${RED}FAILED (XSS not prevented)${NC}"
fi

echo
echo "Cleaning up..."
kill $TOR_PID 2>/dev/null
rm -rf /tmp/tor-dynhost-test
rm -f tor.log

echo -e "${GREEN}Test complete!${NC}"