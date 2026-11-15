# Versioning

## Sources

| Version | Source |
|---|---|
| Firmware | `version.txt` / git describe via `cmake/version.cmake` |
| Web UI | `web/package.json` |
| Protocol | `project_components/wt_common/wt_protocol_version.h` |
| Target flash image | `firmware.bin`/ELF version is owned by the target firmware |

## Rules

- Firmware and Web UI versions must match in a release build.
- The protocol version is frozen until a breaking wire change; bump it in the
  header, documentation and Web client together.
- The device reports firmware/protocol versions through `SYSTEM/GET_FM_INFO` and
  the Web UI checks them on connect.

## Checks

```bash
bash scripts/check-protocol-version.sh
bash scripts/check-version-string.sh
```
