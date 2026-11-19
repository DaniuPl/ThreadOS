# Weave: ThreadOS's own look

Weave is the third desktop style, next to the two ThreadOS already has
(a bar along the top with a dock, and a taskbar along the bottom). The
other two borrow what people know from elsewhere; Weave is ThreadOS's
own, and it takes its ideas from the name: a thread, cloth, a loom.

This document is the design. What is built, and how far, is in
[protonui-plan.md](protonui-plan.md): the look programs read with the
dyes, the grain, stitches, knots and threads as drawing operations, and
the Weave style itself (`layout weave`: the loom, the selvedge, cloth
for glass, the warp).
The mock-ups are in `docs/weave/canvas/` (one `.dc.html` file per
screen, laid out by `canvas.json`), and on the design canvas they were
drawn on: https://claude.ai/artifact/AG3KVaL3nxBS3a3zosmn3B

## The four rules

1. **Cloth, not glass.** Surfaces are opaque and carry a faint woven
   grain: a 3x3 tile at about 2 % ink, copied over a flat fill. Nothing
   is blurred. The compositor draws in software, so a blur is a pass
   over every pixel under the glass in every frame; a tile is a copy.
   This is why Weave is fast on any machine and on the phone's ramfb.
2. **One thread.** Whatever has focus carries one colour, drawn as a
   line: the selvedge along the top of the window in front, its segment
   on the loom, the knot by the chosen row. Nothing else uses that
   colour.
3. **Knots for state.** A small circle means "this is so": hollow is
   empty, ringed has something in it, filled is here. Desktops, running
   programs, unread messages and setup steps are all knots.
4. **Stitches divide.** A dashed line separates parts of one surface; a
   solid line is the edge of a surface. A field has a stitch under it
   that pulls tight into the thread when the field takes focus.

## Names

Every name comes from weaving, so the system reads as one thing.

### The desktop

| Name | What it is | Why the name |
|---|---|---|
| **Weave** | The design language, and the third desktop style | Cloth is woven; so is this desktop |
| **Night**, **Linen** | The dark and the light theme | Linen is the pale cloth; Night needs no reason |
| **Selvedge** | 3 px of the thread colour along the top of the window in front | The selvedge is the woven edge of a cloth |
| **Grain** | The faint woven texture on title bars and the loom | The grain of a fabric |
| **The loom** | The bar along the bottom: the launcher button, programs, desktops, clock | Where the cloth is made |
| **The thread** (on the loom) | The line under the programs; a coloured segment under each that runs | |
| **Spool** | The launcher button at the left end of the loom; it opens loom view | Thread is wound on a spool |
| **Loom view** | The launcher, full screen: open windows on top, programs below, threaded on four threads by what they are for | |
| **The shuttle** | What is going on (music, timers, downloads, calls, camera and microphone) in a capsule that slides up out of the loom - ThreadOS's answer to a "dynamic island" | The shuttle is what travels across the loom carrying the thread |
| **Warp** | The wallpaper: threads of the dyes crossing a dark ground | The warp is the threads stretched on a loom |
| **Threads** | Notifications, each program's on a thread of its own, newest at the top; opened from the knot by the clock. The only thing called Threads - file labels are *Tags* | |
| **Game mode** | While a game holds the mouse, the loom steps aside and notifications wait |
| **Quiet** | Nothing rides the shuttle, nothing chimes; chosen people and Urgent still get through |
| **Quiet, Normal, Urgent** | The three weights of a notification: straight into Threads; on the shuttle for a moment with a chime; on the shuttle until answered |
| **Sliders** | Sound and light: a thread being spun - solid where set, a stitch beyond, a knot to hold. On the phone the volume keys bring a vertical thread out of the edge beside them | |

### Colours: the dyes

Six colours, each named after a real dye, each with one job.

| Dye | Job | Night | Linen |
|---|---|---|---|
| **Indigo** | Focus, the one thread; primary buttons | `#5B7CFA` | `#3F5EE8` |
| **Madder** | Close, delete, errors | `#E5484D` | `#C9302C` |
| **Saffron** | Warnings, the microphone is on | `#F5A524` | `#B86E00` |
| **Weld** | Done, connected, the camera is on | `#46C27A` | `#1E8A4C` |
| **Woad** | Links, the network, Weft | `#2EC4C4` | `#1E7F86` |
| **Cochineal** | Media: music, pictures, sound | `#D152A6` | `#C2366F` |

Each person picks their own thread colour at first start ("Your thread");
it becomes their focus colour and the ring around their picture.

### The phone and the tablet

| Name | What it is |
|---|---|
| **The thread** (navigation) | The third way to get around a phone, next to *Gestures* and *Three buttons* (`nav_buttons` in `kernelarm64/mobile.c` today). A thin thread along the bottom edge with a knot for each running program. Tap a knot: that program. Slide along the thread: one program at a time. Swipe up: home; up and hold: loom view. From the left edge: back. |
| **The shade** | What a pull down from the top shows: switches, light, sound, the shuttle opened, and the threads |
| **The drawer** | Every program on the phone, swiped up from home: on threads by what they are for, or A to Z |
| **The drawer** sort | By use (on threads) or A to Z | |
| **The PIN knots** | Unlocking a phone: six knots on a thread, each digit ties one |
| **Locked** | The lock screen says who and how many, never what; torch and camera in the corners; the shuttle still rides the thread |
| **Touch** / **Windows** | The tablet's two modes; Windows comes on by itself when a keyboard is attached |

### Programs

| Name | What it is | Why the name |
|---|---|---|
| **Weft** | The web browser (exists today) | The weft is the thread woven across the warp |
| **Code** | The IDE (exists today, `user64/gcode.c`), drawn in Weave | |
| **Strand** | Version control (git): each branch a thread, each commit a knot, a merge two threads becoming one | A strand is one thread of many |
| **Purl** | The messenger. Replies to one message go into a *side thread* so the main conversation stays readable | A purl is a knitting stitch, and to purl is to murmur |
| **Family** | People and their devices: who is where (only those who chose to share, and they see who sees them), find, ring, lock or erase a device | |
| **Find** | Searches everything: files, what is inside files, settings, Weft's history | |
| **Market** | The app store. Every app is built from source and signed, shows its licence and what it may use before you get it | A market is where cloth was sold |
| **Map** | Maps and directions; the route is a thread with knots at the turns | |
| **Skein** | The music player: albums, a queue that is a thread, the waveform as a thread being spun | A skein is a loose coil of yarn - a collection |
| **Calendar**, **Gallery**, **Mixer**, **Recorder**, **Monitor**, **Settings**, **Files**, **Term** | The existing programs, drawn in Weave | |
| **Notes** | Notes and lists, shareable; a list's items tick off as knots | |
| **Mend** | The backup: a copy every night to a stick or disk, a day chosen on a thread to bring a file back - beside the one you have, never over it | To mend is to repair cloth |
| **Swatch** | For developers: every ProtonUI control, live, in both themes, with the line of C that makes it; and new programs from a template | A swatch is a sample of cloth |
| **Loomflight** | A game made for the mock-ups: fly a shuttle through the warp and gather knots | |

### Setting up

| Name | What it is |
|---|---|
| **Install ThreadOS** | From the stick: try or install; beside another system or the whole disk, shown as a bar of the disk now and after; a list of what will be written and what will not be touched; typing YES (or ERASE for the whole disk) to go on; the partition filling like cloth on a loom; done, with the boot menu shown. The same steps `user/ginstal.c` takes today. |
| **Welcome to ThreadOS** | The first start after installing: language and keyboard, network, your profile and thread colour, the desktop style (or, on a phone, how you get around), and a summary with "Change" by every line. The fields are those of the first-start card in `kernel/desktop.c` today. |

### Icons

One family: a tile in the program's dye with the grain, one white drawing in a
single stroke on a 48 grid, and the thread along the bottom. Lines and
circles only, so the kernel draws them at any size the way it draws its
icons today (`ART_*` in `kernel/desktop.c`). `docs/weave/canvas/Icon.dc.html`
holds all 32.

### Where each setting lives

Each setting has one home: **Appearance** - theme, thread colour, grain,
wallpaper; **Desktop** - style, window buttons, selvedge, the loom, desktops,
the shuttle; **Easier to use** - text size, contrast, patterns on threads,
motion, keys, reading aloud. Other pages may point to it, never repeat it. Each program's own volume
lives in Mixer alone; the card under the loom's clock has the overall
volume and light, and a way to Mixer.

### One flow, three sizes

Installing and the first start are one flow each, drawn for the desktop,
the tablet and the phone. A step changed on one is changed on all three;
notes on the canvas say which board is which step.

## ProtonUI v2

ProtonUI is the toolkit programs draw their windows with. v1 is what
exists today: `user/udraw.h` (shapes and text, sent to the desktop with
`SYS64_WIN_DRAW` and drawn there) and `user/ugui.h` (controls as a list
of things). v2 is the proposal for Weave, written up as a technical page
in `docs/weave/canvas/ProtonUI.dc.html`:

- every v1 program builds unchanged (the `ug_*` names stay for `pu_*`);
- `struct ud_look` grows at its end - the style, six dyes, the person's
  thread colour, grain, radius and density - so a v1 program that asks
  for eleven words still gets those eleven;
- four drawing operations: `GRAIN`, `STITCH`, `KNOT`, `THREAD`; `GLASS`
  is answered with cloth under Weave;
- new controls: switch, segments, thread progress, spinner, knots, menu,
  card, avatar, side panel, sheet; the items in an array the program
  owns, so the limit of 24 goes;
- touch through a density (pointer or finger), and calls into the
  shell: notifications with a weight and actions, and the shuttle;
- a test group, `t_protonui`, that reads every control off the screen.

## Type and shape

- Interface text: Inter (13 px, titles 13 px bold, the clock 15 px
  bold); JetBrains Mono for Term and Code. Today the system draws DejaVu;
  Inter and JetBrains Mono are under the SIL Open Font Licence; bringing them in would need the owner's yes and an entry in `third_party/README.md`, like any library.
- Window corners 10 px; the selvedge and the focus thread 3 px; the grain
  tile 3 px.
- Window buttons sit on the right, in one capsule; close is
  the only madder one.

## Where it would go in the code

- `kernel/include/look.h`: a third set of colours and the sizes above,
  as plain numbers, as the tests read them; a third value for the
  `layout` setting next to the bar-and-dock and taskbar layouts.
- `kernel/desktop.c`: the selvedge, the grain, the loom and its thread,
  the spool, the shuttle in place of the island.
- `kernel/aurora.c`: the warp beside the aurora, drawn the same way -
  whole numbers only, so a test can draw it again and expect the same
  pixels.
- `kernelarm64/mobile.c`, `mobapps.c`: the thread as a third navigation
  next to `nav_buttons`.
- Tests: `t_desktop` reads the selvedge and the grain off the screen;
  `t_arm64mobile` taps a knot on the thread and checks the program in
  front changed.
