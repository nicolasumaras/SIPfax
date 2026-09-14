# Caller-side V.34 selection for the fallback test

Use only after the active endurance call has completed and the notebook is idle.
The observed notebook driver identifies as **SoftV90 Data Fax Modem with SmartCP**.
Its exact command acceptance has not yet been queried; the command below is a
manufacturer-documented candidate for the SmartCP family, not a verified setting
on this HP notebook.

Fujitsu's [AC97 Soft Data Fax Modem with SmartCP manual, page44](https://www.fmworld.net/biz/fmv/support/fmvmanual/0510-0603/pdf/B6FH6601.pdf#page=44)
documents `AT+MS=V34,0,2400,33600,2400,33600`: V.34-only modulation with
2400–33600bit/s allowed in each direction. The second parameter0 disables
automode. HP also documents using a modulation command in modem initialization,
but [its older Pavilion guide](https://www.hp.com/ctg/Manual/bpi04347.pdf#page=62)
uses numeric mode identifiers for a different generation; do not substitute
that older numeric example for the SmartCP syntax.

For a temporary Windows XP test:

1. Open Control Panel → Phone and Modem Options → Modems. Select the observed
   modem, open Properties → Advanced, and save the exact existing extra
   initialization-command text so it can be restored afterward.
2. The candidate extra initialization command is
   `+MS=V34,0,2400,33600,2400,33600`. Preserve unrelated existing commands;
   replace an existing `+MS` setting rather than leaving conflicting copies.
   If a direct modem terminal is available, `AT+MS=?` and `AT+MS?` can check
   supported parameters and the active setting before dialing. These query forms
   are documented in the [Conexant command reference, section3.2.4](https://www.thinkpenguin.com/files/CX930xx-manual.pdf#page=79); that reference also covers a different modem implementation. Do not open the
   modem port while DialUpLab owns an active call.
3. Keep the server offering V.90 and V.34 with the tested CMA/LAPM profile.
   The server must not set `SIPFAX_LINMODEM_MAX=v34` for this test.
4. Verify the caller's command is accepted and the server's negotiation trace
   actually selects V.34. Then require PPP/IPCP, expected HTTP payload hashes,
   modem counters, and clean teardown. A reported low bit rate alone does not
   prove V.34 selection or a correctly applied caller command.
5. Restore the saved extra initialization text, then verify an ordinary V.90
   call. No saved modem profile write (`AT&W`) is required for this test.

Changing a Windows serial-port maximum speed is not a substitute for limiting
line modulation. This procedure tests a caller requesting V.34 against a server
that offers both modes; it does not prove an established V.90 call can transition
to V.34 without a new connection.
