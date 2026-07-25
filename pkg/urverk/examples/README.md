# Example urverk services

These demonstrate the service types that are not used by the base system. They
are **not installed** — a default VerkOS should not run a timer that writes every
15 seconds, and certainly should not listen on a port nobody asked for.

To try one, copy it into `/etc/urverk/services/` on a running system and restart,
or add it to `pkg/urverk/services/` before building an image.

| file | shows |
|------|-------|
| `50-tick.svc`     | `type timer` with `every 15s` — the cron replacement |
| `60-ondemand.svc` | `listen ADDR:PORT` — the service starts on first connection |

See `pkg/urverk/services/` for the real ones (vdhcp, host keys, sshd, console).
