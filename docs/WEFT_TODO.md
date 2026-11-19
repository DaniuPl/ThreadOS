# Weft: what is left to do

Weft (the WebKit WPE browser, `apps/webkit/`) is set aside for now while
other work goes ahead. This is where it stopped, so that it can be
picked up again without reading the whole journal. Each item says what
is missing, what is known and how it should be tested.

## 1. The phone's media test fails (`arm64media`)

The first run of `t_arm64media` on the media-enabled aarch64 WebKit
(`make weft-arm64` with GStreamer) never gets to the video page: Weft
prints `[weft] load finished ` with an empty URL and then every check
times out. `arm64weft` (pages without media) and `arm64gst` (GStreamer
alone) pass, so the fault is between them.

Known so far:
- the empty URL suggests the first load is `about:blank` or that the
  `run=` argument list was split at a comma and Weft got no page;
- the PC's `webmedia` passes with the same pages.

To do: log the argument Weft receives, load the page by itself, then
find what differs from the PC (the GStreamer plugin registry, the sink,
the decoder elements built for aarch64). The test itself stays as it
is: it fails until the phone plays.

## 2. Now Playing on the phone (Control Center)

On the PC the island shows what a page plays, with a play/pause button,
and it hides five seconds after a pause (`t_webmedia`,
`island_play_pause`). On the phone, `kernelarm64/mobile.c` already draws
a Now Playing card in the pulled-down shade (`np_fit()`), but nothing
tests it. It needs item 1 first.

To do: a check in `t_arm64media` that the shade's card names the page's
media, that a tap pauses it (the sound stops, read from the virtio-sound
model) and that the card goes away after the hold.

## 3. YouTube and YouTube Music

Not checked yet. They need Media Source Extensions (`MediaSource`,
`SourceBuffer`), which our WebKit build has switched on, and network
access from QEMU, which CI does not have.

To do:
- by hand: open youtube.com and music.youtube.com in `make run`, and
  write down what works (the page, sign-in, playback, the island);
- in tests: a local page that plays through MSE the way YouTube does
  (segments appended to a `SourceBuffer`, codec switches, seeking),
  compared with what host GStreamer decodes from the same file;
- performance: how long the first frame takes and how many frames are
  dropped (WebKit's `webkitDroppedFrameCount`).

## 4. Gestures: pinch, double-tap, edge swipe

Weft has a zoom option but no gestures. The phone's touchscreen
(virtio-input, multi-touch slots) already reaches programs as a mouse
through `kernelarm64/winmob.c`, one finger only.

To do:
- pass the second finger through to programs (a new window event, or
  the touch points beside the mouse);
- in Weft: pinch zooms the page (`webkit_web_view_set_zoom_level`, or
  WPE's own pinch handling if the backend can take touch events);
  double-tap zooms in on the text under the finger and back; a swipe
  from the left edge goes back, from the right edge forward;
- test: `t_arm64weft` sends the touches through QMP's `input-send-event`
  with two slots and reads the page's `visualViewport.scale` and
  `history.length`.

## 5. The glass bar and the shell's look

- a floating glass bar for Weft (Liquid Glass, refraction at the rim,
  following the system's light or dark look) in place of the band on
  the phone;
- the phone shell's texts in Polish when the system's language is
  Polish (they are English everywhere now);
- an option to show or hide the home indicator;
- the camera in pages (`getUserMedia`) through the UVC driver on the
  PC.

Each with a screen test like `t_arm64tablet`.

## 6. A rare start-up crash in musl's malloc

Very rarely Weft dies as it starts with a fault in musl's `alloc_slot`
(a read at NULL+8). It has not been seen in the last runs and could not
be reproduced.

To do: run Weft's start 200 times in a loop (`t_arm64weft` and
`t_webkit` with a repeat count), and when it happens look at the
`ctx` of mallocng and at which thread was in `malloc` at the time
(a thread started before `__libc.threaded` was set would explain it).

## Paused Bluetooth items (not Weft, kept here so they are not lost)

- **HID over GATT**: Bluetooth keyboards and mice (HOGP). LE pairing
  is done (`kernel/bt.c`); what is left is the ATT/GATT client, finding
  the HID service, reading the report map into the kernel's HID parser
  and subscribing to the input reports.
- **A2DP**: sound to headphones. L2CAP channels, SDP, AVDTP and an SBC
  encoder (a library, brought in properly under `third_party/`).
