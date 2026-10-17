"""The Signal Function Set mark, as the designer draws it on every panel.

Eleven stroked paths -- a stepped wave, a curve and a bracketed 2x2 matrix --
copied verbatim from res/canon.svg, in that file's own units (11.811 to the mm,
which is also the Figma template's 4x scale). The mark's top left corner is at
(1331, 75) in those units, so a placement translates it there and scales it.

Measured across the designer's 2026-09 panels (canon, count, spool, field,
flock, sigma, wheel...): always 14.33 x 2.54 mm, its top 6.35 mm (1.25 HP)
down, its right edge one HP in from the panel's (half an HP on an 8HP panel),
stroked #4A4A4A at 0.127 mm. Its centre (7.62 mm) is the title's cap centre.
"""

UNITS_PER_MM = 11.811
W_MM, H_MM = 14.33, 2.54
TOP_MM = 6.35
ORIGIN = (1331.0, 75.0)

PATHS = [
    '<path d="M1331 90.0028H1336V85.0019H1341V95.0037H1346V80.0009H1351V100.005H1356V75H1361V100.005H1366V85.0019H1371V95.0037H1376V90.0028H1381" stroke="#4A4A4A" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" fill="none"/>',
    '<path d="M1388.38 100.005L1403.37 75C1403.37 80.4594 1407.58 90.0028 1413.38 90.0028H1433.38L1438.38 100.005" stroke="#4A4A4A" stroke-width="1.5" stroke-linecap="square" stroke-linejoin="bevel" fill="none"/>',
    '<path d="M1455.21 75H1450.21V105.006H1455.21" stroke="#4A4A4A" stroke-width="1.5" stroke-linecap="square" stroke-linejoin="bevel" fill="none"/>',
    '<path d="M1495.21 75H1500.21V105.006H1495.21" stroke="#4A4A4A" stroke-width="1.5" stroke-linecap="square" stroke-linejoin="bevel" fill="none"/>',
    '<path d="M1460.21 77.6255L1470.21 87.6274" stroke="#4A4A4A" stroke-width="1.5" stroke-linejoin="bevel" fill="none"/>',
    '<path d="M1470.21 77.6255L1460.21 87.6274" stroke="#4A4A4A" stroke-width="1.5" stroke-linejoin="bevel" fill="none"/>',
    '<path d="M1460.21 92.6284L1470.21 102.63" stroke="#4A4A4A" stroke-width="1.5" stroke-linejoin="bevel" fill="none"/>',
    '<path d="M1460.21 102.63L1470.21 92.6284" stroke="#4A4A4A" stroke-width="1.5" stroke-linejoin="bevel" fill="none"/>',
    '<path d="M1480.21 77.6255L1490.21 87.6274" stroke="#4A4A4A" stroke-width="1.5" stroke-linejoin="bevel" fill="none"/>',
    '<path d="M1490.21 77.6255L1480.21 87.6274" stroke="#4A4A4A" stroke-width="1.5" stroke-linejoin="bevel" fill="none"/>',
    '<path d="M1490.21 92.6284H1480.21V102.63H1490.21V92.6284Z" stroke="#4A4A4A" stroke-width="1.5" stroke-linejoin="bevel" fill="none"/>',
]


def margin_mm(panel_w_mm):
    """The side margin the designer uses: 1 HP, or half of one on 8HP and under."""
    return 2.54 if panel_w_mm <= 8 * 5.08 + 0.01 else 5.08


def group(panel_w_mm, upmm, indent="    "):
    """The mark placed top right on a panel `panel_w_mm` wide, in a file with
    `upmm` units to the mm."""
    x = (panel_w_mm - margin_mm(panel_w_mm) - W_MM) * upmm
    y = TOP_MM * upmm
    k = upmm / UNITS_PER_MM
    head = (f'{indent}<g id="Logo" transform="translate({x:.3f} {y:.3f}) '
            f'scale({k:.6f}) translate({-ORIGIN[0]:g} {-ORIGIN[1]:g})">')
    return "\n".join([head] + [indent + "  " + p for p in PATHS] + [indent + "</g>"])
