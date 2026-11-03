"""CSS probes: one page per part of CSS, drawn with coloured boxes and no
text, so that where each colour lands can be held against Chromium's
drawing of the same page, box by box, whatever the fonts.

Each probe is (name, body CSS and markup). The page is 560 pixels wide,
as the browser's window; margin 0. Chromium's boxes for them are in
cssref.json (scripts/cssref.py makes it).
"""

WIDTH, HEIGHT = 560, 300

COLOURS = {
    "red": (0xD0, 0x20, 0x20), "green": (0x20, 0xA0, 0x40),
    "blue": (0x20, 0x40, 0xD0), "orange": (0xE0, 0x80, 0x10),
    "purple": (0x80, 0x30, 0xA0), "teal": (0x10, 0x90, 0x90),
}
HEX = {k: "#%02x%02x%02x" % v for k, v in COLOURS.items()}

def _c(name):
    return HEX[name]


def _svg(name):
    return (f"<svg xmlns='http://www.w3.org/2000/svg' width='30' height='12'>"
            f"<rect width='30' height='12' fill='{HEX[name]}'/></svg>")


def _b64(text):
    import base64
    return base64.b64encode(text.encode()).decode()

PROBES = [
    ("var", f"""<style>:root{{--w:120px;--h:30px;--c:{_c('red')}}}
.a{{width:var(--w);height:var(--h);background:var(--c)}}
.b{{--c:{_c('green')};width:calc(var(--w) * 2);height:var(--h);background:var(--c)}}
.c{{width:var(--missing, 90px);height:20px;background:{_c('blue')}}}</style>
<div class=a></div><div class=b></div><div class=c></div>"""),
    ("calc", f"""<style>.a{{width:calc(100% - 100px);height:20px;background:{_c('red')}}}
.b{{width:calc(50% + 2rem);height:calc(10px + 1em);background:{_c('green')}}}
.c{{margin-left:calc(25% - 10px);width:calc((100% - 40px) / 4);height:15px;background:{_c('blue')}}}
.d{{width:min(300px, 50%);height:max(10px, 2vh);background:{_c('orange')}}}
.e{{width:clamp(100px, 30%, 140px);height:12px;background:{_c('purple')}}}</style>
<div class=a></div><div class=b></div><div class=c></div><div class=d></div><div class=e></div>"""),
    ("overflow", f"""<style>.box{{width:100px;height:40px;overflow:hidden;background:{_c('blue')}}}
.big{{width:300px;height:120px;background:{_c('red')};margin-left:20px}}
.clip{{width:200px;height:30px;overflow:clip;margin-top:10px}}
.wide{{width:400px;height:60px;background:{_c('green')}}}</style>
<div class=box><div class=big></div></div><div class=clip><div class=wide></div></div>"""),
    ("hidden", f"""<style>.sr{{position:absolute;width:1px;height:1px;overflow:hidden;clip:rect(0 0 0 0);
white-space:nowrap}} .sr div{{width:200px;height:50px;background:{_c('red')}}}
.cp{{position:absolute;clip-path:inset(50%);top:100px}} .cp div{{width:200px;height:50px;background:{_c('purple')}}}
.v{{visibility:hidden;width:100px;height:20px;background:{_c('orange')}}}
.o{{opacity:0;width:60px;height:10px;background:{_c('blue')}}} .o div{{height:5px;background:{_c('teal')}}}
.ok{{width:80px;height:20px;background:{_c('green')}}}</style>
<div class=sr><div></div></div><div class=cp><div></div></div><div class=v></div><div class=o><div></div></div><div class=ok></div>"""),
    ("transform", f"""<style>.menu{{position:fixed;top:0;left:0;width:200px;height:100px;background:{_c('red')};
transform:translateX(-100%)}} .t{{width:100px;height:30px;background:{_c('green')};transform:translate(50px, 10px)}}
.s{{width:60px;height:20px;background:{_c('blue')};margin-top:30px;transform:translateY(-20px)}}</style>
<div class=menu></div><div class=t></div><div class=s></div>"""),
    ("fixed", f"""<style>.top{{position:sticky;top:0;height:20px;background:{_c('blue')}}}
.f{{position:fixed;right:10px;bottom:10px;width:40px;height:40px;background:{_c('orange')}}}
.n{{height:30px;width:50%;background:{_c('green')}}}
.ab{{position:absolute;display:inline-flex;left:300px;width:30px;height:80px;background:{_c('teal')}}}
.ib{{display:inline-block;vertical-align:top;width:40px;height:10px;background:{_c('purple')}}}</style>
<div class=top></div><div class=n></div><div class=f></div><div><span class=ab></span><span class=ib></span></div>"""),
    ("flexgap", f"""<style>.row{{display:flex;gap:10px}} .row div{{flex:1;height:30px}}
.col{{display:flex;flex-direction:column;gap:6px;width:100px;margin-top:10px}} .col div{{height:10px}}</style>
<div class=row><div style="background:{_c('red')}"></div><div style="background:{_c('green')}"></div>
<div style="background:{_c('blue')}"></div></div>
<div class=col><div style="background:{_c('orange')}"></div><div style="background:{_c('purple')}"></div></div>"""),
    ("grid", f"""<style>.g{{display:grid;grid-template-columns:100px 1fr 2fr;gap:10px}} .g div{{height:30px}}
.h{{display:grid;grid-template-columns:repeat(2, 1fr);grid-template-rows:20px 40px;margin-top:10px}}
.span{{grid-column:1 / 3}}</style>
<div class=g><div style="background:{_c('red')}"></div><div style="background:{_c('green')}"></div>
<div style="background:{_c('blue')}"></div></div>
<div class=h><div class=span style="background:{_c('orange')}"></div><div style="background:{_c('purple')}"></div>
<div style="background:{_c('teal')}"></div></div>"""),
    ("areas", f"""<style>.g{{display:grid;grid-template-columns:120px 1fr;grid-template-rows:30px 60px;
grid-template-areas:"head head" "side main"}} .hd{{grid-area:head;background:{_c('red')}}}
.sd{{grid-area:side;background:{_c('green')}}} .mn{{grid-area:main;background:{_c('blue')}}}</style>
<div class=g><div class=mn></div><div class=hd></div><div class=sd></div></div>"""),
    ("grid2", f"""<style>.g{{display:grid;grid-template-columns:repeat(auto-fill, minmax(120px, 1fr));gap:8px 12px}}
.g div{{height:20px}} .r{{grid-row:span 2;height:auto!important}}
.k{{display:grid;grid-template-columns:[a] 80px [b] 1fr [c] 60px [d];grid-auto-rows:25px;margin-top:6px}}
.k .x{{grid-column:b / d}} .k .y{{grid-column:-2;grid-row:2}}</style>
<div class=g><div class=r style="background:{_c('red')}"></div><div style="background:{_c('green')}"></div>
<div style="background:{_c('blue')}"></div><div style="background:{_c('orange')}"></div>
<div style="background:{_c('green')}"></div></div>
<div class=k><div class=x style="background:{_c('purple')}"></div><div class=y style="background:{_c('teal')}"></div></div>"""),
    ("grid3", f"""<style>.g{{display:grid;grid-template-columns:100px 100px;grid-template-rows:60px;
justify-items:center;align-items:end}} .g div{{width:40px;height:20px}}
.f{{display:grid;grid-template-columns:repeat(auto-fit, minmax(100px, 1fr));margin-top:10px}} .f div{{height:15px}}
.c{{display:grid;grid-template-columns:1fr 1fr 1fr;margin-top:10px}} .c div{{height:15px}}
.c .z{{grid-column:2;justify-self:end;width:50px}}</style>
<div class=g><div style="background:{_c('red')}"></div><div style="background:{_c('green')};justify-self:start;align-self:start"></div></div>
<div class=f><div style="background:{_c('blue')}"></div><div style="background:{_c('orange')}"></div></div>
<div class=c><div class=z style="background:{_c('purple')}"></div><div style="background:{_c('teal')}"></div></div>"""),
    ("selectors", f"""<style>div{{height:16px;width:50px}} .x:not(.y){{background:{_c('red')}}}
:is(.p, .q){{background:{_c('green')};width:80px}} .w:where(.z){{background:{_c('blue')};width:110px}}
@supports (display:flex){{.s{{background:{_c('orange')};width:140px}}}}
.h:has(> span){{background:{_c('purple')};width:170px}} .t:not(:enabled){{background:{_c('teal')};width:200px}}</style>
<div class=x></div><div class=q></div><div class="w z"></div><div class=s></div><div class=h><span></span></div><div class=t></div>"""),
    ("images", f"""<style>.m{{width:40px;height:20px;background-color:{_c('green')};
mask-image:url("data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' width='40' height='20'><rect x='10' width='20' height='20' fill='%23000'/></svg>");
-webkit-mask-image:url("data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' width='40' height='20'><rect x='10' width='20' height='20' fill='%23000'/></svg>")}}
.b{{width:60px;height:12px;margin-top:4px;background:url("data:image/svg+xml;base64,{_b64(_svg('orange'))}") no-repeat}}</style>
<div class=m></div><div class=b></div>"""),
    ("media", f"""<style>.a{{height:20px;background:{_c('red')};width:100px}}
@media (max-width:600px){{.a{{width:200px}}}} @media (min-width:1000px){{.a{{width:300px}}}}
@media (prefers-color-scheme:dark){{.b{{background:{_c('blue')}}}}}
.b{{height:20px;width:120px;background:{_c('green')}}}
.c{{height:10px;width:50px;background:{_c('purple')}}} @media (max-width:calc(600px - 1px)){{.c{{width:150px}}}}
@media (min-width:calc(500px + 10%)){{.c{{width:250px}}}}</style><div class=a></div><div class=b></div><div class=c></div>"""),
]


def page(body):
    return ("<!doctype html><html><head><title>Probe</title><style>html,body{margin:0;"
            "padding:0;background:#fff}</style></head><body>" + body +
            "<div style='height:400px'></div></body></html>").encode()


def boxes(width, height, at, region=None):
    """Where each probe colour is: {name: (x0, y0, x1, y1, pixels)} in the
    region (x0, y0, x1, y1) - within 2 of the colour, to allow for
    nothing but exact paint"""
    x0, y0, x1, y1 = region or (0, 0, width, height)
    found = {}
    for y in range(y0, y1):
        for x in range(x0, x1):
            p = at(x, y)
            for name, c in COLOURS.items():
                if abs(p[0] - c[0]) <= 2 and abs(p[1] - c[1]) <= 2 and abs(p[2] - c[2]) <= 2:
                    b = found.get(name)
                    if b:
                        found[name] = (min(b[0], x - x0), min(b[1], y - y0),
                                       max(b[2], x - x0), max(b[3], y - y0), b[4] + 1)
                    else:
                        found[name] = (x - x0, y - y0, x - x0, y - y0, 1)
                    break
    return found
