# Virtual cellular switch

`tools/cellular_broker.ps1` is a local TCP switch for exercising the Simon
firmware’s high-level RF-deck interface.  It listens on `127.0.0.1:5555` by
default.  It is deliberately local-only and is not an AMPS base station,
cellular modem, SIP gateway, or audio transport.

## Start and connect

```powershell
.\tools\Start-CellularSwitch.ps1
.\tools\Start-Simon.ps1 -MameRoot D:\src\mame -FirmwareRoot D:\firmware -CellularEndpoint socket.127.0.0.1:5555
```

Start two Simon instances.  The switch assigns `1001`, `1002`, and so on in
connection order.  Wait for each PHONE screen to report service before making
a call.

## Switch console commands

| Command | Effect |
| --- | --- |
| `list` | Show each connected unit, peer, registration state, signal and operator. |
| `set <number> <home1..home4\|roam\|altroam\|offline\|noservice> <0..6> [operator]` | Change a unit’s network profile. |
| `ring <from> <to>` | Trigger an incoming call on `to`, attributed to `from`. |
| `connect <number1> <number2>` | Force a connected event to both ends. Useful for diagnostics. |
| `hang <number>` | End a unit’s current call and notify its peer. |
| `clear` or `cls` | Clear the console without changing client/call state. |
| `quit` | Stop the switch. |

## Tested call sequence

1. Caller enters the target’s virtual number and chooses **Send**.
2. The switch sends a ringing event to the target.
3. The target uses the original **Answer** or **End** UI.
4. On Answer, both instances enter the original `Phone In Use` state and the
   call timer advances.
5. Either side can use **Mute**, **Unmute**, or **End**.  The switch propagates
   call end to the peer.

The RF-byte trace is intentionally disabled by default to prevent console I/O
from stalling the telephone UI.  Start `Start-CellularSwitch.ps1 -TraceRF`
when investigating protocol flow.

## Host line protocol

The MAME driver and switch exchange newline-delimited ASCII records over the
MAME `-bitb socket.127.0.0.1:5555` endpoint.

| Direction | Record | Meaning |
| --- | --- | --- |
| Simon → switch | `N` | RF deck is ready; request profile. |
| Simon → switch | `P …` | Profile acknowledgement. |
| Simon → switch | `O <number>` | Originate call. |
| Simon → switch | `A` | Answer an incoming call. |
| Simon → switch | `H` | Hang up/reject. |
| Simon → switch | `T XX` | Diagnostic RF byte. |
| switch → Simon | `S <registration> <0..6> [operator]` | Network profile. |
| switch → Simon | `R <caller>` | Incoming call. |
| switch → Simon | `C` | Call connected. |
| switch → Simon | `H` | Call ended. |
| switch → Simon | `I XX` | Inject a raw RF byte for diagnostics. |

The firmware’s original UI does not expose arbitrary operator text.  The
operator is preserved in the switch profile and `list` output while the Simon
screen uses its native Home/Roam and signal indicators.
