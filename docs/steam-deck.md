# Steam Deck

## Recommended setup

1. **Install the Linux build**, not the Windows build through Proton ([installer.md](installer.md), "Linux release").
   Install in Desktop Mode; for Game Mode, add `xcx` from the install folder to Steam as a non-Steam game.
2. **Turn on two settings.** Open the settings menu (Minus and Plus together), go to the *Advanced* tab, turn on
   *GPU frame prep on its own thread*, then the row under it, *Snapshot GPU data at submit*, and restart the game.
   Or put these lines in `xcx.ini` beside `xcx`:

   ```ini
   [performance]
   replay_async = 1
   mirror_cow = 1
   ```

3. **Leave the rest to the Deck's device profile.** The game recognizes the Deck (LCD and OLED) and uses its profile
   for every setting `xcx.ini` does not set: fullscreen, game resolution 1280x720 letterboxed in the 1280x800 screen,
   the game's own 3D scale, vsync and a 40 fps limit.

With these two settings the Deck holds its 40 fps limit in the open field and in New Los Angeles. Without them it runs
at about 37 and 33 fps, unevenly against the limit. They cost about 0.5 to 1 W of APU power and add up to one frame of
input latency (25 ms at 40 fps).

For a longer battery, `fps_limit = 30` holds in both places (measured with the two settings off) at about 10.4 W,
1 to 1.5 W less than 40 with them on.

## What we measured

On 2026-11-27 and 2026-11-28 on a Steam Deck OLED (SteamOS 3.9.5, build 20261023.100; Mesa 26.2 RADV) with commit
2fafc91 built as the release builds it, no mods and no `xcx.ini`: every setting is the device profile's except the
ones named. The LCD model is not measured yet.

Two places in clear daylight: an open field in Primordia and New Los Angeles' commercial district. Each value is the
mean of three boots with 45 s of frames from each, after a warm-up boot for the shader caches. p99 is the 99th
percentile frame time, and power is the APU's package power (amdgpu's hwmon, the same value the overlay shows on
Linux).

### The two settings

`replay_async` moves the preparation of each frame for the GPU off the game's thread onto its own, at most one frame
behind. `mirror_cow` keeps each frame's vertex and storage data as it was when the game submitted it, so models do not
stretch into spikes at the start of camera pans; it only works with `replay_async`. Both are off by default.

| Place, `fps_limit` | Both off | Both on |
|---|---|---|
| Field, 40 (the profile) | 37.6 fps, p99 34 ms, 11.1 W | 40.0 fps, p99 32 ms, 11.6 W |
| New Los Angeles, 40 | 33.3 fps, p99 38 ms, 10.8 W | 39.9 fps, p99 32 ms, 11.9 W |
| Field, 60 | 37.3 fps, p99 34 ms, 11.0 W | 51.7 fps, p99 26 ms, 13.0 W |
| New Los Angeles, 60 | 33.2 fps, p99 38 ms, 10.9 W | 45.2 fps, p99 29 ms, 12.5 W |

Both on was faster in all 12 pairs of boots. With the limit at 60 they show how much headroom they add: 39% in the open
and 36% in the city. At 40 the limit caps the gain at 6% and 20%.

A long session with both on, at the profile's 40 fps limit: 25 minutes in each place with the party walking out and
back and the camera panning the whole time, which took it into a fight, through dusk into night and onto the city's
raised walkways. The frame rate stayed at 40.0 from the first minute to the last, p99 at 31 to 33 ms, APU power at
about 10 W. In 50 minutes one frame took longer than 100 ms (206 ms, early in the field while new ground streamed in),
the log shows no device fault, crash or hang, and the frames captured along the way show no stretched or missing
geometry.

### The device profile's settings, both off

| Setting | Field | New Los Angeles |
|---|---|---|
| `fps_limit` 30 | 30.0 fps, p99 41 ms, 10.3 W | 29.9 fps, p99 41 ms, 10.4 W |
| `fps_limit` 40 (the profile) | 37.5 fps, p99 34 ms, 11.1 W | 33.4 fps, p99 38 ms, 10.9 W |
| `fps_limit` 45 / 60 | 37.4 / 37.4 fps | 33.0 / 33.3 fps |
| `render_scale` 50 / 75 / 100, `fps_limit` 60 | 41.0 / 39.5 / 37.0 fps; 10.5 / 10.7 / 11.0 W | 34.4 / 34.2 / 33.2 fps; 10.3 / 10.6 / 10.8 W |
| `crowd_distance` 4, `fps_limit` 60 | | 26.8 fps, p99 47 ms |

### What this says about the Deck

- **The CPU's work per frame is the limit, not the GPU.** With both settings off, no limit above 40 changes the frame
  rate. A lower `render_scale` gains a few frames in the open and about one in the city, and saves about 0.5 W.
- **Splitting that work over two threads is what helps.** With frame preparation on its own thread, the game's thread
  has time to spare at 40 fps in both places.
- **Crowds cost the most in the city.** `crowd_distance` 4 takes New Los Angeles from 33 to 27 fps with both settings
  off. With them on it is not measured yet; the menu advises turning on frame prep before raising it.
- **30 fps holds everywhere** we measured, at about 10.4 W with the settings off.
- **Proton does not reach the game.** The Windows build of the same commit, through Proton 10.0 and 11.0 in the Steam
  Linux Runtime, failed 8 boots out of 8 before the game world ([installer.md](installer.md), "Linux release"); the
  Linux build reached it every time.

## Reports that still help

1. The performance overlay (L3 + R3, both stick clicks) in full mode, with the two settings on: the frame rate, mean
   and longest frame time over a minute in other busy places (a big fight, Noctilum's forests, Oblivia), with the
   settings named.
2. A long session on battery: the frame rate and temperatures after half an hour, and the battery drain.
3. An LCD Deck's numbers in the places above.
4. LCD or OLED, SteamOS version (`uname -r` in Desktop Mode's terminal), and the game update.
5. Anything that looks stretched or cut off: the game draws 16:9 only, so the profile keeps black bars above and
   below; `picture_fit = stretch` fills the screen instead.

`device_profile = desktop` in `xcx.ini` turns the profile off; `deck` turns it on elsewhere, to try the Deck's
settings on a PC.
