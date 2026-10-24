**English** · [Polski](../pl/dmx-and-patch.md) · [Docs home](../../README.md#documentation)

# DMX and patch

## Receiving DMX

Scene Visualiser listens on every network interface:

| Protocol | Port | Notes |
|---|---|---|
| Art-Net 4 | UDP 6454 | ArtDmx, ArtSync and ArtPoll (the app replies, so consoles can discover it) |
| sACN E1.31 | UDP 5568 | multicast; priority, per-address priority (0xDD) and sync |

**Universe numbering:** universe U in the app is sACN universe U, and Art-Net Port-Address U−1. So universe 1 is
Art-Net 0:0:0 and sACN 1.

When several sources send to the same universe, the merge rule decides (set per universe):
- **Priority (default):** the highest priority wins. On a tie, intensity merges HTP and everything else LTP.
- **HTP** or **LTP**.

A source that stops sending times out after 3 s (Art-Net) or 2.5 s (sACN).

On Windows, the installer adds a firewall rule for private networks. On a public network, allow the app in the
firewall yourself.

## Patch screen

![Patch screen](../images/patch.png)

- Every fixture with its ID, type, mode, address (`universe.address`), footprint and position.
- The bar at the top shows universe occupancy: used, conflicting and selected channels. Conflicts and overflows are
  marked in the table.
- **Filter** narrows the list, and **Select shown** selects what is left.
- **Re-address:** select fixtures, type a start address (`universe.address`, for example `2.1`) and click
  **Re-address**. Fixtures are packed in fixture-ID order and roll over to the next universe when full.
- **Locate** sets the selection to full white so you can find it in the rig.

## DMX monitor

![DMX monitor](../images/dmx.png)

A live grid of all 512 channels of one universe. The left column lists every universe, with its rate and its lost and
dropped packets.

## Control screen (programmer)

![Control screen](../images/control.png)

Run the rig without a console:
1. Pick fixtures, or a group or fixture type, on the left.
2. Use **Full**, **Out** and **Locate**, or move the faders: intensity and strobe, colour (with swatches), pan, tilt and
   zoom. For multi-head fixtures, the **HEAD** chips choose one head. The **×** next to a fader releases it.

- The values are held on top of incoming DMX (the title bar shows **PROG**).
- **Release selection** and **Release all** give control back.
- **GROUPS:** type a name and click **Save** to store the selection.
- **LOOKS:** **Save** stores everything currently held. Click a look to recall it.

## Sending DMX out

**SIGNAL OUTPUT** on the Project screen sends the final result (input, playback and programmer):
- **Send DMX** over Art-Net or sACN to a target address (empty means broadcast or multicast).
- **Universe +** offsets the output universes.

Use it to drive the real rig from a recording or the programmer. **Send Resolume outputs as NDI** publishes each output
feed as an NDI source called `SV <output>`.

## Test source

There is no console needed to try it. Start the app with `--generator rainbow|chase|sweep|full` for an internal
pattern, or run `python3 tools/dmx_send.py --proto sacn --universes 1-4 --pattern chase` from a checkout.

Next: [Video and mapping](video-and-mapping.md)
