# Local curve sampler API

The Web curve page uses the ESP32's local sampler; the browser does not pause
the target itself. All paths are relative to the device HTTP server.

## Endpoints

| Method | Path | Purpose |
|---|---|---|
| POST | `/api/curve/start` | Start sampling (JSON body) |
| POST | `/api/curve/start-bin` | Start sampling (CBS1 binary body, preferred) |
| POST | `/api/curve/stop` | Stop sampling |
| GET | `/api/curve/status` | Sampler status |
| GET | `/api/curve/frames?since=&max=` | CVF1/CVF2 frame pages |

## Start body

- `trigger`: `periodic`, `breakpoint` or `watchpoint`
- `interval_us`: 1000..200000 (5000 minimum for halt triggers)
- `series` or `groups`: 1..64 series total, up to 8 groups
- series fields: `address` + `size`, or an address expression; optional `name`,
  `sample_every`, `format`
- optional: `halt`, `watchpoints`, `snapshot_after`, `condition`

Limits and validation are enforced in `web/src/api/curveLocal.ts`.

## Frames

`GET /api/curve/frames` returns binary pages (CVF2). A page carries per-series
sample values plus valid/stale flags and timestamps; the Web curve page decodes
it in `curveLocal.ts`.

## Scope

Only firmware-side local sampling is exposed. Browser-side WS-DAP sampling and
UART-trace/backtrace panels were removed.
