# Writing: tooltips, menus, manuals and docs

Anything a player reads (a param tooltip, a menu item, a screen label, a manual)
and anything a reader of this repo reads (design docs, CLAUDE.md, comments)
should say what happens in plain, literal terms.

## Say what physically happens; do not personify it

Do not give sound, air, surfaces or code verbs that make them sound like they
have intent or a ledger. These read as an AI's voice, not a person's:

- "the air takes the treble" → "drier air absorbs more high frequencies"
- "a vault that flexes takes the bass" → "a flexing vault absorbs low frequencies"
- "the walls pay for it", "you pay for it in CPU" → "it costs 2% more CPU"
- "the air takes over" → "above 500 Hz, air absorption sets the decay"
- "X eats the highs", "the tail swallows the attack" → say what is absorbed or masked

**`takes` and `pays` are red flags.** Use them only in their plain literal sense
("a trace takes 0.7 s", "the input takes a poly cable"). If the sentence uses
either one figuratively, rewrite it.

## The other tells

The website's guide (`signalfunctionset-web/CLAUDE.md`, "Write plainspoken and
direct") lists the rest, and they apply here too: no setup-and-payoff
constructions ("X isn't just Y, it's Z"), no balanced two-part wrap-up sentences
("X does the work, Y makes it sing"), no hype adjectives, no "it's worth noting"
or "under the hood". Short declarative sentences; a period over a comma splice.

Tooltips in particular: a parameter name, then units or what the control does,
in a few literal words. "Humidity (drier air absorbs more high frequencies)",
not a slogan.
