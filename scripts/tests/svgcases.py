"""SVG pictures for the tests: each a part of SVG the drawing is to get
right, and the size to draw it at"""

NS = 'xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"'

CASES = [
    ("shapes", 120, 80, f"""<svg {NS} width="120" height="80">
<rect x="4" y="4" width="50" height="30" fill="#c03030"/>
<rect x="62" y="4" width="50" height="30" rx="8" fill="#3060c0"/>
<circle cx="25" cy="58" r="16" fill="#30a050"/>
<ellipse cx="85" cy="58" rx="28" ry="12" fill="orange"/>
</svg>"""),
    ("path", 100, 100, f"""<svg {NS} width="100" height="100" viewBox="0 0 100 100">
<path d="M10 10 H 50 V 40 L 30 60 Z" fill="#202080"/>
<path d="M60,10 C 90,10 90,40 60,40 S 30,70 60,90" fill="none" stroke="#a02020" stroke-width="4"/>
<path d="m10 70 q 15 -20 30 0 t 30 0" fill="none" stroke="#208020" stroke-width="3"/>
</svg>"""),
    ("arcs", 100, 100, f"""<svg {NS} width="100" height="100">
<path d="M20 50 A 30 20 0 0 1 80 50 Z" fill="#7030a0"/>
<path d="M20 60 a30 30 0 1 0 60 0" fill="none" stroke="#000" stroke-width="2"/>
<path d="M10 90a5 5 0 1 1 10 0 5 5 0 1 1-10 0z" fill="red"/>
</svg>"""),
    ("transforms", 120, 120, f"""<svg {NS} width="120" height="120">
<g transform="translate(60 60) rotate(30)">
  <rect x="-30" y="-10" width="60" height="20" fill="#1080c0"/>
</g>
<g transform="matrix(1 0 0.5 1 0 0) scale(1.5)"><rect x="5" y="5" width="20" height="10" fill="#c08010"/></g>
<rect x="80" y="80" width="20" height="20" fill="#30c030" transform="rotate(45 90 90)"/>
</svg>"""),
    ("strokes", 140, 90, f"""<svg {NS} width="140" height="90">
<polyline points="10,70 30,20 50,70" fill="none" stroke="#000" stroke-width="8" stroke-linejoin="miter"/>
<polyline points="60,70 80,20 100,70" fill="none" stroke="#a00" stroke-width="8" stroke-linejoin="round" stroke-linecap="round"/>
<polyline points="110,70 120,20 130,70" fill="none" stroke="#00a" stroke-width="6" stroke-linejoin="bevel" stroke-linecap="square"/>
<line x1="10" y1="82" x2="130" y2="82" stroke="#080" stroke-width="3"/>
</svg>"""),
    ("rules", 100, 60, f"""<svg {NS} width="100" height="60">
<path fill-rule="evenodd" fill="#204080" d="M5 5h40v40h-40z M15 15h20v20h-20z"/>
<path fill="#802040" d="M55 5h40v40h-40z M65 15v20h20v-20z"/>
<rect x="5" y="50" width="90" height="8" fill="#000" fill-opacity="0.3"/>
</svg>"""),
    ("viewbox", 160, 80, f"""<svg {NS} width="160" height="80" viewBox="0 0 10 10">
<circle cx="5" cy="5" r="5" fill="#d04040"/>
<rect x="0" y="0" width="2" height="2" fill="#4040d0"/>
</svg>"""),
    ("gradients", 120, 60, f"""<svg {NS} width="120" height="60">
<defs>
<linearGradient id="g1"><stop offset="0" stop-color="#ff0000"/><stop offset="1" stop-color="#0000ff"/></linearGradient>
<radialGradient id="g2" cx="0.5" cy="0.5" r="0.5"><stop offset="0%" style="stop-color:#ffffff"/><stop offset="100%" style="stop-color:#008000"/></radialGradient>
<linearGradient id="g3" xlink:href="#g1" x1="0" y1="0" x2="0" y2="1"/>
</defs>
<rect x="2" y="2" width="56" height="26" fill="url(#g1)"/>
<circle cx="90" cy="30" r="26" fill="url(#g2)"/>
<rect x="2" y="32" width="56" height="26" fill="url(#g3)"/>
</svg>"""),
    ("use", 120, 60, f"""<svg {NS} width="120" height="60">
<defs>
<symbol id="dot" viewBox="0 0 10 10"><circle cx="5" cy="5" r="4" fill="#c04000"/></symbol>
<rect id="bar" width="30" height="6" fill="#004080"/>
</defs>
<use href="#dot" x="0" y="0" width="40" height="40"/>
<use xlink:href="#dot" x="45" y="10" width="20" height="20"/>
<use href="#bar" x="70" y="10"/>
<use href="#bar" x="70" y="30" fill="red"/>
</svg>"""),
    ("styles", 100, 50, f"""<svg {NS} width="100" height="50">
<style>.a{{fill:#e08000}} .b , #c {{ fill:#0080e0; stroke:#000; stroke-width:2 }}</style>
<rect class="a" x="2" y="2" width="30" height="30"/>
<rect class="x b" x="35" y="2" width="30" height="30"/>
<rect id="c" x="68" y="2" width="30" height="30" style="fill:#00a000"/>
<g fill="#808080" opacity="0.5"><rect x="2" y="36" width="96" height="10"/></g>
</svg>"""),
    ("icon", 96, 96, f"""<?xml version="1.0"?>
<!-- an icon, as icon sets write them: 24 units drawn at 96 pixels -->
<svg {NS} viewBox="0 0 24 24" width="24" height="24" fill="none" stroke="currentColor" color="#333366" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
<path d="M3 9l9-7 9 7v11a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/><polyline points="9 22 9 12 15 12 15 22"/>
</svg>"""),
]
