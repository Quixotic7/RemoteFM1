# The standalone mockup (approved by the owner, 2026-10-30)

The sources of the Claude Design canvas "RemoteFM1 Standalone": one `.dc.html` file per artboard, `canvas.json` for
their layout and titles. Each screen artboard is 480 x 480 CSS px, which is **the FM-1's 240 x 240 panel at 2x**: halve
every size, position and font size to get device pixels.

- `Main.dc.html`: the panel map (every control's job). STANDALONE-PLAN.md section 3 is the same table.
- The screens: Home-Notes, Home-Scope, Home-Keys, Preset, Channel, CC-Bank, CC-Turn, CC-Assign, Octave, Rec, Loop-Play,
  Loop-Pick, Rec-Options, Arp, Arp-Settings, Theme (the SAVE menu, interactive), Themes (the 8 themes side by side,
  with their colour tokens in its script).
- Fonts: Jersey 10 (Google Fonts, OFL). Backgrounds solid black. Motion: `steps()` animations in each file's
  `<helmet><style>`; colours and sprites (pixel maps as strings) in each file's script.
- The owner's notes that shaped it: not ChoralRoot's look; big readable values with chunky pixel nostalgia; no
  bevelled keys; "Dm7" is the chord being played; a furball monster on the octave screen.
