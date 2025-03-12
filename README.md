# Asynchronous Web Server

## Overview
This project implements an asynchronous web server in C using advanced I/O operations on Linux. The server is designed to efficiently handle multiple client requests using non-blocking sockets, zero-copy mechanisms, and asynchronous file operations.

## Features
- **Multiplexing I/O Operations:** Uses `epoll` for efficient connection handling.
- **Zero-Copy Transmission:** Static files are served using `sendfile` to minimize CPU overhead.
- **Asynchronous File Handling:** Dynamic files are read asynchronously before being sent to the client.
- **Non-Blocking Sockets:** Ensures scalability and responsiveness under high loads.
- **Minimal HTTP Protocol Implementation:** Supports GET requests with proper response headers.

## Installation & Compilation
### Prerequisites
- Linux-based OS
- GCC compiler
- `make` utility

### Steps to Build
```sh
make
```

## Usage
1. Build the server:
   ```sh
   make
   ```
2. Run the server:
   ```sh
   ./aws_server
   ```
3. Access the server via a web browser or use `curl`: (index.html is offered to test the server)
   ```sh
   curl http://localhost:8888/static/index.html
   ```

## Request Handling
- Requests for files under `static/` are served using zero-copy (`sendfile`).
- Requests for files under `dynamic/` are read asynchronously and sent using non-blocking sockets.
- Non-existent files return an HTTP 404 response.

## API Details
- Uses `epoll` for efficient event-driven connection handling.
- Non-blocking socket operations ensure high performance under concurrent loads.
- Static files are served using `sendfile` for efficient file transfer.
- Asynchronous file operations are performed using the Linux AIO API.
