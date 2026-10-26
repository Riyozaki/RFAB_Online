#!/usr/bin/env python3
"""Генерирует SVG «пеликан на велосипеде» (1200x900) — тест консистентности."""
import math

W, H = 1200, 900
REAR, FRONT, R_TIRE = (355, 640), (855, 640), 140


def spokes(cx, cy, n=24, r0=16, r1=116, w=3.0, color="#a9b7bd"):
    return "\n".join(
        f'<line x1="{cx + r0*math.cos(math.radians(180*i/n)):.1f}" y1="{cy + r0*math.sin(math.radians(180*i/n)):.1f}" '
        f'x2="{cx + r1*math.cos(math.radians(180*i/n)):.1f}" y2="{cy + r1*math.sin(math.radians(180*i/n)):.1f}" '
        f'stroke="{color}" stroke-width="{w}"/>'
        for i in range(n))


def wheel(cx, cy, ident):
    return f'''
  <g id="{ident}">
    <circle cx="{cx}" cy="{cy}" r="{R_TIRE}" fill="none" stroke="#2e3238" stroke-width="21"/>
    <circle cx="{cx}" cy="{cy}" r="{R_TIRE+10.5}" fill="none" stroke="#1d2126" stroke-width="2" opacity="0.45"/>
    <circle cx="{cx}" cy="{cy}" r="{R_TIRE-10.5}" fill="none" stroke="#4a5057" stroke-width="2" opacity="0.5"/>
    <circle cx="{cx}" cy="{cy}" r="{R_TIRE-19}" fill="none" stroke="#dbe2e6" stroke-width="8"/>
    <circle cx="{cx}" cy="{cy}" r="{R_TIRE-23}" fill="none" stroke="#98a6ac" stroke-width="2" opacity="0.65"/>
    {spokes(cx, cy)}
    <circle cx="{cx}" cy="{cy}" r="17" fill="#8d9aa0" stroke="#5d686e" stroke-width="3"/>
    <circle cx="{cx}" cy="{cy}" r="7" fill="#4d575c"/>
  </g>'''


svg = f'''<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">
  <defs>
    <linearGradient id="skyGrad" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#c7e5f2"/><stop offset="0.55" stop-color="#e2f2f8"/><stop offset="1" stop-color="#f5fbfd"/>
    </linearGradient>
    <radialGradient id="sunGlow" cx="0.5" cy="0.5" r="0.5">
      <stop offset="0" stop-color="#ffe9a8" stop-opacity="0.9"/>
      <stop offset="0.45" stop-color="#ffe9a8" stop-opacity="0.38"/>
      <stop offset="1" stop-color="#ffe9a8" stop-opacity="0"/>
    </radialGradient>
    <linearGradient id="groundGrad" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#e9f0e3"/><stop offset="0.35" stop-color="#dde7d6"/><stop offset="1" stop-color="#ccd8c4"/>
    </linearGradient>
    <linearGradient id="bodyVol" x1="0.22" y1="0.02" x2="0.72" y2="1">
      <stop offset="0" stop-color="#ffffff"/><stop offset="0.5" stop-color="#f6f8fa"/><stop offset="1" stop-color="#d3dce3"/>
    </linearGradient>
    <linearGradient id="neckGrad" x1="0.1" y1="0" x2="0.9" y2="1">
      <stop offset="0" stop-color="#ffffff"/><stop offset="0.6" stop-color="#f2f6f8"/><stop offset="1" stop-color="#d7e0e6"/>
    </linearGradient>
    <linearGradient id="headGrad" x1="0.2" y1="0" x2="0.8" y2="1">
      <stop offset="0" stop-color="#ffffff"/><stop offset="1" stop-color="#e5ebef"/>
    </linearGradient>
    <linearGradient id="wingGrad" x1="0.1" y1="0" x2="0.6" y2="1">
      <stop offset="0" stop-color="#fbfdfe"/><stop offset="0.45" stop-color="#eaf1f4"/><stop offset="1" stop-color="#c2ced6"/>
    </linearGradient>
    <linearGradient id="beakGrad" x1="0" y1="0" x2="0.55" y2="1">
      <stop offset="0" stop-color="#ffd15c"/><stop offset="0.45" stop-color="#f7a52c"/><stop offset="1" stop-color="#db7a17"/>
    </linearGradient>
    <linearGradient id="pouchGrad" x1="0.15" y1="0.1" x2="0.75" y2="1">
      <stop offset="0" stop-color="#ffd588"/><stop offset="0.5" stop-color="#f3a950"/><stop offset="1" stop-color="#dd8631"/>
    </linearGradient>
    <linearGradient id="frameGrad" x1="0" y1="0" x2="0.4" y2="1">
      <stop offset="0" stop-color="#2f9aa0"/><stop offset="0.55" stop-color="#1a757c"/><stop offset="1" stop-color="#10575e"/>
    </linearGradient>
    <linearGradient id="scarfGrad" x1="0" y1="0" x2="1" y2="0.7">
      <stop offset="0" stop-color="#e2564a"/><stop offset="0.6" stop-color="#cf4438"/><stop offset="1" stop-color="#a83229"/>
    </linearGradient>
    <linearGradient id="legGrad" x1="0" y1="0" x2="0.3" y2="1">
      <stop offset="0" stop-color="#f4a355"/><stop offset="1" stop-color="#d9762a"/>
    </linearGradient>
    <filter id="soft" x="-30%" y="-120%" width="160%" height="340%"><feGaussianBlur stdDeviation="9"/></filter>
    <filter id="soft3" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="3.2"/></filter>
  </defs>

  <!-- ================= BACKGROUND ================= -->
  <rect width="{W}" height="{H}" fill="url(#skyGrad)"/>
  <circle cx="1040" cy="170" r="185" fill="url(#sunGlow)"/>
  <circle cx="1040" cy="170" r="72" fill="#fff4c8" opacity="0.95"/>
  <circle cx="1040" cy="170" r="58" fill="#fffbe8"/>

  <g fill="#ffffff" opacity="0.85">
    <path d="M 150 170 C 150 148 172 138 190 146 C 200 124 236 120 250 142 C 272 134 292 148 290 168 C 306 170 310 188 296 196 L 168 196 C 150 196 140 182 150 170 Z"/>
    <path d="M 430 92 C 430 76 446 68 460 74 C 468 58 494 56 504 72 C 520 66 534 76 532 90 C 544 92 546 106 536 112 L 444 112 C 430 112 422 100 430 92 Z" opacity="0.7"/>
    <path d="M 640 176 C 640 166 652 160 662 164 C 668 154 686 152 694 163 C 704 159 714 167 712 176 L 646 180 Z" opacity="0.55"/>
  </g>

  <g stroke="#8fa9a4" stroke-width="3.2" fill="none" stroke-linecap="round" opacity="0.45">
    <path d="M 300 250 q 13 -10 26 0"/><path d="M 314 250 q 13 -10 26 0"/>
    <path d="M 366 214 q 10 -8 20 0"/><path d="M 376 214 q 10 -8 20 0"/>
  </g>

  <path d="M 0 668 C 120 596 246 592 344 636 C 412 668 470 664 538 636 C 632 598 742 590 838 630 C 930 668 1060 664 1200 626 L 1200 706 L 0 706 Z" fill="#cfe0d2"/>
  <path d="M 0 676 C 150 636 250 650 350 676 C 470 708 590 690 700 662 C 810 634 940 646 1040 678 C 1120 704 1160 700 1200 690 L 1200 730 L 0 730 Z" fill="#bfd4c4"/>

  <rect x="0" y="700" width="{W}" height="200" fill="url(#groundGrad)"/>
  <path d="M 0 700 C 260 692 470 710 700 702 C 900 695 1060 706 1200 700 L 1200 712 L 0 712 Z" fill="#c2d1b8" opacity="0.9"/>

  <g opacity="0.45" stroke="#9fb394" stroke-width="3" stroke-linecap="round" fill="none">
    <path d="M 120 742 v -18"/><path d="M 126 744 q 3 -14 12 -18"/>
    <path d="M 990 754 v -20"/><path d="M 997 756 q 4 -15 13 -20"/>
    <path d="M 1104 726 v -14"/><path d="M 1110 728 q 3 -11 10 -14"/>
  </g>
  <g fill="#b7c7ab" opacity="0.55">
    <ellipse cx="240" cy="812" rx="16" ry="7"/><ellipse cx="300" cy="846" rx="11" ry="5"/>
    <ellipse cx="930" cy="806" rx="18" ry="7"/><ellipse cx="1050" cy="852" rx="12" ry="5"/>
    <ellipse cx="640" cy="866" rx="22" ry="8"/>
  </g>

  <ellipse cx="610" cy="790" rx="330" ry="21" fill="#7e9078" opacity="0.36" filter="url(#soft)"/>

  <!-- ================= WHEELS ================= -->
  {wheel(*REAR, "wheelRear")}
  {wheel(*FRONT, "wheelFront")}

  <g stroke="#7d8a90" stroke-width="4.5" stroke-dasharray="7 5" fill="none" opacity="0.95">
    <path d="M 605 625 L 355 626"/><path d="M 605 705 L 355 653"/>
  </g>
  <circle cx="355" cy="640" r="15" fill="#aab6bb" stroke="#6d797f" stroke-width="3"/>
  <circle cx="355" cy="640" r="5" fill="#59646a"/>

  <!-- far leg behind frame -->
  <path d="M 486 386 C 502 446 512 494 518 528" stroke="#c9762f" stroke-width="13" fill="none" stroke-linecap="round"/>
  <path d="M 518 528 C 526 582 532 628 536 656" stroke="#c9762f" stroke-width="9" fill="none" stroke-linecap="round"/>
  <g transform="translate(546,678) rotate(6)">
    <path d="M -24 -5 C -31 1 -30 10 -20 11 L 22 9 C 33 8 37 1 30 -4 C 21 -9 -10 -10 -24 -5 Z"
          fill="#bb6c29" stroke="#9c5821" stroke-width="2" stroke-linejoin="round"/>
    <path d="M -2 5 C 6 4 14 2 20 0" stroke="#9c5821" stroke-width="2" fill="none" opacity="0.6"/>
  </g>

  <!-- ================= FRAME ================= -->
  <g stroke="url(#frameGrad)" fill="none" stroke-linecap="round">
    <path d="M 605 665 L 355 640" stroke-width="12"/>
    <path d="M 540 455 L 355 640" stroke-width="10"/>
    <path d="M 540 455 L 607 668" stroke-width="15"/>
    <path d="M 607 668 L 790 458" stroke-width="17"/>
    <path d="M 548 466 L 800 420" stroke-width="15"/>
    <path d="M 800 418 L 786 462" stroke-width="19"/>
    <path d="M 790 452 C 812 520 836 586 856 642" stroke-width="13"/>
  </g>
  <g stroke="#5fc0c4" fill="none" stroke-linecap="round" opacity="0.5">
    <path d="M 600 657 L 372 636" stroke-width="3"/>
    <path d="M 552 462 L 793 421" stroke-width="3"/>
    <path d="M 612 662 L 787 462" stroke-width="3"/>
  </g>

  <circle cx="605" cy="665" r="40" fill="none" stroke="#b8c3c8" stroke-width="7"/>
  <circle cx="605" cy="665" r="40" fill="none" stroke="#7c898f" stroke-width="2" stroke-dasharray="5 6"/>
  <circle cx="605" cy="665" r="13" fill="#8d9aa0" stroke="#5d686e" stroke-width="3"/>

  <g stroke="#77848a" stroke-width="9" stroke-linecap="round">
    <path d="M 605 665 L 547 686"/><path d="M 605 665 L 663 644"/>
  </g>
  <circle cx="605" cy="665" r="6" fill="#4d575c"/>

  <g transform="rotate(-20 663 644)">
    <rect x="640" y="639" width="52" height="13" rx="5.5" fill="#3c4247"/>
    <rect x="640" y="639" width="52" height="5.5" rx="2.7" fill="#5c646a"/>
  </g>
  <g transform="rotate(-20 547 686)">
    <rect x="524" y="681" width="50" height="13" rx="5.5" fill="#3c4247"/>
    <rect x="524" y="681" width="50" height="5.5" rx="2.7" fill="#5c646a"/>
  </g>

  <path d="M 524 428 L 545 460" stroke="#6d797f" stroke-width="9" stroke-linecap="round"/>
  <path d="M 464 424 C 462 402 494 390 526 394 C 554 398 576 408 570 420 C 550 432 490 436 464 424 Z" fill="#4a3b34" stroke="#2f2622" stroke-width="2.5" stroke-opacity="0.35"/>
  <path d="M 478 410 C 498 400 534 398 558 406" stroke="#6e5a4e" stroke-width="4" fill="none" opacity="0.8" stroke-linecap="round"/>

  <path d="M 800 418 L 812 386" stroke="#6d797f" stroke-width="11" stroke-linecap="round"/>
  <path d="M 812 388 C 828 354 806 334 772 330 L 738 328" stroke="#2b2f34" stroke-width="11" fill="none" stroke-linecap="round"/>
  <path d="M 786 332 L 738 328" stroke="#1a1d21" stroke-width="15" fill="none" stroke-linecap="round"/>
  <circle cx="830" cy="352" r="11" fill="#f0c04a" stroke="#b98f22" stroke-width="2.5"/>
  <circle cx="826.5" cy="348.5" r="3.4" fill="#fff3c4"/>

  <!-- ================= PELICAN ================= -->
  <!-- tail -->
  <path d="M 392 258 C 352 240 312 248 288 274 C 314 276 336 286 350 300
           C 328 306 312 320 306 338 C 342 334 378 316 400 288 Z"
        fill="#f0f4f6" stroke="#c7d1d8" stroke-width="2.5" stroke-linejoin="round"/>
  <g stroke="#c7d1d8" stroke-width="2" fill="none" opacity="0.65" stroke-linecap="round">
    <path d="M 320 276 C 336 283 350 292 360 302"/>
    <path d="M 316 330 C 334 322 352 308 366 292"/>
  </g>

  <!-- far wing (folded behind body) -->
  <path d="M 588 232 C 538 214 478 220 438 244 C 412 260 396 284 394 306
           C 412 298 432 294 450 296 C 446 314 448 330 456 342
           C 474 322 498 304 526 292 C 554 280 578 256 588 232 Z"
        fill="#dbe3ea" stroke="#c1ccd5" stroke-width="2.5" stroke-linejoin="round"/>

  <!-- near wing reaching handlebar -->
  <path d="M 590 272 C 634 276 678 296 716 314
           C 736 323 754 331 766 339
           C 776 345 772 357 760 358
           C 748 359 734 354 724 348
           C 690 350 654 354 626 358
           C 606 361 594 348 594 328
           C 594 308 592 288 590 272 Z"
        fill="url(#wingGrad)" stroke="#aebbc4" stroke-width="2.5" stroke-linejoin="round"/>
  <g stroke="#9fadb7" stroke-width="2.2" fill="none" opacity="0.65" stroke-linecap="round">
    <path d="M 640 288 C 674 302 704 318 730 334"/>
    <path d="M 648 306 C 682 314 706 324 728 340"/>
    <path d="M 636 322 C 670 328 698 336 722 346"/>
  </g>
  <path d="M 612 282 C 654 286 696 302 730 320" stroke="#ffffff" stroke-width="4.5" fill="none" opacity="0.85" stroke-linecap="round"/>
  <path d="M 752 334 C 766 340 772 350 762 356 C 754 360 746 352 744 344 Z" fill="#e8eff3" stroke="#aebbc4" stroke-width="2.2"/>

  <!-- body -->
  <g transform="rotate(-12 472 294)">
    <ellipse cx="472" cy="294" rx="124" ry="96" fill="url(#bodyVol)" stroke="#ccd6dd" stroke-width="2.5"/>
    <path d="M 366 344 C 406 386 482 396 544 374 C 490 392 418 374 366 344 Z" fill="#c6d1d9" opacity="0.5"/>
    <path d="M 374 236 C 428 208 518 210 566 240" stroke="#ffffff" stroke-width="7" fill="none" opacity="0.9" stroke-linecap="round"/>
  </g>
  <g stroke="#cfd9df" stroke-width="2.2" fill="none" opacity="0.45" stroke-linecap="round">
    <path d="M 528 330 C 538 348 546 364 550 380"/>
    <path d="M 552 322 C 562 342 570 358 574 374"/>
    <path d="M 506 334 C 514 350 520 364 524 378"/>
  </g>

  <!-- scarf tails over the back -->
  <path d="M 616 172 C 584 150 546 152 516 168 C 534 174 544 182 550 192
           C 526 192 504 200 490 214 C 520 218 544 226 560 240
           C 578 218 600 200 622 190 Z"
        fill="url(#scarfGrad)" stroke="#8f2b23" stroke-width="2.5" stroke-linejoin="round"/>
  <path d="M 534 164 C 562 157 590 160 612 170" stroke="#f4887c" stroke-width="3.2" fill="none" opacity="0.45" stroke-linecap="round"/>
  <path d="M 604 192 C 578 190 552 196 530 208 C 546 212 558 218 566 226 C 588 218 602 206 610 200 Z"
        fill="#b93a30" stroke="#8f2b23" stroke-width="2.2" stroke-linejoin="round"/>

  <!-- neck (long S-curve) -->
  <path d="M 536 300 C 556 224 584 168 640 132 L 676 100
           C 706 128 706 162 684 182 C 640 194 606 232 592 296
           C 574 328 538 326 536 300 Z"
        fill="url(#neckGrad)" stroke="#d3dce2" stroke-width="2.5" stroke-linejoin="round"/>
  <path d="M 568 268 C 582 216 606 176 650 148" stroke="#c9d3da" stroke-width="3.4" fill="none" opacity="0.35" stroke-linecap="round"/>

  <!-- head -->
  <path d="M 660 108 C 654 86 642 70 624 60 C 642 82 648 98 650 116 C 654 113 657 110 660 108 Z"
        fill="#eef3f5" stroke="#ccd6dd" stroke-width="2.5" stroke-linejoin="round"/>
  <circle cx="700" cy="128" r="54" fill="url(#headGrad)" stroke="#dbe3e8" stroke-width="2"/>
  <path d="M 670 82 C 684 76 714 77 728 86" stroke="#ffffff" stroke-width="6" fill="none" opacity="0.9" stroke-linecap="round"/>

  <!-- pouch -->
  <path d="M 742 148 C 730 218 770 278 828 276 C 868 274 894 222 916 172 C 850 184 786 172 742 148 Z"
        fill="url(#pouchGrad)" stroke="#c9792c" stroke-width="2.5" stroke-linejoin="round"/>
  <path d="M 760 184 C 768 232 796 262 830 264" stroke="#c9792c" stroke-width="2.4" fill="none" opacity="0.4"/>
  <path d="M 788 192 C 800 228 820 250 848 258" stroke="#ffdcaa" stroke-width="3" fill="none" opacity="0.4"/>

  <!-- upper mandible -->
  <path d="M 738 104 C 802 110 874 136 914 154 C 924 158 926 170 914 172 L 742 148 Z"
        fill="url(#beakGrad)" stroke="#c9792c" stroke-width="2.5" stroke-linejoin="round"/>
  <path d="M 912 152 C 924 156 927 170 913 173 C 907 164 907 159 912 152 Z" fill="#c9792c"/>
  <path d="M 746 112 C 802 118 862 137 898 153" stroke="#ffe4a8" stroke-width="4" fill="none" opacity="0.85" stroke-linecap="round"/>
  <ellipse cx="770" cy="115" rx="6.5" ry="3.2" fill="#b96a1d" opacity="0.6" transform="rotate(10 770 115)"/>

  <!-- eye -->
  <circle cx="684" cy="117" r="12" fill="#f7f3e7"/>
  <circle cx="685" cy="118" r="6.6" fill="#232c33"/>
  <circle cx="682.4" cy="115.2" r="2.3" fill="#ffffff"/>
  <path d="M 670 105 C 678 100 692 101 700 107" stroke="#c2ccd4" stroke-width="2.8" fill="none" stroke-linecap="round"/>

  <!-- scarf band + knot -->
  <path d="M 600 168 L 634 214" stroke="url(#scarfGrad)" stroke-width="19" stroke-linecap="round"/>
  <path d="M 606 172 L 620 194" stroke="#f4887c" stroke-width="3" stroke-linecap="round" opacity="0.45"/>
  <ellipse cx="602" cy="164" rx="15" ry="12" fill="#cf4438" stroke="#8f2b23" stroke-width="2.5" transform="rotate(-26 602 164)"/>
  <path d="M 594 158 C 599 153 608 153 613 158" stroke="#f4887c" stroke-width="3" fill="none" opacity="0.6" stroke-linecap="round"/>

  <!-- near leg -->
  <path d="M 494 380 C 520 424 544 466 562 500" stroke="url(#legGrad)" stroke-width="15" fill="none" stroke-linecap="round"/>
  <path d="M 562 500 C 588 546 616 594 636 622" stroke="url(#legGrad)" stroke-width="10.5" fill="none" stroke-linecap="round"/>
  <circle cx="562" cy="500" r="6.6" fill="#e9954a"/>
  <path d="M 508 390 C 524 416 538 442 548 464" stroke="#ffc08a" stroke-width="4" fill="none" opacity="0.5" stroke-linecap="round"/>

  <!-- near foot on pedal -->
  <g transform="translate(670,630) rotate(-10)">
    <path d="M -26 -6 C -34 0 -33 11 -22 12 L 24 10 C 36 9 40 1 32 -5 C 22 -11 -11 -12 -26 -6 Z"
          fill="#e58a3c" stroke="#c06f28" stroke-width="2.5" stroke-linejoin="round"/>
    <path d="M -4 6 C 4 5 14 3 22 1" stroke="#c06f28" stroke-width="2.2" fill="none" opacity="0.7"/>
    <path d="M -6 -2 C 2 -3 12 -4 20 -6" stroke="#ffbe8a" stroke-width="2.6" fill="none" opacity="0.55" stroke-linecap="round"/>
  </g>

  <!-- ================= MOTION ================= -->
  <g stroke="#a7c0cb" stroke-width="9" stroke-linecap="round" opacity="0.5" fill="none">
    <path d="M 60 372 L 208 372"/><path d="M 96 452 L 244 452"/><path d="M 46 532 L 176 532"/>
  </g>
  <g stroke="#ffffff" stroke-width="7" stroke-linecap="round" opacity="0.65" fill="none">
    <path d="M 132 412 L 216 412"/><path d="M 156 492 L 222 492"/>
  </g>

  <g fill="#ffffff" filter="url(#soft3)" opacity="0.85">
    <circle cx="300" cy="752" r="15"/><circle cx="262" cy="768" r="11"/><circle cx="222" cy="780" r="7.5"/>
  </g>
  <g fill="#dbe3d2" opacity="0.7"><circle cx="880" cy="800" r="5"/><circle cx="935" cy="792" r="3.4"/></g>
</svg>
'''

if __name__ == "__main__":
    import os
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "pelican_on_bicycle.svg")
    with open(out, "w", encoding="utf-8") as fh:
        fh.write(svg)
    print("ok", out, len(svg))
