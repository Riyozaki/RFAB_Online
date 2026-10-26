import math

W, H = 900, 700
OUT = "/home/user/RFAB_Online/art/pelican-bicycle.svg"

INK = "#2b3240"
OUTLINE = "#aebfcc"      # body outline
BODY = "#ffffff"
BODY2 = "#eef4f8"        # shading
WING = "#e2ecf3"
WING_LINE = "#a9bccb"
FRAME_DARK = "#a83543"
ORANGE_DARK = "#a85a1f"
ORANGE = "#f09347"


def spokes(cx, cy, n=14, r1=20, r2=92, w=2.6, col="#c2ced6"):
    out = []
    for i in range(n):
        a = 2 * math.pi * i / n + 0.18
        x1, y1 = cx + r1 * math.cos(a), cy + r1 * math.sin(a)
        x2, y2 = cx + r2 * math.cos(a), cy + r2 * math.sin(a)
        out.append(f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" />')
    return f'<g stroke="{col}" stroke-width="{w}" stroke-linecap="round">' + "".join(out) + "</g>"


def wheel(cx, cy, cog=False):
    p = []
    p.append(f'<circle cx="{cx}" cy="{cy}" r="112" fill="none" stroke="{INK}" stroke-width="21"/>')
    p.append(f'<circle cx="{cx}" cy="{cy}" r="112" fill="none" stroke="#49525f" stroke-width="21" stroke-dasharray="7 27" stroke-dashoffset="3" opacity="0.55"/>')
    p.append(f'<circle cx="{cx}" cy="{cy}" r="100" fill="none" stroke="#d7e0e6" stroke-width="8"/>')
    p.append(f'<circle cx="{cx}" cy="{cy}" r="94" fill="none" stroke="#b3c1c9" stroke-width="3"/>')
    p.append(spokes(cx, cy))
    if cog:
        p.append(f'<circle cx="{cx}" cy="{cy}" r="17" fill="#8d99a6" stroke="{INK}" stroke-width="4"/>')
        p.append(f'<circle cx="{cx}" cy="{cy}" r="19" fill="none" stroke="#8d99a6" stroke-width="5" stroke-dasharray="3 4"/>')
    p.append(f'<circle cx="{cx}" cy="{cy}" r="13" fill="#6b7683" stroke="{INK}" stroke-width="4"/>')
    p.append(f'<circle cx="{cx}" cy="{cy}" r="4" fill="{INK}"/>')
    return "\n".join(p)


RH = (275, 505)
FH = (645, 505)
BB = (450, 490)

svg = []
A = svg.append

A(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" role="img" aria-labelledby="t d">')
A('<title id="t">Пеликан на велосипеде</title>')
A('<desc id="d">Белый пеликан с большим оранжевым клювом и красным шарфом едет на коралловом велосипеде по лугу.</desc>')
A('''<defs>
  <linearGradient id="sky" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0" stop-color="#eef8ff"/>
    <stop offset="0.55" stop-color="#d8eefb"/>
    <stop offset="1" stop-color="#c2e6f6"/>
  </linearGradient>
  <linearGradient id="ground" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0" stop-color="#e3f1da"/>
    <stop offset="1" stop-color="#cfe7c4"/>
  </linearGradient>
  <linearGradient id="bodyG" x1="0.15" y1="0" x2="0.75" y2="1">
    <stop offset="0" stop-color="#ffffff"/>
    <stop offset="0.62" stop-color="#fafcfd"/>
    <stop offset="1" stop-color="#e7eef4"/>
  </linearGradient>
  <linearGradient id="beakG" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0" stop-color="#ffcf5c"/>
    <stop offset="1" stop-color="#f2a01f"/>
  </linearGradient>
  <linearGradient id="pouchG" x1="0.1" y1="0" x2="0.3" y2="1">
    <stop offset="0" stop-color="#ffd873"/>
    <stop offset="0.55" stop-color="#fbb04a"/>
    <stop offset="1" stop-color="#ef8329"/>
  </linearGradient>
  <linearGradient id="frameG" x1="0" y1="0" x2="0.4" y2="1">
    <stop offset="0" stop-color="#f0707a"/>
    <stop offset="1" stop-color="#d8414f"/>
  </linearGradient>
  <radialGradient id="sunG" cx="0.5" cy="0.5" r="0.5">
    <stop offset="0" stop-color="#fff3c4"/>
    <stop offset="0.7" stop-color="#ffe287"/>
    <stop offset="1" stop-color="#ffe287" stop-opacity="0"/>
  </radialGradient>
</defs>''')

# ---------------- background ----------------
A(f'<rect width="{W}" height="{H}" fill="url(#sky)"/>')
A('<circle cx="118" cy="118" r="96" fill="url(#sunG)" opacity="0.85"/>')
A('<circle cx="118" cy="118" r="46" fill="#ffe07a"/>')
A('''<g fill="#ffffff" opacity="0.9">
  <path d="M636 128c0-17 14-30 31-30 6-16 21-27 40-27 24 0 43 17 46 39 15 1 27 13 27 28 0 16-13 29-29 29H650c-8 0-14-6-14-14z"/>
  <path d="M262 96c0-13 11-24 25-24 5-13 18-22 32-22 19 0 34 14 37 32 12 1 21 10 21 22 0 13-10 23-23 23h-73c-10 0-19-8-19-19z" opacity="0.75"/>
  <path d="M736 268c0-10 8-18 19-18 4-10 14-17 25-17 15 0 27 11 29 25 9 1 16 8 16 17 0 10-8 18-18 18h-55c-8 0-16-7-16-15z" opacity="0.7"/>
</g>''')
A('<path d="M0 592c78-38 148-22 226 4 96 32 176-18 274-16 98 2 200 34 300 12 36-8 66-6 100 2v106H0z" fill="#d6ebcd"/>')
A(f'<rect y="600" width="{W}" height="100" fill="url(#ground)"/>')
A('<ellipse cx="455" cy="612" rx="268" ry="26" fill="#a9c9a4" opacity="0.5"/>')
A('<ellipse cx="275" cy="616" rx="72" ry="12" fill="#94b992" opacity="0.5"/>')
A('<ellipse cx="645" cy="616" rx="72" ry="12" fill="#94b992" opacity="0.5"/>')
for gx, gy, s in [(140, 640, 1.0), (196, 660, 0.8), (762, 650, 1.05), (822, 628, 0.8), (66, 620, 0.75)]:
    A(f'<g transform="translate({gx} {gy}) scale({s})" fill="none" stroke="#8fbd88" stroke-width="4" stroke-linecap="round">'
      '<path d="M0 0c-2-14-8-22-16-28"/><path d="M0 0c1-16 6-26 14-32"/><path d="M0 0c-6-10-14-15-24-17"/></g>')
A('<g stroke="#ffffff" stroke-width="9" stroke-linecap="round" opacity="0.75">'
  '<path d="M74 372h74"/><path d="M46 432h54"/><path d="M92 492h66"/></g>')
A('<g stroke="#ffffff" stroke-width="5" stroke-linecap="round" opacity="0.5">'
  '<path d="M118 330h44"/><path d="M30 500h40"/></g>')

# ---------------- bike ----------------
A('<g id="bike">')
A(wheel(*RH, cog=True))
A(wheel(*FH))
A(f'<g stroke="#9aa6b2" stroke-width="5" stroke-linecap="round" stroke-dasharray="7 5" opacity="0.95">'
  f'<path d="M{BB[0]+2} {BB[1]-35} L {RH[0]+1} {RH[1]-18}"/>'
  f'<path d="M{BB[0]+2} {BB[1]+35} L {RH[0]+1} {RH[1]+18}"/></g>')
FRAME_PATHS = (f'M{BB[0]} {BB[1]} L 368 352 '
               'M368 352 L 585 352 '
               f'M{BB[0]} {BB[1]} L 600 380 '
               'M585 352 L 601 390 '
               f'M{BB[0]} {BB[1]} L {RH[0]} {RH[1]} '
               f'M368 352 L {RH[0]} {RH[1]} '
               f'M601 388 C 614 424 632 470 {FH[0]-2} {FH[1]}')
A(f'<path d="{FRAME_PATHS}" fill="none" stroke="{FRAME_DARK}" stroke-width="19" stroke-linecap="round" stroke-linejoin="round"/>')
A(f'<path d="{FRAME_PATHS}" fill="none" stroke="url(#frameG)" stroke-width="13" stroke-linecap="round" stroke-linejoin="round"/>')
A('<g fill="none" stroke="#ff9aa4" stroke-width="3.4" stroke-linecap="round" opacity="0.85">'
  '<path d="M372 348 L 578 348"/><path d="M456 486 L 592 382"/><path d="M452 486 L 281 500"/></g>')
A('<path d="M368 352 L 358 330" stroke="#8d99a6" stroke-width="9" stroke-linecap="round"/>')
A('<path d="M336 316 C 352 305 386 303 404 310 C 414 314 412 323 400 325 C 378 329 352 328 338 324 Z" '
  f'fill="#3b4453" stroke="{INK}" stroke-width="4" stroke-linejoin="round"/>')
A('<path d="M588 350 L 574 316" stroke="#8d99a6" stroke-width="10" stroke-linecap="round"/>')
A('<path d="M574 316 L 574 300" stroke="#8d99a6" stroke-width="10" stroke-linecap="round"/>')
A('<path d="M540 296 C 566 288 600 292 616 302" fill="none" stroke="#2b3240" stroke-width="12" stroke-linecap="round"/>')
A('<path d="M536 297 C 546 292 556 291 566 292" fill="none" stroke="#3f4a59" stroke-width="15" stroke-linecap="round"/>')
A('<path d="M608 300 C 616 302 620 306 622 312" fill="none" stroke="#3f4a59" stroke-width="15" stroke-linecap="round"/>')
A('<g><circle cx="586" cy="284" r="10" fill="#ffd166" stroke="#2b3240" stroke-width="3.5"/>'
  '<path d="M582 283a5 5 0 0 1 8 0" fill="none" stroke="#e0a92f" stroke-width="2.5"/></g>')
A(f'<circle cx="{BB[0]}" cy="{BB[1]}" r="38" fill="#cfd9df" stroke="{INK}" stroke-width="4"/>')
A(f'<circle cx="{BB[0]}" cy="{BB[1]}" r="43" fill="none" stroke="#8d99a6" stroke-width="7" stroke-dasharray="4 5"/>')
A(f'<circle cx="{BB[0]}" cy="{BB[1]}" r="26" fill="none" stroke="#aab7c0" stroke-width="3"/>')
A('<g stroke="#5d6875" stroke-width="3" opacity="0.9">'
  + "".join(f'<line x1="{BB[0]+10*math.cos(a):.1f}" y1="{BB[1]+10*math.sin(a):.1f}" '
            f'x2="{BB[0]+24*math.cos(a):.1f}" y2="{BB[1]+24*math.sin(a):.1f}"/>'
            for a in [2 * math.pi * i / 5 for i in range(5)]) + '</g>')
A(f'<circle cx="{BB[0]}" cy="{BB[1]}" r="9" fill="#6b7683" stroke="{INK}" stroke-width="3.5"/>')
PED_N = (499, 456)
PED_F = (401, 524)
A(f'<g stroke="#3f4a59" stroke-width="11" stroke-linecap="round">'
  f'<path d="M{BB[0]} {BB[1]} L {PED_F[0]} {PED_F[1]}"/>'
  f'<path d="M{BB[0]} {BB[1]} L {PED_N[0]} {PED_N[1]}"/></g>')
A('<g fill="#2b3240" stroke="#1d232c" stroke-width="3">'
  f'<rect x="{PED_F[0]-19}" y="{PED_F[1]-7}" width="38" height="14" rx="5" transform="rotate(8 {PED_F[0]} {PED_F[1]})"/>'
  f'<rect x="{PED_N[0]-19}" y="{PED_N[1]-7}" width="38" height="14" rx="5" transform="rotate(-8 {PED_N[0]} {PED_N[1]})"/></g>')
A('</g>')

# ---------------- pelican ----------------
A('<g id="pelican">')

# tail feathers at the rump (behind the body)
A('<g fill="#f4f9fc" stroke="#b9c9d6" stroke-width="3.2" stroke-linejoin="round">'
  '<path d="M324 288 C 300 284 278 290 260 302 C 280 308 304 306 326 298 Z"/>'
  '<path d="M324 304 C 302 302 280 310 266 322 C 288 326 310 320 328 310 Z"/>'
  '<path d="M326 320 C 308 320 290 328 278 340 C 298 342 318 334 332 324 Z"/></g>')

# far leg (behind the body)
FAR_LEG = ("M388 332 C 374 372 378 408 392 434 "
           "C 398 468 398 494 400 512")
A(f'<path d="{FAR_LEG}" fill="none" stroke="{ORANGE_DARK}" stroke-width="18" stroke-linecap="round"/>')
A(f'<path d="{FAR_LEG}" fill="none" stroke="{ORANGE}" stroke-width="11" stroke-linecap="round"/>')
A(f'<path d="M{PED_F[0]-20} {PED_F[1]-16} C {PED_F[0]-4} {PED_F[1]-23} {PED_F[0]+16} {PED_F[1]-19} {PED_F[0]+19} {PED_F[1]-6} '
  f'C {PED_F[0]+2} {PED_F[1]-4} {PED_F[0]-12} {PED_F[1]-6} {PED_F[0]-20} {PED_F[1]-16} Z" '
  f'fill="{ORANGE}" stroke="{ORANGE_DARK}" stroke-width="3.2" stroke-linejoin="round"/>')

# neck (drawn first: its base hides under the body / head)
NECK = "M406 212 C 402 172 436 132 480 112"
A(f'<path d="{NECK}" fill="none" stroke="{OUTLINE}" stroke-width="45" stroke-linecap="round"/>')
A(f'<path d="{NECK}" fill="none" stroke="url(#bodyG)" stroke-width="38" stroke-linecap="round"/>')

# wing-arm reaching the handlebar grip (base hidden under the body)
ARM = "M392 250 C 424 236 470 246 510 268 C 536 282 554 292 568 302"
A(f'<path d="{ARM}" fill="none" stroke="#a2b6c7" stroke-width="40" stroke-linecap="round"/>')
A(f'<path d="{ARM}" fill="none" stroke="#eaf2f7" stroke-width="32" stroke-linecap="round"/>')
A('<g fill="none" stroke="#c3d4e1" stroke-width="2.6" stroke-linecap="round" opacity="0.85">'
  '<path d="M446 258 C 470 268 496 282 520 296"/>'
  '<path d="M500 276 C 520 288 538 298 552 306"/></g>')
# feather fingers curling over the grip
A('<g fill="#e2ecf3" stroke="#a2b6c7" stroke-width="2.8" stroke-linejoin="round">'
  '<path d="M562 290 C 574 290 584 296 588 304 C 576 304 566 300 558 294 Z"/>'
  '<path d="M556 302 C 568 304 576 312 578 322 C 568 318 560 312 554 306 Z"/></g>')

# near leg (in front of the frame; top hidden by the body)
NEAR_LEG = "M400 338 C 406 374 428 396 452 406 C 476 416 488 434 498 450"
A(f'<path d="{NEAR_LEG}" fill="none" stroke="{ORANGE_DARK}" stroke-width="18" stroke-linecap="round"/>')
A(f'<path d="{NEAR_LEG}" fill="none" stroke="{ORANGE}" stroke-width="11" stroke-linecap="round"/>')
A(f'<path d="M{PED_N[0]-20} {PED_N[1]-16} C {PED_N[0]-4} {PED_N[1]-23} {PED_N[0]+16} {PED_N[1]-19} {PED_N[0]+19} {PED_N[1]-6} '
  f'C {PED_N[0]+2} {PED_N[1]-4} {PED_N[0]-12} {PED_N[1]-6} {PED_N[0]-20} {PED_N[1]-16} Z" '
  f'fill="{ORANGE}" stroke="{ORANGE_DARK}" stroke-width="3.2" stroke-linejoin="round"/>')

# body
BODY_PATH = ("M298 214 "
             "C 290 176 328 148 376 160 "
             "C 412 170 432 206 430 252 "
             "C 428 300 410 336 376 346 "
             "C 338 356 304 334 294 298 "
             "C 286 264 290 238 298 214 Z")
A(f'<path d="{BODY_PATH}" fill="url(#bodyG)" stroke="{OUTLINE}" stroke-width="4.5" stroke-linejoin="round"/>')
A('<path d="M340 338 C 376 350 406 340 420 310 C 428 292 431 272 431 254 '
  'C 424 288 410 320 372 334 C 358 339 348 339 340 338 Z" fill="#e6eef4" opacity="0.7"/>')

# folded wing on the flank
WING_PATH = ("M348 236 C 384 222 414 240 420 272 "
             "C 426 304 410 330 386 334 "
             "C 366 338 352 318 348 290 "
             "C 345 268 344 250 348 236 Z")
A(f'<path d="{WING_PATH}" fill="{WING}" stroke="{WING_LINE}" stroke-width="4" stroke-linejoin="round"/>')
A(f'<g fill="none" stroke="{WING_LINE}" stroke-width="3" stroke-linecap="round" opacity="0.95">'
  '<path d="M362 254 C 384 266 400 288 404 312"/>'
  '<path d="M358 274 C 378 286 392 306 396 326"/>'
  '<path d="M374 240 C 396 250 412 268 416 290"/></g>')
A('<path d="M378 330 C 380 342 378 352 372 360 C 366 348 364 338 364 330 Z" '
  'fill="#d5e3ee" stroke="#a2b6c7" stroke-width="3" stroke-linejoin="round"/>')

# scarf around the neck (band runs across the neck, knot on the near side)
A('<g id="scarf">')
A('<path d="M410 143 L 440 166" fill="none" stroke="#a82e38" stroke-width="24" stroke-linecap="butt"/>')
A('<path d="M410 143 L 440 166" fill="none" stroke="#e0454f" stroke-width="17" stroke-linecap="butt"/>')
A('<path d="M413 147 L 436 165" fill="none" stroke="#f58a92" stroke-width="4" stroke-linecap="butt" opacity="0.9"/>')
# knot
A('<circle cx="440" cy="166" r="9.5" fill="#e0454f" stroke="#a82e38" stroke-width="3.2"/>')
# tails fluttering back over the shoulder
A('<path d="M436 172 C 412 176 390 186 372 200 C 384 202 396 200 406 196 '
  'C 394 208 386 222 382 238 C 398 222 412 202 430 188 Z" '
  'fill="#ef5560" stroke="#a82e38" stroke-width="3.4" stroke-linejoin="round"/>')
A('<path d="M430 180 C 412 188 398 202 390 218 C 402 208 416 196 432 188 Z" '
  'fill="#d63b47" stroke="#a82e38" stroke-width="3" stroke-linejoin="round"/>')
A('</g>')

# head
A('<circle cx="498" cy="94" r="31" fill="url(#bodyG)" stroke="%s" stroke-width="4.5"/>' % OUTLINE)
# soft feather hint on the back of the head
A('<path d="M482 74 C 492 78 500 86 504 96" fill="none" stroke="#dbe6ee" stroke-width="3" stroke-linecap="round" opacity="0.9"/>')

# beak: pouch first, upper mandible on top
POUCH = ("M520 110 C 560 118 620 126 700 118 "
         "C 692 148 648 178 596 182 "
         "C 546 186 518 156 520 120 Z")
A(f'<path d="{POUCH}" fill="url(#pouchG)" stroke="#c47c14" stroke-width="3.6" stroke-linejoin="round"/>')
A('<path d="M534 138 C 570 156 616 164 662 150" fill="none" stroke="#ffc06d" stroke-width="3.4" stroke-linecap="round" opacity="0.85"/>')
A('<path d="M542 158 C 576 172 612 176 648 166" fill="none" stroke="#d9762a" stroke-width="2.6" stroke-linecap="round" opacity="0.65"/>')

BEAK = ("M516 86 C 570 72 646 76 708 92 "
        "C 716 94 719 102 710 105 "
        "C 656 104 572 106 516 114 Z")
A(f'<path d="{BEAK}" fill="url(#beakG)" stroke="#c47c14" stroke-width="3.6" stroke-linejoin="round"/>')
A('<path d="M524 84 C 574 73 642 78 700 94" fill="none" stroke="#ffe6a0" stroke-width="3" stroke-linecap="round"/>')
A('<path d="M520 106 C 574 100 646 98 706 100" fill="none" stroke="#c47c14" stroke-width="2.6" stroke-linecap="round" opacity="0.8"/>')
A('<path d="M700 92 C 713 94 717 101 710 106" fill="none" stroke="#a86410" stroke-width="3" stroke-linecap="round"/>')
# nail (hook) at the tip of the upper mandible
A('<path d="M702 90 C 710 92 714 99 710 105 C 706 108 701 104 699 97 Z" '
  'fill="#eb9a17" stroke="#c47c14" stroke-width="2.6" stroke-linejoin="round"/>')
# soft highlight on the pouch
A('<ellipse cx="608" cy="150" rx="46" ry="16" transform="rotate(12 608 150)" fill="#ffe9b0" opacity="0.35"/>')
# nostril
A('<path d="M548 88 C 558 87 566 88 572 91" fill="none" stroke="#c47c14" stroke-width="2.4" stroke-linecap="round" opacity="0.8"/>')

# eye
A('<circle cx="512" cy="84" r="7.5" fill="#2b3240"/>')
A('<circle cx="509.5" cy="81" r="2.6" fill="#ffffff"/>')
A('<path d="M500 74 C 506 68 516 67 523 72" fill="none" stroke="#aabccb" stroke-width="3" stroke-linecap="round"/>')

A('</g>')
A('</svg>')

open(OUT, "w", encoding="utf-8").write("\n".join(svg) + "\n")
print("written", OUT)
