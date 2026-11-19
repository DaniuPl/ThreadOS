# Weave: ThreadOS's own look

Weave is the third desktop style, next to the two ThreadOS already has
(a bar along the top with a dock, and a taskbar along the bottom). The
other two borrow what people know from elsewhere; Weave is ThreadOS's
own, and it takes its ideas from the name: a thread, cloth, a loom.

This document is the design, not the code. Nothing of it is built yet.
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
| **Spool** | The launcher button at the left end of the loom; also the window buttons' capsule (minimise, maximise, close) | Thread is wound on a spool |
| **Loom view** | The launcher, full screen: open windows on top, programs below, threaded on four threads by what they are for | |
| **The shuttle** | What is going on (music, timers, downloads, calls, camera and microphone) in a capsule that slides up out of the loom - ThreadOS's answer to a "dynamic island" | The shuttle is what travels across the loom carrying the thread |
| **Warp** | The wallpaper: threads of the dyes crossing a dark ground | The warp is the threads stretched on a loom |
| **Threads** | Notifications, hung on one vertical line, newest at the top | |
| **Game mode** | While a game holds the mouse, the loom steps aside and notifications wait | |

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
| **Calendar**, **Gallery**, **Mixer**, **Recorder**, **Monitor**, **Settings**, **Files**, **Term** | The existing programs, drawn in Weave | |
| **Loomflight** | A game made for the mock-ups: fly a shuttle through the warp and gather knots | |

### Setting up

| Name | What it is |
|---|---|
| **Install ThreadOS** | From the stick: try or install; beside another system or the whole disk, shown as a bar of the disk now and after; a list of what will be written and what will not be touched; typing YES (or ERASE for the whole disk) to go on; the partition filling like cloth on a loom; done, with the boot menu shown. The same steps `user/ginstal.c` takes today. |
| **Welcome to ThreadOS** | The first start after installing: language and keyboard, network, your profile and thread colour, the desktop style (or, on a phone, how you get around), and a summary with "Change" by every line. The fields are those of the first-start card in `kernel/desktop.c` today. |

## Type and shape

- Interface text: Inter (13 px, titles 13 px bold, the clock 15 px
  bold); JetBrains Mono for Term and Code. Today the system draws DejaVu;
  Inter and JetBrains Mono are under the SIL Open Font Licence; bringing them in would need the owner's yes and an entry in `third_party/README.md`, like any library.
- Window corners 10 px; the selvedge and the focus thread 3 px; the grain
  tile 3 px.
- Window buttons sit on the right, in one capsule (the spool); close is
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
