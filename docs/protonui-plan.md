# ProtonUI v2 and Weave - the plan

This is a work plan for whoever builds it (a person or an AI assistant),
written on 2026-11-19 from the design in [WEAVE.md](WEAVE.md) and its
mock-ups in `docs/weave/canvas/` - above all `ProtonUI.dc.html`, the
toolkit's technical page - and from a read of the code at that date.
Read [AGENTS.md](../AGENTS.md) first: every rule there applies, above
all "a test that would fail without the feature" and "never weaken a
test".

Weave is ThreadOS's own look, a third desktop style beside the bar and
dock and the taskbar. ProtonUI is the toolkit programs draw their
windows with; v2 is the version Weave needs. They are built together:
the toolkit gives programs the look, the desktop gives the look to
everything else.

The estimates are working days at the pace of the TFS4 stages, within
half again either way; every stage also waits for its test groups and,
before it is committed, every group that clicks on or reads the windows
it touches (`t_gcode`, `t_desktop`, `t_arm64win` and the rest).

## Where things are today

| What | Where |
|---|---|
| ProtonUI v1: shapes and text sent to the desktop | `user/udraw.h` (228 lines), `UD_OP_*` 1-14 |
| ProtonUI v1: controls as a list of things | `user/ugui.h` (1359 lines), `UG_MAX` 24 |
| The draw commands, PC | `kernel/desktop.c` `desktop_win_draw` (`WD_*` in `kernel/include/desktop.h`) |
| The draw commands, phone | `kernelarm64/winmob.c` (the same `WD_*`) |
| The theme a program reads | `SYS64_UI_LOOK` (69): eleven words, `struct ud_look` |
| The desktop's sizes and colours | `kernel/include/look.h` (plain numbers the tests read) |
| The desktop styles | the `layout` setting (`kernel/config.c`): `mac`, `windows` |
| The wallpaper | `kernel/aurora.c` |
| The phone's shell and navigation | `kernelarm64/mobile.c` (`nav_buttons`), `mobapps.c` |
| Notifications, now playing | `SYS64_NOTIFY` (70), `SYS64_NOW_PLAYING` (115) |

## Before anything: the fonts

WEAVE.md draws Inter and JetBrains Mono. Both are under the SIL Open
Font Licence, which AGENTS.md rule 6 allows, with an entry in
`third_party/README.md` and `scripts/mknotices.py`.

- **Done** for Inter (journal: "Weave as its design draws it"): Inter
  4.1's regular and bold, cut by `scripts/mkfonts.py` like DejaVu, are
  the desktop's letters while it is in Weave (`fonts_use_inter`);
  `t_ttf` holds them to FreeType. JetBrains Mono is still to come: the
  terminal measures its cells by its font, and changing that is a step
  of its own.

## Stage 1 - the look grows - half a day

- `struct ud_look` gets the v2 fields at its end: the style
  (`PU_BAR_DOCK`, `PU_TASKBAR`, `PU_WEAVE`), the six dyes of the theme
  in use, the person's thread colour, the grain's ink, the control
  radius, the density.
- `SYS64_UI_LOOK` takes how many words the program knows and fills no
  more: a v1 program asking for eleven gets exactly eleven. The phone's
  kernel answers the same call the same way.
- The dyes are numbers in `look.h` (Night and Linen), read by the tests.
- **Test `t_protonui` (new), part 1:** a program asks for eleven words
  with a guard word after them, which stays untouched; asks for all of
  them and prints them; the test compares with `look.h`. On ARM64 too
  (`t_arm64win`).
- **Done** (journal: "ProtonUI v2 stages 1-2"). The word count rides in
  the byte above the theme in `SYS64_UI_LOOK`'s second argument, so a
  binary from before v2 (nothing there) gets eleven words; the phone
  answers with its own style (`UI_STYLE_PHONE`) and a finger's density.

## Stage 2 - four drawing operations - 1 day

- `WD_GRAIN` (the 3x3 tile copied over a rectangle at the look's ink),
  `WD_STITCH` (a dashed 2 px line), `WD_KNOT` (a circle hollow, ringed or
  filled), `WD_THREAD` (a cubic curve with soft edges and a width) in
  `desktop_win_draw` and in `winmob.c`; `ud_grain`, `ud_stitch`,
  `ud_knot`, `ud_thread` in `udraw.h`.
- **Test:** a program draws each on a known background; the screen is
  read through QMP and compared with the same shapes drawn by a small
  rasteriser in Python written from the definitions, not from the C.
  The grain's pattern is checked pixel for pixel.
- **Done** (journal: "ProtonUI v2 stages 1-2"). The four are in
  `kernel/gfx.c` (`gfx_grain_s`, `gfx_stitch_s`, `gfx_knot_s`,
  `gfx_thread_s`), which both kernels build; a thread's two pulls ride
  packed in the command's `font` and `px` words. `t_protonui` checks them
  on the PC's screen; the phone draws them with the same code.

## Stage 3 - Weave as the third style - 3 to 4 days

- `layout weave` beside `mac` and `windows`, in Settings and in the
  first-start card.
- `look.h`: the third set of colours and sizes (window corners 10, the
  selvedge 3, the grain tile 3, the loom's height).
- `kernel/desktop.c`: opaque window surfaces with the grain on title
  bars; the selvedge in the thread colour along the top of the window in
  front; window buttons at the right in one capsule, close the only
  madder one; the loom along the bottom - the spool, the programs on
  their thread with a coloured segment under each that runs, desktops
  as knots, the clock; loom view (the launcher, full screen).
- `WD_GLASS` answered with cloth under Weave, still returning how light
  it came out.
- `kernel/aurora.c`: the warp wallpaper, whole numbers only, so a test
  can draw it again and expect the same pixels.
- **Test `t_weave` (new):** the desktop in Weave; the selvedge's colour
  along the window in front and not along the others, moving with the
  focus; the grain's pattern on a title bar; the loom's segments under
  running programs; a click on the spool opens loom view; the warp
  compared with a Python drawing of it. The groups for the other two
  styles must pass unchanged.
- **Done** (journal: "Weave, stage 3", then "Weave as its design draws
  it"). The second pass laid it out as `docs/weave/canvas/Main.dc.html`
  and `Spool.dc.html` draw it: the design's own icons (its table in
  `Icon.dc.html`, flattened by `scripts/mkweaveicons.py`, stroked by
  `kernel/weave.c`), the window's selvedge over a bar of 38 with the
  program's icon, its name and the capsule of round buttons (the window
  keeps its content's size and grows by the difference), the loom with
  the field that finds anything, one icon a program with its thread,
  the tray and the clock, and loom view in place of the menu. `t_weave`
  reads all of it: the loom's cloth and the warp to the pixel, the Files
  icon's dye, grain and stroke against the design's table, the bar's
  height and cloth, the title in Inter, close's madder, the selvedge
  moving with the focus, a desktop's knot, loom view's four threads and
  its keys, and the style a program hears.

## Stage 4 - the new controls - 3 days

- `pu_*` names in `ugui.h`, with every `ug_*` name kept, so v1 programs
  build unchanged; the items in an array the program hands in, so the
  limit of 24 goes; `pu_stack` and `pu_grid` beside `ug_layout`.
- The v1 controls drawn in the style in use (the field's stitch pulled
  into the thread on focus, the slider as a thread being spun, ...).
- New: switch, segments, progress (and the shuttle-like unknown
  length), spinner, knots, menu, card, avatar, side panel, sheet -
  switch, segments and progress first, since Settings needs them.
- Focus drawn as the thread 2 px outside the control; every item has a
  name (`pu_name`) for a screen reader.
- **Test `t_protonui`, part 2:** `user/pudemo.c` shows every control;
  the screen is read through QMP for each control's states (a switch
  on and off, the chosen segment, a knot's three states, the thread
  under the focused control), clicks and keys move them and the program
  prints what it was told. `t_gcode` and every group that clicks a v1
  program pass unchanged.
- **Done** (journal: "ProtonUI v2 stage 4"), the first of the new
  controls: buttons of four kinds, switch, segments, progress (known
  and not), knots; the field, the box, the slider, the list and the tabs
  drawn Weave's way under Weave and the old way under the other styles;
  `pu_start` with the program's own array, Tab and Space, the focus as
  the thread round the control, `pu_name`. Settings draws its controls
  with them. Still to come: spinner, menu, card, avatar, side panel,
  sheet - and the programs that draw their own controls moved over.

## Stage 5 - density and touch - 1.5 days

- `pu_density`: pointer (rows 28, targets 24) or finger (rows 48,
  targets 44), from the device - the phone and the tablet in Touch
  mode are fingers.
- A long press is a right click; a swipe on a list row brings its
  actions out.
- **Test:** `pudemo` on the phone (`t_arm64win`): the rows are 48 px,
  a tap on a 44 px target lands, a long press opens the menu.
- **Done** (journal: "ProtonUI v2 stage 5"). `pu_density`, `pu_row` and
  `pu_target` from the look; under `pu_start` a list's rows, a box's
  and a field's height and the least of what is aimed at follow it,
  and a thing smaller than a target takes a tap within one round it.
  The phone tells a finger held still for half a second to the program
  as a right click (`WEV_ALTCLICK`), which `ugui` answers with
  `UG_MENU`; `pu_menu` is the menu, its line `UG_CHOSEN`. A list row
  swiped left shows its actions (`pu_list_actions`, `UG_SWIPED`, a tap
  on one `UG_ACTION`). `pudemo` moved to `libc/tests/`, musl for both
  machines, laid out for the density it is told.

## Stage 6 - the shell - 2 days

- `pu_notify(weight, title, text)` with Quiet, Normal and Urgent and up
  to two actions that come back as events (`SYS64_NOTIFY` grows; the
  old call keeps working).
- `pu_shuttle(&activity)`: a timer, a download, a call - a title, a
  progress or a count-down, the program's colour; a new call beside
  `SYS64_NOW_PLAYING`. Under Weave the island becomes the shuttle,
  sliding up out of the loom.
- Threads (notifications, one thread per program) opened from the knot
  by the clock.
- **Test:** a program sends one notification of each weight; the
  screen shows Normal on the shuttle and then in Threads, Urgent stays
  until answered, Quiet goes straight to Threads; a click on an action
  comes back to the program as its event.

## Stage 7 - the phone's thread - 1.5 days

- The third way to get around a phone, next to gestures and three
  buttons: a thread along the bottom edge with a knot for each running
  program - tap a knot, slide along it, swipe up for home, up and hold
  for loom view, from the left edge for back. Settings > Navigation.
- The PIN as six knots on a thread.
- **Test (`t_arm64mobile`):** with the thread chosen, a tap on a knot
  brings that program to the front; a swipe up goes home.

## After that

The programs WEAVE.md names are each a project of their own and are not
in these estimates: Strand, Purl, Family, Find, Market, Map, Skein,
Mend, Swatch, Notes, and the existing programs redrawn in Weave. Swatch
comes first of them - every control, live, with the line of C that
makes it - because it is ProtonUI v2's own catalogue.

**In all: about 13 to 15 working days** for stages 1-7.
