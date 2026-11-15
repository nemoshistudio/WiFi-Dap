# WiFi Dap protocol

protocol_version = 1

The device exposes a JSON/WebSocket protocol on port **80** (`/api` for JSON
requests, `/ws` for WebSocket). Binary channels use the 4-byte header in §3.

## 1. JSON requests

A request is `{"module":<u8>,"cmd":<u16>, ...}`. A successful response echoes
`module` and `cmd`; errors are `{"module","cmd","err","code"}`.

模块号：SYSTEM=0、WIFI=1、DATA_FLOW=2、RESERVED=3、UART=4、OTA=5。

### Error codes（沿用 JSON 错误码）

| code | err |
|---|---|
| 2 | `Bad json request` |
| 3 | `Internal error` |
| 4 | `Unsupported cmd` |
| 5 | `Property error` |
| 6 | `Busy` |

## 2. Main endpoints

| Module | Commands |
|---|---|
| SYSTEM | version/info, reset, diagnostics |
| WIFI | STA/AP scan, connect, saved networks, AP credentials |
| UART | text/binary UART bridge |
| OTA | upload status/abort (module 5; requires OTA build) |
| TARGET (HTTP) | `/api/target/*`: detect, info, flash, SWD clock |

Binary UART/raw data use `/ws` binary frames; target flashing uses
`POST /api/target/flash` with an octet-stream body.

OTA upload states: `IDLE`, `RECEIVING`, `VERIFYING`, `READY`, `APPLYING`, `ERROR`.

## 3. 二进制帧协议

Binary frames start with a 4-byte header:

| byte | field | value |
|---|---|---|
| 0 | `data_type` | `EVENT=0x02`、`RAW_BROADCAST=0x04`、`RAW=0x20`、`CMD=0x21`、`RESPONSE=0x22` |
| 1 | `module` | module id (UART=4 …) |
| 2 | `sub_mod` | sub-instance |
| 3 | reserved | 0 |

`RAW`/`RAW_BROADCAST` payloads are UART bytes; `CMD`/`RESPONSE` carry JSON.
