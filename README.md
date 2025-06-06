﻿
# Workshop-in-communication
# RDMA Key-Value Store

This project implements a key-value store using RDMA (Remote Direct Memory Access) and the Verbs API. The system supports basic `SET` and `GET` operations with an eager protocol for low-latency data exchange.

## Features
- Efficient key-value storage over RDMA.
- Eager protocol for small messages.
- Server-client communication for distributed operation.

## Requirements
- InfiniBand hardware with RDMA support.
- `libibverbs` and related RDMA libraries installed.
- Linux environment.

## Installation
Ensure RDMA libraries are installed:
```sh
sudo apt-get install rdma-core ibverbs-utils
```
Clone the repository and compile the code:
```sh
git clone <repository_url>
cd rdma-kvstore
make
```

## Usage
### Start the RDMA Server
```sh
./rdma_server -d <device_name> -p <port>
```

### Start the RDMA Client
```sh
./rdma_client -d <device_name> -p <port> -s <server_address>
```

### API
- **`int kv_set(void *kv_handle, const char *key, const char *value);`**
  - Stores a key-value pair.
- **`int kv_get(void *kv_handle, const char *key, char **value);`**
  - Retrieves a value based on a key.
- **`int kv_close(void *kv_handle);`**
  - Closes the RDMA connection.

## License
This project is licensed under the MIT License.

## Contributions
Pull requests are welcome. Please ensure code is well-documented and follows project structure.

