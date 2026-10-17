# Pip-Boy map

The hub's **MAP** page shows an offline, Fallout-style street map. It is drawn by the terminal holotape from a
map pack on the Pip-Boy SD card (`PIPLINK/MAP/`).
- **Your position:** the player arrow comes from the MeshCore radio's GPS (SELF_INFO lat/lon). The hub asks for it every 15 s while the map is up. The arrow's heading comes from successive fixes 15 m or more apart.
- **Contacts:** contacts with an advertised position are drawn as markers. The icon depends on the contact type: repeater = RADIO, room = TOWN, sensor = INFO, everyone else = PERSON.
- **Saved locations:** KMZ pins plus spots saved or received on the Pip-Boy (see below). SETTINGS > Map locations hides them.
- **Crosshair:** marks the view centre whenever the map isn't following you.

Controls:
- **Click** (knob1) opens the mode menu in the footer: `ZOOM`, `PAN`, `CONTACTS`, `GO TO`, `CENTER ON ME`. **knob2** highlights an item and a click picks it.
- In **ZOOM**, knob1 zooms. (v1.2 turned the wheel direction round for both ZOOM and PAN: in v1.1 people kept zooming out when they meant in.)
- In **PAN**, knob1 moves north/south and knob2 moves east/west, 32 pixels per step - fine enough to put the crosshair on a spot. Click when you're done; the view stays where you left it. Leaving the tab also ends PAN.
  - While panning the hub sets flag 2 on `T.mv`. The holotape then scrolls the picture (`h.scroll`), flips at once (~0.15 s), and only then draws the strip that came into view (~0.3 s), instead of redrawing the screen (~0.9 s). The crosshair's pixels are kept, so it can be lifted off before scrolling. Every marker is redrawn after a scroll. The view is drawn whole when the zoom changes, a NAV line is up, a marker that was on screen drops out of the list, or the move is over 320 × 180 px.
  - Turns made while the Pip-Boy draws add up. The hub sends only the newest view (`renderMap` waits while `pip.queued()`), and the holotape doesn't drop turns while panning, the way it does after other map draws.
- In **CONTACTS**, knob1 steps through located contacts from nearest to farthest, showing distance, bearing and when each was last heard.
- **GO TO** opens the location list. Knob1 moves, click picks, knob2 goes back a step:
  - `NEW` saves the spot under the crosshair: type a name, then pick an icon.
  - `MANUAL ENTRY` asks for `latitude,longitude` in decimal degrees, with `-` for south or west (e.g. `35.8325,-115.4319`). The map centres there and stays put.
  - Then the saved locations: the NAV target first (marked `*`), the rest nearest first. Picking one offers:
    - `GO TO` centres the map on it.
    - `NAV` follows you again and draws a dotted line to it, with the distance (m/km) and azimuth written along the line; the footer shows the same. `STOP NAV` appears in its place while it's the target. The target survives restarts (prefs `nav`).
    - `SHARE` sends `[WPT] <name> @ <lat>,<lon> <ICON>` to a channel or contact you pick. A PIP LINK that receives one adds it to its list (unless it already has it), plays `WPT.WAV` and shows a footer note.
    - `DELETE` (with a confirmation).
- **CENTER ON ME** follows you again.
- With a BLE keyboard: Up/Down or +/- zoom, `P` pans with the arrow keys (Enter = done), `C` contacts, `G` the GO TO list (arrows, Enter, Esc / Left = back), `H` centres on you, and Esc goes back to following you.

## Making maps (PC)

Maps are built on the PC into a **map library**, `%LOCALAPPDATA%\PIP LINK\maps\<name>\`. The setup app's MAPS page and `tools/map_lib.py` share it. A map grows:

- **Adding an area only draws the tiles the map doesn't have yet.** A deeper build of a small area adds detail inside a big overview, and a neighbouring area extends the map. Nothing already built is lost.
- **Several maps can be kept**, for example "Nevada" and "Zion trip". The Pip-Boy holds one at a time, in `PIPLINK/MAP/`.
- **Big areas are built in pieces, middle first.** Zooms up to 12 come in pieces about 150 km across (a zoom-8 tile). Zooms 13 and up come in pieces 4 zooms out from the deepest one: about 60 km for z13, 20 km for z15 and 10 km for z16+. The next piece downloads while the current one draws.
- **A stopped build can be resumed.** RESUME, or `map_lib.py resume`, skips finished pieces and tiles already drawn.
- **UPDATE redraws an area with fresh data.** It fetches new OpenStreetMap data and replaces that area's tiles.
- **Removing an area deletes only the tiles no other area covers.**
- **Labels never cross into tiles drawn at a different time.** That avoids half-labels at the seams between areas.

```
cd tools
python osm_extract.py get us/nevada                                     # optional: offline street data (big maps)
python map_lib.py add --map Nevada --place "Clark County, Nevada" --zooms 10-15
python map_lib.py add --map Nevada --center 35.8325,-115.4319 --radius-km 3 --zooms 10-16 --name Goodsprings
python map_lib.py kmz --map Nevada ..\maps\pins.kmz
python map_lib.py info --map Nevada
python map_preview.py "%LOCALAPPDATA%\PIP LINK\maps\Nevada" --zoom 15        # optional: look at it on the PC
python map_upload.py "%LOCALAPPDATA%\PIP LINK\maps\Nevada\SD card\PIPLINK\MAP"   # USB, Pip-Boy plugged into the PC
```

**Zooms.**
- 8–17 can be built; the default is 10–15.
- z16 is street level, about 1 km across the Pip-Boy screen. z12 is about 15 km and z10 about 60 km.
- Roads appear by zoom:
  - motorways and main roads from z10–11
  - secondary roads from z12
  - tertiary roads from z13
  - residential streets from z14
  - service roads and paths from z15–16

**Street data** is © OpenStreetMap contributors, ODbL.
- **Online (Overpass).** By default it comes from the public Overpass servers, rotating through four mirrors when one is busy. Answers are cached gzipped in `cache\osm`, and the query asks only for what the piece's zooms draw. A lake that encloses a whole piece is fetched too (`is_in`), so the middle of Great Salt Lake isn't blank. The servers can be slow or busy for minutes at a time.
- **Offline data (`osm_extract.py`, OFFLINE DATA tab).** This downloads a whole Geofabrik extract once. Nevada is a 120 MB `.osm.pbf`; a state takes several minutes to import into `cache\extracts\<id>.db`, which holds the features with an R-tree. Any piece inside the extract's bounds is built from it with no network: much faster, and it's how a whole state gets built.

**Terrain contours** (feet) come from AWS Terrain Tiles (Terrarium PNG, no key needed, cached in `cache\dem_cache`).
- The interval is 200 ft at z11–12, 100 ft at z13, 40 ft at z14 and 20 ft at z15–17.
- Every 5th line is a thicker index line, labelled from z14.
- Areas over 2000 km² use one-coarser elevation tiles: z13 data (about 15 m) for zooms up to 15.

**Water.** Lakes made of many ways (multipolygons) are joined into rings, and their islands are cut back out. Antelope Island is land, with its contours.

**Buildings** (z16+) are on by default and can be turned off for big areas.

**PREVIEW** (ADD AREA tab) draws the tiles in view with the current options and the map's KMZ lines, exactly as ADD TO MAP would, without saving anything. It uses the picker's zoom, clamped to the chosen zoom range. Found landmarks show as icons, and the preview is cleared when the area or map changes or a build finishes.

**Pins** (PINS tab):
- The map's pins and landmarks show on the picker as Pip-Boy icons. Landmarks appear from z11; pins have their names shown from z13.
- **Picking pins:** click a pin on the map, or in the list, to pick it. Shift/Ctrl-click in the list picks several.
- **Editing:** change the name, icon or `lat,lon`, then SAVE, or DELETE.
- **NEW PIN** then a click on the map adds one.
- **DUPLICATES** finds pins with the same name within 40 m, or any two within 5 m, and deletes the repeats or selects them so you can look first.
- **SAVE AS KML** writes the map's pins out as a KML.
- **Edits reach the Pip-Boy.** The old version of a changed or deleted pin goes into the `pins_gone` table, exported as `PINS.DEL`. On send or copy, a `-` line is written to `WPT.TXT` for each old version still exactly there (name, icon, position within ~2 m), followed by the new versions. Re-sending changes nothing, and locations made on the Pip-Boy are never touched. Re-adding a KMZ file replaces that file's pins the same way.

**Landmarks** from OSM are added automatically: hospitals, city halls, police/fire, stadiums, universities, airports, campsites, caves, named peaks and historic sites.

**Getting it onto the Pip-Boy:**
- **SEND TO PIP-BOY (USB)** runs at about 5 KB/s, so it suits town-sized maps and small updates. It only sends changed files and can be resumed.
- **COPY TO SD CARD** is for big maps. Put the Pip-Boy's card in a card reader. The app finds the removable drive, copies only the changed files (`PIPLINK/MAP/.sync.json` keeps sha1s), deletes files the map no longer has, and merges the KMZ pins into `PIPLINK/WPT.TXT`.
- **By hand:** the map folder's `SD card\PIPLINK` can be copied to the top of the card (merge folders). This doesn't add the KMZ pins.
- The card should be FAT32 (what the Pip-Boy uses). Bundling keeps a whole-state map to a few thousand files.

**Rough sizes,** with contours:
- A town (z10–16, 4 km): about 3 MB.
- A large county, about 60 km across (z10–15): about 40–60 MB.
- A whole state about 450 × 550 km (z10–15): roughly 1.3 GB and 360,000 tiles. Expect a few hours offline.

## Saved locations (`PIPLINK/WPT.TXT`)

- One `lat,lon,ICON,name` per line (ICON = a marker type name below).
- A line starting with `-` deletes an earlier one with the same name within 30 m.
- PIP LINK only appends to the file (`T.wadd`) and reads it with `T.wload` each time the terminal starts.
- The map pack's `PINS.TXT` (the KMZ pins) is not copied to the card. Instead `map_upload.py` merges it into `WPT.TXT`, adding only pins that aren't there and weren't deleted there.
- The setup app's **SAVE PIP-BOY LOCATIONS AS KML** writes the live list as a KML. The icon is kept in each placemark's description, so the KML can go back in as pins.

## KMZ / KML pins → Fallout markers

Google Earth placemarks become saved locations (above). Lines and polygons are drawn into the tiles as bright dashed overlays.

A pin's marker type is chosen in this order:
1. A description that is exactly a marker type name (KMLs saved by PIP LINK Setup).
1. `tools/marker_styles.json` overrides (copy `marker_styles.example.json`).
2. The icon: Google Earth's `shapes/*` icons map by name. For example `campground` → CAMP, `caution` → DANGER, `hospitals` → MEDICAL, `airports` → AIRPORT. Paddles with stars → QUEST.
3. The pin's name or folder for plain pushpins and paddles: "repeater" or "tower" → RADIO, "vault" → VAULT, "camp" → CAMP, "cave" or "mine" → CAVE, and so on.
4. Otherwise LANDMARK.

Adding a KMZ (`map_lib.py kmz`, or ADD KMZ / KML in the app) logs a table of every pin style it found, what it became and why, so you can see what to override.

Marker set (`tools/map_icons.py`, 16×16 1 bpp): LANDMARK VAULT TOWN CAMP CAVE MILITARY FACTORY RADIO MEDICAL WATER DANGER HOME QUEST TARGET FLAG VEHICLE AIRPORT STORE DINER GAS NATURE INFO PERSON. `python map_icons.py` prints them all.

## Map format (`PIPLINK/MAP/` on the SD card)

The map lives in its own folder. The Pip-Boy's own world map lives in `MAP/NV` and `MAP/F3`, and PIP LINK never touches anything outside `PIPLINK/`. Early builds used `MAP/` directly and deleted the stock map files there; if the DATA tab errors with `MAP/NV/MAPS.JSON`, use Factory Reset on the official upgrade page.

| file | contents |
|---|---|
| `INFO.TXT` | `PBMAP 2`, `zoom <min> <max>`, `center <lat> <lon>`, `bundle 8`, `bucket 13`, `types ...` |
| `ICONS.BIN` | 32 bytes per marker type, 16×16 1 bpp, MSB = left |
| `B/<z>/<bx>/<by>.PBB` | 8×8 tiles in one file (`bx` = x div 8). The header is 64 × (`u32` offset, `u32` length) LE, indexed by `(y mod 8) * 8 + (x mod 8)`, followed by the tiles. Offset 0 means the map doesn't have that tile; length 0 means an empty tile. |
| tile | 256×256 2 bpp slippy tile, stored as 4 strips of 64 rows. Each strip is `u16 LE length` + heatshrink data (window 8, lookahead 6, which is Espruino's setting); length 0 means a blank strip. |
| `M/<x>_<y>.TXT` | landmark markers bucketed by zoom-13 tile: `lat,lon,typeIndex,name` per line |
| `PINS.TXT` | the map's KMZ pins. They are merged into `PIPLINK/WPT.TXT` when sent or copied; the Pip-Boy doesn't read this file. |

**Missing detail.** The holotape knows when the map lacks a tile, as opposed to an empty one. In that case it draws the same spot from 1–2 zooms out, enlarged (`drawImage` `{scale}` with a clip rect). Zooming into a z15 area at z16 shows enlarged z15 tiles instead of a blank screen. With nothing at all, it shows "LESS DETAIL HERE - ZOOM OUT" or "NO MAP TILES HERE".

**Older maps.** `PBMAP 1` (one file per tile, `T/<z>/<x>/<y>.PBT`, empty tiles not written) is still read, but without the fallback.

**PC side.** `map.db` (SQLite) keeps:
- `tiles(z,x,y,gen,n,data)`
- `areas` (rect, zooms, options, finished pieces)
- `markers`, `pins`, `lines` (KMZ overlays)
- `bundles` (the `gen` each `.PBB` was written at, so exporting rewrites only the bundles with newer tiles)

## Terminal protocol additions (v0.6)
- `T.map(1|0)`: switches the grid area to the map viewer and back. Turning it on replies `<<T MAP zmin zmax lat lon>>`, where `zmax < zmin` means no pack is installed.
- `T.mv(lat, lon, zoom, b64)`: centre, zoom, and dynamic markers as `lat,lon,type,heading,label;...`. Type -1 is the player arrow.
- A redraw takes about 1.2 s at z16, so the hub only sends `mv` when something changed.
- Later additions:
  - `T.mv(..., flags)`: flags 1 = crosshair. A dyn entry with type -3 is the NAV line from the player to that point; its label is the distance text.
  - `T.ln(row, b64, attr, icon)`: a marker icon in the row's first two columns.
  - `T.theme(0|1)`: green or amber, using the firmware's palette maths. The Pip-Boy's own palette (`settings.palette`) is restored on exit.
  - `T.wload()` → `<<T WPF b64>>`… `<<T WPF END bytes>>`; `T.wadd(b64)` → `<<T WAD 1|0>>`.

Pip-Boy notes learned here:
- `drawImage` with a 2 bpp image needs `palette: [0,1,2,3]`, or the levels are squashed to 0,0,1,1.
- On Windows the Pip-Boy holds back a REPL line's output until something prints later. The PC tools add a short delayed `print(" ")` to push it out.
