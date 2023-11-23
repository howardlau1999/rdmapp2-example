# RDMA++ Examples

This repository contains examples of using [rdmapp2](https://github.com/howardlau1999/rdmapp2) library.

## Build

```bash
git clone --recursive https://github.com/howardlau1999/rdmapp2-example
cd rdmapp2-example
cmake -Bbuild .
cmake --build build
```

## Run

```bash
# Server
./build/rdmapp-helloworld 12345

# Client
./build/rdmapp-helloworld 127.0.0.1 12345
```

You are expected to see the following output:

```
# Client
Serialized qp data size: 14
received header lid=10920 qpn=1328 psn=1 user_data_size=0
Received 6 bytes from server: hello
Sent to server: world
Received mr addr=0x7ff550001b30 length=2048 rkey=187339 from server
Read 6 bytes from server: hello
Received mr addr=0x7ff550002370 length=8 rkey=190423 from server
Fetched and added from server: 42
Compared and swapped from server: 43

# Server
received header lid=10920 qpn=1329 psn=1 user_data_size=0
Serialized qp data size: 14
Sent to client: hello
Received from client: world
Sent mr addr=0x7ff550001b30 length=2048 rkey=187339 to client
Written by client (imm=1): world
Sent mr addr=0x7ff550002370 length=8 rkey=190423 to client
Fetched and added by client: 43
Compared and swapped by client: 4422
```