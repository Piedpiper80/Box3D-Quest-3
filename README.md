# THE FIGURE

**Play it: <https://piedpiper80.github.io/Box3D-Quest-3/arena.html>** — open it in
the Quest browser and hit **Start — in your room**.

One opponent, standing on your real floor, built on a human skeleton. Hit it and
the metal caves in where you land. Hit the same place enough and the bone comes
off, and everything below it goes too.

There is no core, no reactor, nothing to detonate. There is a body, and what
happens to a body when you hit it.

---

## What it is

Passthrough VR. Your room, your floor, and one figure in it, sized to you.

- **Seventeen bones, sixteen joints, laid out on real anthropometry.** Every
  length is a fraction of standing height, and the height is yours — taken from
  your head the moment you put it on, so it stands eye to eye with you.
- **The joints are the joints you have.** Ball shoulders and hips, hinge elbows
  and knees that bend the way yours bend, a spine in three parts, a neck. That
  is most of why it reads as a body rather than as a rig.
- **Every bone dents.** Not a texture swap and not repositioned blocks — a real
  subdivided mesh whose vertices cave in around each contact point, with a
  raised rim, permanently. Lighting is recomputed from the crumpled triangles,
  so a dent catches the light like a dent.
- **Every bone breaks.** Beaten past a third of itself, a bone loses the joint
  holding it and falls, taking everything below it with it. Break an elbow and
  the forearm and the hand go together. Nothing in the code says so; the
  skeleton does.
- **The legs are the way down.** Take one and it drops to that knee and stops
  walking. Take both and it goes down for good. Take both arms and it can no
  longer throw anything at you.
- **It fights back.** It closes, weaves on the way in, draws an arm back — that
  is the whole telegraph — and throws. When it has thrown one, it is open.
- **Every sound is synthesised from the physics** as it happens. Nothing here is
  a recording.

## How to play

1. Open the link in the Quest browser and press **Start — in your room**.
2. First time only: rest a controller on the floor and pull the trigger. That is
   the entire calibration, and it is remembered.
3. **Raise both fists above your head** to bring one in. Two glowing markers show
   you where, and they brighten as you hold.
4. Fight. Same gesture afterwards for the next one.

Plays seated. Motion-sensitive? Add `?calm=1` for no view shake at all.

Two knobs, live from the address bar: `?power=` (0.3–3) scales how much of your
arm goes into a punch, `?tempo=` (0.3–2) how fast it fights. The report on the
page names any knob you set.

## What is measured, and what is not

Everything except feel. Two suites run in CI on every push:

```bash
node wasm/test.js                     # 46 engine checks
node wasm/page-test.js arena.html     # 15 page checks — runs the real page
                                      # headlessly with a scripted pilot
```

The engine suite covers the solver, the destructible-matter system, and the
figure: that it stands, that its head is where a head goes, that its knees do
not bend forwards, that it turns to face you, that walking bends its knees, that
a broken forearm takes the hand with it, that both legs gone puts it down, and
that a step of the whole fight fits inside the frame budget. The page suite runs
the real page in Node with stubbed GL and XR, driven by a scripted pilot who
calibrates the floor, squares up, watches it come, and then takes it apart.

`arena.html?flat=1` runs the whole thing without a headset on an ordinary
canvas, with that same scripted pilot — so the way it looks can be seen and
reviewed without putting the headset on.

**What no test can answer is how it feels**, and that is the only thing a
headset is ever asked for.

## Where things are

```
docs/arena.html          the game
docs/box3d.wasm          built artifact, rebuilt by CI from bridge.c — don't hand-edit
wasm/bridge.c            the engine: destructible matter, the skeleton, its will
wasm/build.md            how the wasm is built (Zig toolchain, pinned Box3D revision)
wasm/test.js             the engine suite
wasm/page-test.js        the page harness
docs/design/ROADMAP.md   the record: every system, measured number, known limitation
```

Built on [Box3D](https://github.com/erincatto/box3d) (Erin Catto, creator of
Box2D), compiled to WebAssembly with the Zig toolchain. The live site is the
`gh-pages` branch.
