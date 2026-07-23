# Global Drum Patterns — Country-by-Country Reference

A research-backed survey of traditional and popular drum/percussion patterns
worldwide, compiled for the SFS drum pattern library. Step placements are given
in box/TUBS notation and 0-indexed step lists ready for a 16-step (straight
16ths) or 12-step (12/8 triplet) sequencer grid; odd (aksak) meters use their
natural pulse count. Patterns marked **[verified]** have placements confirmed
against notation-bearing sources by adversarial cross-checking; **[stylized]**
marks conventional approximations assembled from pedagogy sources and genre
convention rather than a single authoritative notation. Everything here is a
genre convention or folkloric timeline, not a transcription of any recording.

Notation: `x` = stroke, `X` = accented stroke, `.` = rest. Grids read left to
right, one character per step, starting on the downbeat (rotation flags noted
where traditions disagree). "IOI" = inter-onset intervals.

## The timeline concept (read first)

Research strongly supports a two-class model for this library:

**Cyclic timelines (key patterns)** — short asymmetric ostinatos (clave, bell
patterns, madera, ti-bwa, compás) that regulate the whole ensemble and never
vary. Toussaint's geometric analyses catalog the two great families: six
5-onset/16-pulse binary timelines (Shiko, Son, Soukous, Rumba, Bossa, Gahu) and
ten 7-onset/12-pulse ternary bell timelines, all maximally even distributions.
**Groove patterns** — the variable drum parts (tumbao, surdo lines, kit
grooves) that lock to a timeline or backbeat and admit variation, fills, and
intensity tiers.

The library encodes timelines as standalone one-lane patterns (`kind:
"timeline"`) and grooves as multi-lane sets.

The 16-pulse binary timeline family **[verified — Toussaint]**, 0-indexed:

```
Son      x..x..x...x.x...   0,3,6,10,12   (IOI 3-3-4-2-4)
Rumba    x..x...x..x.x...   0,3,7,10,12   (son with 3rd stroke a 16th late)
Bossa    x..x..x...x..x..   0,3,6,10,13
Shiko    x...x.x...x.x...   0,4,6,10,12
Soukous  x..x..x..xx.....   0,3,6,9,10
Gahu     x..x..x...x...x.   0,3,6,10,14
```

The 12-pulse ternary bell family **[verified — Toussaint]**, 0-indexed:

```
Bembé (standard pattern)  x.x.xx.x.x.x   0,2,4,5,7,9,11  (IOI 2-2-1-2-2-2-1)
Soli                      x.x.x.x.xx.x   0,2,4,6,8,9,11
Tambú                     x.x.x.xx.x.x   0,2,4,6,7,9,11
Bembé-2                   xx.x.xx.x.x.   0,1,3,5,6,8,10
Yoruba                    x.x.xx.x.xx.   0,2,4,5,7,9,10
Tonada                    x.xx.xx.x.x.   0,2,3,5,6,8,10
Asaadua                   x.x.x.xx.xx.   0,2,4,6,7,9,10
Sorsonet                  xxx.x.x.x.x.   0,1,2,4,6,8,10
Bemba                     x.xx.x.x.xx.   0,2,3,5,7,9,10
Ashanti                   x.xx.x.xx.x.   0,2,3,5,7,8,10
```

Short building-block cells (8-pulse, double for a 16 grid):

```
Tresillo   x..x..x.   0,3,6      (3+3+2 — the seed of dembow, baião, malfuf)
Cinquillo  x.xx.xx.   0,2,3,5,6  (danzón baqueteo, Haitian ti-bwa, klezmer)
Habanera   x..xx.x.   0,3,4,6    (tango/milonga, early jazz "Spanish tinge")
```

**Placement disagreement to flag globally**: Agawu (JAMS 2006) documents that
the same standard-pattern interval cycle is anchored differently by different
cultures — Anlo-Ewe hearing the regulative beat at element 1, Yoruba at
element 4 — so ternary bell entries should be treated as rotation families;
and the regulative pulse is the dotted quarter (4 beats × 3), not 12 equal
counts. Encode as beats=4, stepsPerBeat=3.

---

# AFRICA

## Ghana (Ewe, Ga, Ashanti)

**Gankogui standard pattern** (agbekor, bembé family) **[verified]** — 12/8
timeline above; carried on the double iron bell, with axatse rattle doubling
and a 4-pulse dance beat underneath. ~100-140 bpm (dotted-quarter pulse).

**Gahu** **[verified — Toussaint; Pocket Operations]** — 16-pulse social dance
from the Ewe; bell at 0,3,6,10,14. Pocket Operations adds a programmable kit
reduction: bass drum 0,4,8,12,14, sticks on 2,3,6,7,10,11,14,15. ~110-130.

**Kpanlogo** (Ga recreational, 16 pulses) **[stylized]** — bell plays the son
timeline (0,3,6,10,12); low conga-type drums answer across the bar. ~110-130.

**Highlife** (popular, 4/4 16) **[stylized]** — clave-derived bell/woodblock
(son positions), kick 0 and 8, rim 4 and 12, shaker 8ths; the guitar carries
most syncopation. ~100-125.

## Nigeria

**Yoruba bell / konkolo** **[verified — Toussaint]** — the Yoruba ternary
timeline x.x.xx.x.xx. (0,2,4,5,7,9,10); used in sacred repertoire and (per
Toussaint) in the Cuban columbia. Rotation disagreement with Ewe usage
flagged above.

**Shiko** **[verified — Toussaint/Pocket Operations]** — 16-pulse timeline
0,4,6,10,12 with bass drum 0,4,6,8,12,14.

**Afrobeat** (popular, 4/4 16, ~100-126) **[stylized]** — Tony Allen-school
kit: broken kick (0,7,10), ghosted snare, continuous 16th hats with offbeat
opens, clave-like bell. Already encoded in library v1 (`global.afrobeat`).

**Afrobeats (modern pop)** **[stylized]** — dembow-derived: kick on tresillo
positions (0,3,6 doubled), rim/snare 3-3-2 figures, ~100-110.

## Guinea / Mali / Senegal (Mandé djembe belt)

The Paul Nas / WAP Pages compilation **[verified as a source]** documents 150+
named rhythms in exactly the two grids this library uses: binary = 16 pulses,
ternary = 12 pulses, scored per instrument (djembe bass/tone/slap; kenkeni,
sangban, dununba, bells). Signature repertoire: **Kuku** (binary, harvest
dance), **Djole** (binary, mask dance), **Kassa** (binary, field work),
**Soli** (ternary, initiation — its bell is the Soli timeline above),
**Tiriba**, **Dununba** family ("dance of the strong men"), **Mendiani**,
**Fanga**, **Yankadi** (documented in BOTH ternary and binary versions — a
subdivision disagreement worth flagging). Dunun lanes pair a drum stroke
(open/muted) with a bell stroke — encode as paired lanes. Senegal adds the
sabar/mbalax tradition (talking-drum led, bakks phrases) **[stylized]**.

## Congo (DRC)

**Soukous timeline** **[verified — Toussaint]** — 0,3,6,9,10; the clustered
9,10 pair gives the distinctive "limp". Modern soukous/ndombolo kit: four-floor
kick ~130-160 with snare/rim seben patterns 16ths **[stylized]**.

## Morocco

**Gnawa** (ternary, 12 pulses) **[stylized]** — iron qraqeb castanets play a
galloping x.xx.xx.xx.x-type cell over a low tbel/guembri pulse on the dotted
beats; ceremonies accelerate continuously ~90→140. Grid: beats=4, spb=3.

## South Africa

**Amapiano** (~110-115, 4/4 16) **[stylized]** — sparse kick, log-drum bass
playing syncopated tresillo-leaning melodic hits (e.g. 3,6,10,13), shaker
16ths, rim clicks; half-time feel over house tempo.
**Gqom** (~120-128) **[stylized]** — broken kick in 3+3+2 (0,3,6 + displaced
copies), no four-floor, sparse claps, dark tom stabs.
**Kwaito** (~100-110) **[stylized]** — slowed house: four-floor kick, offbeat
open hats, heavy bass.

## Zimbabwe / Central-Southern Africa

**Bemba clap pattern** **[verified — Toussaint]** — ternary timeline
x.xx.x.x.xx. (0,2,3,5,7,9,10), attested as a hand-clap/axe-blade pattern in
Northern Zimbabwe; Jones's rotational reduction <23223>/<22323> spans Bemba,
Lala, Ila, Tonga usage.

## Ethiopia, Kenya/Tanzania (report-only)

Ethiopian secular grooves ride a 6/8 "chik-chik" feel (kebero drum, triplet
grid) **[stylized]**; Swahili-coast taarab/chakacha uses a 12/8 women's dance
rhythm **[stylized]**; Kenyan benga is guitar-led with light 16th kit. Thin
notation sources — encode later if needed.

---

# THE AMERICAS

## Cuba

**Son clave / rumba clave** **[verified]** — see timeline table; 12/8 triplet
variants exist for both (son 12/8: x.x.x..x.x..). The 3-2/2-3 terminology is
Cuban-popular only — folkloric and African musics don't use it.
**Cáscara** (timbale shell, 2-3/3-2) **[stylized placements, widely taught]**
— 3-2: 0,2,3,5,7,8,10,12,13,15 against clave.
**Tumbao (conga marcha)** **[verified — Rhythm Notes]** — slap on beat 2
(step 4), two open tones on beat 4 (steps 12 and 14), heel-toe filler on the
remaining 16ths; clave-aligned variants put the **bombo** (low tumba accent)
on the "and" of 2 on the three-side (step 6) and the **ponche** on beat 4
(step 12). (v1 of our library had the opens at 14,15 — corrected to 12,14 in v2.)
**Bembé** 12/8 **[verified]** — standard bell on guataca, in batá liturgy.
**Danzón baqueteo** **[verified derivation]** — cinquillo doubled across the
clave cycle. **Songo / timba** **[stylized]** — kit hybrids over clave,
documented in 21-variation pedagogy sources. Tempo: son ~85-110, rumba
guaguancó ~95-130, timba 90-105.

## Brazil

**Samba** **[verified — microtiming studies]** — meter is 2/4 with a 16th
tatum (a 16-step grid = 2 bars). Surdo marks **beat 2** (not four-on-floor);
caixa fills 16ths with ghosts; tamborim carries telecoteco figures. Measured
microtiming: 3rd and 4th 16ths of every beat anticipated by 10-52 ms across
all instrument groups, surdo's downbeat lags ≤21 ms, and the swing ratio is
tempo-dependent — so flag samba as "negative swing" (pushed), not delayed
swing. ~95-115 (samba-enredo up to ~130).
**Partido alto** **[stylized]** — 16-pulse timeline ~ .x..x..x.x..x..x-family
variants; sources disagree on the rotation — flag.
**Bossa nova** **[verified timeline]** — bossa clave 0,3,6,10,13 on rim; kick
0,6,8,14 dotted figure. ~60-84.
**Baião** (Northeast) **[stylized]** — zabumba low on tresillo (0,3,6…
doubled: 0,3,8,11), triangle 8ths/16ths open-closed. ~90-120.
**Maracatu** **[stylized]** — heavy alfaia unison on 0,3,6,8,11,14-type
figures, gonguê bell timeline; ~90-110. **Frevo** fast 2/4 ~130-150.

## Uruguay

**Candombe** **[verified — Jure & Rocamora]** — three drums (chico, repique,
piano) plus the **madera** clave struck on drum shells: son-clave-shaped
timeline over a 4-beat/16-pulse cycle. The chico plays one fixed cell per
beat — rest on the beat, strokes on subdivisions 2,3,4 (steps 1,2,3 of each
beat) — with measured contraction (later strokes early, near-triplet feel);
repique adds improvised figures whose last onset leans ternary. Tempos in
measured recordings: 105-140, centered ~130. Encode straight with a "pushed"
microtiming flag.

## Argentina

**Chacarera** (6/8 vs 3/4 sesquialtera) **[stylized]** — bombo legüero
alternates dotted-beat lows (0,3) with 3/4 cross-accents (0,2,4 in 6-8th
units); rim clicks fill. ~100-120 dotted-quarter.
**Milonga** **[verified cell]** — habanera timeline 0,3,4,6 (doubled 0,6,8,12
on 16); tango's marcato in 4 and síncopa displace the same cell. ~95-120.

## Colombia

**Cumbia** **[verified meter/feel; stylized placements]** — 2/4; llamador
drum strikes the backbeat (steps 4,12), tambora low answers, alegre leads;
güira/guacharaca scrapes swung 16ths (the "gallop"). Modern cumbia: kick 0,8,
snare/rim 4,12, ~85-105.

## Peru

**Festejo** **[verified meter]** — Afro-Peruvian 6/8-12/8 (triplet grid),
cajón-led: low strokes on dotted beats with tone answers across the hemiola;
quijada/cajita colors. ~100-120 dotted-quarter. Placements **[stylized]**.
**Landó** — slower 12/8 with famous metric ambiguity (6/8+3/4) — flag.

## Dominican Republic

**Merengue** **[verified tempo/instrument]** — 2/4 at 120-160; tambora
"slap-pop" with the característico roll into beat 1, güira straight 16ths.
Placements **[stylized]**: tambora low 0, rim figures 3,4,6,7, roll 14-15.
**Bachata** **[verified accent]** — bongo martillo 8ths with the marked
accent on beat 4 (step 12); güira 8ths, ~108-135.

## Puerto Rico

**Bomba sicá** **[stylized]** — buleador low pattern with cuá sticks timeline;
16 pulses, ~90-110. **Plena** **[stylized]** — pandero seguidor on beats,
requinto syncopations, ~95-115.
**Reggaeton / dembow** **[verified — multiple sources]** — kick 0,4,8,12;
snare/rim 3,6,11,14 (the 3+3+2 tresillo skeleton). ~88-100. Lineage flag:
pattern named for Shabba Ranks's "Dem Bow" riddim, developed by Jamaican and
Afro-Panamanian producers — the same skeleton appears in dancehall's Bam Bam
and Fever Pitch riddims, so Jamaica/Panama/PR share one sequencer pattern.

## Jamaica

**One drop** **[verified — MusicRadar/Drum Magazine]** — kick AND cross-stick
together on beat 3 only (step 8); beat 1 empty; hats 8ths with skank on the
offbeats. ~60-80 (or double-time count ~140).
**Rockers/steppers** **[stylized]** — four-floor kick under the same skank.
**Ska** **[stylized]** — kick 0,8, snare 4,12, hats/guitar all four offbeat
8ths (2,6,10,14). ~120-140.
**Nyabinghi** **[stylized]** — "heartbeat" fundeh on 0 and 6 of 16 (one-two
rest), bass drum open on 0 with repeater improvising. ~70-90.
**Dancehall** **[verified skeleton]** — 3+3+2 kick (0,3,6 + backbeat snares),
the dembow/Bam Bam skeleton at ~95-105.

## Trinidad & Tobago

**Soca** **[stylized]** — power soca ~155-165: kick 0,4,8,12, snare 4,12 with
pickup 14, "engine room" iron playing straight 16ths with 3+3+2 accents,
cowbell offbeats. Groovy soca ~110-125 relaxes the kick to 0,8.
**Calypso** ~100-125, lighter backbeat with bell tresillo.

## Haiti

**Ti-bwa / kompa** **[verified cell]** — the cinquillo timeline (0,2,3,5,6
in 8, doubled on 16) is Haiti's key pattern, struck on the drum shell (ti-bwa)
in rara, vodou and in kompa's cowbell/gong line; kompa kit adds a light
four-floor and rim 8ths, ~95-115. **Yanvalou** (vodou) rides the 12/8
standard bell **[verified timeline]**.

## Mexico

**Son jarocho** **[stylized]** — 6/8 sesquialtera strum-driven (no kit);
percussive layer = zapateado heels alternating 0,3 vs 0,2,4 accents.
**Banda/tambora** ~ polka 2/4 or waltz 3/4 kit **[stylized]**; cumbia
sonidera at ~85-95 borrows the Colombian pattern.

## USA

**Backbeat family** **[verified]** — snare on beats 2 and 4 (steps 4,12) is
the defining constant across rock, R&B, funk, country. Hip-hop convention:
displace or omit the second-bar kick downbeat (anticipate to steps 7-8, delay
to 10-11, or drop).
**New Orleans second line** **[verified basis]** — built on 3-2 son clave
with swung, laid-back microtiming; bass drum tresillo figures, press rolls.
~90-110.
**Bo Diddley beat** **[verified]** — the 3-2 son clave orchestrated as a
floor-tom groove: accents 0,3,6,10,12. ~90-110.
**Blues shuffle** **[verified convention]** — triplet grid; at slow tempos
all three partials ride the cymbal, at fast tempos the middle partial drops —
tempo-dependent density worth encoding as sparse/main tiers.
**Swing ride** — triplet grid: ride 0, 2-of-3 pattern with hat 2&4.
(Rock, funk, boom bap, trap, house, techno, DnB already in library v1.)

---

# EUROPE

## Spain

**Flamenco 12-count compás** (soleá/bulerías/alegrías family) **[stylized,
well-attested]** — a 12-step cycle with accents on counts 3,6,8,10,12;
bulerías is conventionally counted starting from 12: encoding from count 12
as step 0 gives accents at 0,3,6,8,10. Palmas fill all 12. ~120-160 counts/min
(bulerías much faster). Rotation/count-start disagreement between schools —
flag. **Rumba flamenca** — 16-pulse strummed tresillo feel ~100-120.

## Ireland

**Reel** (4/4, 16ths) **[stylized]** — bodhrán pulse on quarters with 8th
subdivisions, accent 0 and 8; ~100-120 (two-beat feel).
**Jig** (6/8) **[stylized]** — DUD-DUD accents 0,3 on a 12-grid; **slip jig**
9/8 (3+3+3); Gotham's review renders a jig-family timeline as IOI [2-1-2-1-3]
over 9 pulses. ~110-130 dotted-quarter.

## Bulgaria (and the wider Balkans)

**[verified meters — Folkdance Footnotes / Chromatone]** Aksak dance meters
conceived as chains of quick (2) and slow (3) pulses; the tapan (davul) plays
dum on group starts. The canonical set, with group starts 0-indexed:

```
Pajduško    5/16  = 2+3        starts 0,2
Račenica    7/16  = 2+2+3      starts 0,2,4
Makedonsko  7/16  = 3+2+2      starts 0,3,5   (reverse račenica — flag pair)
Dajčovo     9/8   = 2+2+2+3    starts 0,2,4,6
Kopanica    11/16 = 2+2+3+2+2  starts 0,2,4,7,9
Krivo Sadovsko 13/16 = 2+2+2+3+2+2      starts 0,2,4,6,9,11
Bučimiš     15/16 = 2+2+2+2+3+2+2       starts 0,2,4,6,8,11,13
```

Subdivision pulse up to ~520/min in fast dances. Composite meters (18/16 =
7+11, 22/16 = 9+13) chain these blocks.

## Greece

**Kalamatianos** 7/8 = 3+2+2 (starts 0,3,5) **[well-attested]**;
**zeibekiko** 9/4 (slow, 2+2+2+3 family); **hasapiko** straight 2/4.
Dum on group starts, tek elsewhere **[stylized]**.

## Turkey

**Karşılama** 9/8 = 2+2+2+3 **[verified meter — khafif.com FAQ]** — dum 0,
tek 2, dum 4, teks in the 3-group (6,8); Roman havası places extra offbeat
teks — variants flagged in the FAQ. **Çiftetelli** — slow 8/4 with
dum...tek-tek phrasing **[stylized, sources vary]**. **Curcuna** 10/8 =
3+2+2+3 (starts 0,3,5,7). Usul tradition notates düm/tek strokes per pulse —
directly encodable.

---

# MIDDLE EAST / NORTH AFRICA

## Egypt (and the pan-Arab dance repertoire)

**[verified family — khafif.com Middle Eastern Rhythms FAQ]** Dum (low,
center) / tek (high, rim) box notation per 8th:

```
Maqsum     D T . T D . T .    kick 0,8 · tek 2,6,12 (16-grid)
Baladi     D D . T D . T .    kick 0,2,8 · tek 6,12  ("heavier" maqsum)
Saidi      D T . D D . T .    kick 0,6,8 · tek 2,12  (upper-Egypt cane dance)
Malfuf     D . . T . . T .    kick 0 · tek 3,6 per half-bar (tresillo shape)
Ayyub/Zar  D . . D T . D .    variants disagree — flag; driving 2/4
Masmoudi kabir  8/4: D D . . T . . . D . . T T . T .  (slow; many fills)
```

Tempo: maqsum/saidi ~90-115, malfuf fast ~120-140, masmoudi slow ~60-80.
(Maqsum/baladi/saidi already partly in library v1 — v2 aligns saidi.)

## Lebanon / Levant

**Dabke** line dances ride maqsum/ayyub-family cells with heavy downbeat
stomps **[stylized]**; the FAQ documents regional naming disagreements
(baladi/saidi/maqsum overlap) — flag.

## Iran

**Shesh-o-hasht (6/8)** **[stylized]** — the dominant Persian pop/classical
groove: tombak bass (tom) on 0 and 3 with tek chatter on 2,4,5 of a 6-8th
grid; ~110-130 dotted. Classical dastgah music also uses free meter — not
encodable.

## Israel (report-only)

Popular hora grooves are straight 4/4 with offbeat accents ~120-140
**[stylized]**; Mizrahi pop borrows maqsum/baladi directly.

---

# ASIA

## India (Hindustani)

**[verified — tala index sources]** Talas are cyclic timelines with
hierarchically marked beats (sam = cycle start, tali = clapped groups, khali
= waved group). The theka bols map one-per-matra onto steps; Dha/Dhin carry
the bass (baya) voice, Tin/Na/Ta the treble only. Khali vibhags drop the bass.

```
Tintal   16 (4+4+4+4)  Dha Dhin Dhin Dha | Dha Dhin Dhin Dha | Dha Tin Tin Na | Na Dhin Dhin Dha
                        tali 0,4,12 · khali 8 (bass drops on steps 8-11)
Keherwa   8 (4+4)      Dha Ge Na Ti | Na Ka Dhi Na       (film/folk workhorse)
Dadra     6 (3+3)      Dha Dhin Na | Dha Tun Na
Rupak     7 (3+2+2)    Tin Tin Na | Dhin Na | Dhin Na    (khali ON beat 1 — unique)
Jhaptal  10 (2+3+2+3)  Dhin Na | Dhin Dhin Na | Tin Na | Dhin Dhin Na
```

**Bhangra / chaal** (Punjab, popular) **[stylized]** — dhol at ~ swing 8ths:
bass dha on 0,6,8,14 with tihai-style fills; ~ 95-110. Carnatic adi tala
(8) parallels keherwa structurally.

## Pakistan (report-only)

Qawwali rides dholak keherwa/dadra variants with handclap reinforcement on
the tali structure **[stylized]**.

## Indonesia (Java/Bali)

**Gamelan colotomy** **[verified — colotomic structure sources]** — nested
gong cycles, END-weighted: the biggest gong lands on the LAST beat of the
cycle (seleh), the reverse of Western downbeat orientation. On a 16-step,
one-stroke-per-beat grid (0-indexed, 1 beat per step):

```
Lancaran  T.TN TPTN TPTN TPTG →
  ketuk 0,2,4,6,8,10,12,14 · kenong 3,7,11,15 · kempul 5,9,13 (wela at 1) · gong 15
Ketawang  pTpW pTpN pTpP pTpG →
  kempyang 0,2,…(alternating with ketuk) · kenong 7,15 · kempul 11 · gong 15
Ladrang   32 beats (ketawang doubled: kenong 7,15,23,31, kempul 11,19,27, gong 31)
```

Instruments generally do not sound when a larger gong sounds (except kenong
with gong). Balinese kecak interlocks are vocal — report-only.

## Japan

**Taiko matsuri patterns** **[stylized]** — "don doko" cells on a 16 grid:
don (low) 0,8 with doko doubles (6,7 or 10,11), ka (rim) offbeats; ji base
patterns straight or swung ("umauchi" horse-beat = long-short). ~90-130.

## Korea

**Jangdan** **[verified meter class — 12/8 compound]** — janggu drum cycles:
gutgeori (12/8, lilting, kung 0 deok 3,8-type placements **[stylized within
verified meter]**, ~ 60-90 dotted) and jajinmori (fast 12/8, "deong 0 kung
6" skeleton, 90-140) — full stroke maps need pedagogy cross-check.

## China, Thailand, Vietnam, Philippines (report-only)

Chinese luogu (gong-and-drum) repertoire is pattern-cycle based with
percussion "sentences"; lion-dance drumming uses cued phrases rather than
fixed loops. Thai/Khmer classical uses colotomic ching cycles (ching-chap).
Filipino kulintang repertoire (e.g. binalig, duyug modes) is gong-melody
driven. All encodable only with dedicated sources — flagged for a v3.

---

# OCEANIA

## Tahiti / French Polynesia

**'Ōte'a drumming** **[stylized]** — tō'ere log drums at high 16th density
with 3+3+2 accent cells over pahu bass on the pulse; ~120-160. Cook Islands
drumming is closely related.

## Hawaii (report-only)

Ipu heke gourd: pa (low, on beats) / um (slap, subdivisions) in hula ~ 80-120.

## Australia (report-only)

Clapstick patterns accompany didgeridoo in cyclic cells but vary by song and
clan; no stable "national pattern" to encode — deliberately left out.

---

# Encoding recommendations (applied in library v2)

Timelines get `kind: "timeline"`, one or two lanes, no set. Grooves get sets
with sparse/main/lift/fill roles as in v1. Aksak meters encode with
`stepsPerBeat: 1` and `beats` = pulse count, accents on group starts, with
the grouping recorded in `notes`. Samba/candombe/second-line carry a
`microtiming` note ("pushed"/"laid-back") pending a signed timing row in a
future format rev. Where traditions disagree (standard-pattern rotation,
ayyub/çiftetelli variants, partido-alto rotation, Yankadi ternary/binary,
flamenco count-start), the chosen encoding is stated in `notes` and flagged
`variants-disagree` in tags.

# Sources

Academic: Toussaint, *Classification and Phylogenetic Analysis of African
Ternary Rhythm Timelines* (BRIDGES 2003); Toussaint, *The Geometry of Musical
Rhythm* (McGill); Agawu, "Structural Analysis or Cultural Analysis?" (JAMS
59/1, 2006); Gotham, review of Toussaint (MTO 19.2); Fiol, *Hidden Rhythm*
(clave as analytical bridge); Jure & Rocamora, *Microtiming in Candombe
Drumming* (2016); Naveda et al., *Multidimensional Microtiming in Samba*
(2009); Danielsen et al., "Shaping Rhythm" (Popular Music, Cambridge, 2023);
JNMR tempo-dependent samba swing study (2020).

Pedagogy/practitioner: Jas's Middle Eastern Rhythms FAQ (khafif.com); Paul
Nas / WAP Pages West African djembe compilation (box notation, 150+ rhythms);
djemberhythms.com notation lessons; Rhythm Notes conga tumbao (21 variations);
DRUM! Magazine "49 Beats"; MusicRadar one-drop programming; Ethan Hein,
drum-machine programming essays (NYU); Chromatone clave & Balkan systems;
Folkdance Footnotes Bulgarian rhythms; ragajunglism.org tala index; RagaNet
tabla thekas; Wikipedia: Colotomy, Traditional Korean rhythm (jangdan),
Dembow; Pocket Operations pattern book (P. Wenzel, 2024 — placements
cross-checked, book itself not CC0); Attack Magazine Beat Dissected (grid
format reference); kickdrum.io pattern library; Baba Yaga Music rhythm
diagrams; UJAM Latin rhythms guide.

*Compiled 2026-07-22 via multi-agent search + adversarial verification (20
core placement claims confirmed 3-0; refuted inferences excluded). CC0 —
the patterns themselves are traditional/conventional material.*
