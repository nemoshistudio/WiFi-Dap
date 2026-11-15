# WebSocket binary router

The WebSocket binary path multiplexes module streams over 4-byte headers.

- `WT_WS_ROUTER_MAX_PAYLOAD 1792`
- `WT_WS_MODULE_MAX 6`

Binary frames use the header layout from [protocol.md](protocol.md) §3.
Sub-module `0` is the UART stream; module ids follow the JSON module table.
