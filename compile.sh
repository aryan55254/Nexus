#!/bin/bash
echo "Compiling websocket_server.cpp..."

# -o server: Specifies the output executable name
# -lssl: Links the OpenSSL SSL library
# -lcrypto: Links the OpenSSL crypto library
# -std=c++11: Ensures C++11 features are enabled
g++ websocket.server.cpp -o server -lssl -lcrypto -std=c++11

if [ $? -eq 0 ]; then
    echo "Compilation successful!"
    echo "Run the server with: ./server"
    echo "Then, open test.html in your browser."
else
    echo "Compilation failed. Please check the errors."
    echo "Make sure you have libssl-dev installed (e.g., sudo apt-get install libssl-dev)"
fi