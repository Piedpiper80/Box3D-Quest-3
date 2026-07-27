# Roadmap

A living document. It holds the **ordering** and the **dependencies** — not the
detail. Detail lives in a per-milestone plan under `docs/design/`, written
just-in-time when that milestone comes up. Only the next milestone should ever
be planned in depth; everything past it is a heading until you get there.

Re-read this when deciding what to do next. Edit it freely — an ordering that
never changes is an ordering nobody is using.

---

## Governing rules

1. **Prove each idea in its simplest form first.** Every new system starts as a
   throwaway spike with a yes/no question attached. If the spike can't be
   phrased as a question, the idea isn't understood well enough to build yet.

   **And build up from there one confirmed layer at a time.** This was broken
   badly on the piloting spike: shoulders, joints, motors, cone limits and an
   upgrade curve were all built and shipped before anyone had confirmed that a
   cube follows a controller. When it felt wrong there was no way to tell which
   of five layers was at fault, and three rounds went into fixing the wrong one.
   The order is always: does the input arrive → does it render where it should →
   does one body follow it → does a joint follow it → does the game feel right.
   Each answered in the headset before the next is written.
2. **Every milestone ends with something testable in the headset.** Not a
   passing unit test — something you put the Quest on and try.

   **But it must be confirmed to run before it gets there.** "You need a headset
   to test VR" was taken for granted and it was wrong. Only the poses and the GL
   calls come from the headset; stub those two and the whole page — wasm load,
   world build, physics, draw list — runs in Node. `wasm/page-test.js` does
   exactly that and CI runs it on every push, because a page was handed over
   three times in a row that rendered nothing at all and there was no way to
   know. What still needs a person in a headset is how it *feels*, and that is
   the only thing that does.
3. **Look at it.** Nobody building this had ever rendered an image of it until
   a playtest said it looked rudimentary — geometry and physics were verified
   headlessly while the *look* shipped sight unseen, and an armour plate sat
   across the player's face for a full release because no one could see it.
   `arena.html?flat=1` runs the whole game without a headset on an ordinary
   canvas — same engine, same shaders, scripted pilot — and the screenshot
   loop (`scratchpad/shot.js`, Chromium) is part of every visual change:
   render it, look at it, then ship it. The flat page also means the designer
   can review style from a phone.
4. **Measure before you design.** The perf HUD (Phase 0) exists so that every
   later decision is made against real numbers instead of intuition. Quest 3 is
   a fixed budget; the numbers decide the design, not the other way round.
5. **Keep a playable spine.** From Phase 5 on there is always a complete
   game — two destructible mechs, a killable core, one arena — and every later
   phase is an addition to something that already works. This isn't about
   shipping early; it's so quality can be judged by *playing* rather than by
   imagining the finished thing.
6. **Take the time.** There is no deadline. Where there's a fast path and a
   right path, take the right one — including tooling, rework, and throwing
   away things that turned out wrong.

---

## What "top tier indie" means here

The bar is a top-tier indie VR game, not a tech demo that runs well. Distribution
is sideload / SideQuest / itch, so **store policy does not constrain the design**
— content, structure and length are open.

One distinction is worth keeping though: disregard *policy*, not *physiology*.
Frame rate targets and comfort mitigations aren't platform rules, they're facts
about human vestibular systems. A VR game that makes people sick isn't bold,
it's unplayable. Those stay.

None of what separates top-tier from good-prototype is systems work, which is
exactly why it needs to be written down next to the systems:

- **Coherent art direction.** Not realism — *coherence*. The current look is
  flat-shaded coloured cubes; whatever replaces it needs to be a deliberate
  choice that a voxel mech, a town, and deep space can all live inside.
- **Audio.** For a giant mech, sound does more work than graphics to convey
  mass. Servo whine, hydraulic hiss, metal under stress, the boom of a footfall,
  the crunch of an impact, engine tone shifting as systems fail. This is the
  highest-leverage polish investment in the whole project and it's routinely
  underrated by engineers.
  - **Source:** ElevenLabs, generated through the web app and exported as
    files — not the API. So audio is an asset pipeline, not a runtime
    dependency: generate, audition, commit.
  - Convert to Ogg/Opus before committing. Raw wav will bloat the APK fast, and
    a mech needs a lot of individual sounds.
  - Confirm the plan tier grants commercial use rights before selling anything
    built with it.
- **Game feel.** Hit stop, haptics, cockpit shake, particulate, oil spray,
  debris. The difference between "the solver resolved a collision" and "I hit
  him."
- **Content volume.** Opponents, arenas, builds, environments.
- **Comfort and absence of jank.** In VR this is a correctness requirement, not
  polish.
- **Playtesting with people who aren't you.** Nothing else substitutes.

These are **tracks, not a phase**. Polish bolted on at the end doesn't produce
top tier; each gets a first pass when the system it decorates lands, starting
with audio and game feel at Phase 4.

---

## VR conventions — the default, not a per-feature decision

Quest players arrive with expectations, and meeting them costs nothing while
breaking them makes a game feel wrong for reasons players can rarely name. These
are the house rules; deviate only with a stated reason.

| Convention | Meaning here |
|---|---|
| **Meta button long-press** | Recentre. The system owns the button; listen for the reference space's `reset` event |
| **Grip** | Hold / grab. Holds a mech arm; releasing lets the limb go slack |
| **Trigger** | Primary action — attack. Never bind it to configuration |
| **A / X** | Secondary action, cycling options |
| **B / Y** | Cancel, back |
| **Menu button** (left) | Pause / menu |
| **Left stick** | Move |
| **Right stick** | Turn — snap by default, smooth as an option |
| **Body rig** | Derived from live head tracking every frame, never placed once and frozen |
| **Comfort** | Vignette during artificial locomotion; never move the player's view without input |
| **Hand tracking** | Pinch substitutes for trigger; anything gated on a button that hands lack must degrade to "always on" rather than to "broken" |
| **Floor height** | Set by the player — rest a controller on the ground and pull the trigger — then remembered. Never inferred from the runtime |
| **Destructive actions** | Never on a gameplay input. Calibration, resets and anything that discards state live in a menu |

The destructive-action rule was learned the hard way too: floor recalibration was
briefly bound to holding both triggers, which is a two-handed punch — precisely
what a mech pilot does constantly. Any gesture reachable in a fight will happen
in a fight.

The body-rig rule is worth stating plainly because it was learned the hard way:
a rig placed once from a head pose leaves the player behind the moment they lean
or stand, and every attempt to fix that with a calibration step is treating the
symptom. Attach it to the head and the problem stops existing.

---

## Decisions made

### Combat: you *are* the mech, with diegetic controls

Your real arm motion maps 1:1 to the mech's arms at scale, and you see a pilot
avatar gripping controls in the cockpit — the
[Underdogs](https://www.roadtovr.com/underdogs-vr-mech-preview-quest-pc-vr/)
presentation. Two reasons this is the spine of the whole design:

- **Mass is felt, not displayed.** Your hand is a target; the mech's arm is a
  motorised Box3D joint chasing it with a *limited* torque, so a heavy arm lags
  behind your hand. Upgrading joint strength changes how the mech feels in your
  body — which turns the material and joint systems (Phase 3) into core game
  feel rather than a stats screen.
- **The cockpit hides the seams.** A pilot avatar holding grips gives tracking
  latency and hand/mech mismatch somewhere to live. Un-mediated limb mapping has
  no such cover.

### Combat: momentum is the mechanic, responsiveness is the upgrade

Fights are physical, and they're built on committed motion. Once a mech's arm is
moving it *stays* moving — let a punch go and you can't easily pull it back, and
neither can your opponent. You win by baiting commitment and punishing the
recovery.

The important part: **none of that is scripted.** Most fighting games simulate
commitment with animation lockout frames. Here it falls straight out of rigid
bodies with mass and torque-limited joints — a heavy arm genuinely cannot
reverse quickly, because reversing it is a physics problem. You don't implement
attack commitment; you implement mass and torque limits, and commitment emerges.
That's the game's real differentiator and it's worth protecting.

**Responsiveness is the upgrade axis.** A better mech tracks your hand more
closely, asymptotically approaching 1:1. This is the same force-limited motor
target the piloting design already uses, so the upgrade curve *is* the torque
curve — progression is felt in your body rather than read off a stat screen.

> **Calibration caution:** don't actually reach 1:1 at the top. A mech that
> tracks your hand perfectly stops feeling like a mech and starts feeling like
> your own arm, which loses the fantasy exactly where the player has earned the
> most. Keep a little lag and follow-through even at the ceiling.

### Weight classes

Progression runs up through weight classes — small, fast mechs at the start,
working up to something that can fight a god.

**Weight class sets mass; upgrades set torque.** Keeping those on separate axes
is what stops the two systems collapsing into each other. A light mech needs
little torque to feel near-1:1. A heavyweight needs enormous torque just to
match that responsiveness, and even fully upgraded its momentum once moving is
vastly greater — so the best heavyweight feels sharp *for its weight*, hits like
a truck, and still can't change its mind. That's the fantasy, and it comes out
of the physics rather than a balance table.

> **The starting mech should still feel heavy.** A lightweight is light
> *relative to a heavyweight*, not light in absolute terms. If the first mech in
> the game feels weightless the fantasy never lands.

Weight class ends up doing six jobs at once, which is usually the sign of a
correct concept:

| It defines | How |
|---|---|
| Mass budget | The build constraint in Phase 4 — and what a legless build frees up |
| Feel band | Light is twitchy and near-1:1; heavy is committed and devastating |
| Physical intensity | See below — it doubles as a comfort setting |
| Progression ladder | The climb from circuit fighter to god-killer |
| Circuit structure | How Phase 6's roster and tournaments are organised |
| Worst-case perf load | Heavyweights are the load Phase 0.5 must model |

**Physical intensity scales inversely with weight, and that's a feature.** A
lightweight means rapid, small, constant arm movement — a genuine cardio
workout. A heavyweight means slow, deliberate, consequential swings — far less
physical load per minute, but far more demanding of timing and reading. So the
climb from light to heavy is also a climb from athletic to tactical, and a long
session in a heavyweight stays sustainable even if lightweight fights are
exhausting. The game gets an intensity range without ever labelling one.

### Locomotion: a consequence of the build and the damage state

Not a menu setting and not one scheme — how you move falls out of what your mech
*is* and what's left of it:

| Mech state | How you move |
|---|---|
| Legs intact | Walk / stride, stick-driven |
| Built without legs — a deliberate build choice | Arm-drag: anchor a fist, pull yourself along |
| Legs destroyed mid-fight | Arm-drag, forced |
| Mech dead | On foot, your own legs (Phase 5) |

This is the strongest structural idea in the project so far, because it makes
four other systems pay off at once:

- **Destruction gets mechanical consequence**, not just visual. Losing a leg
  changes how you play, immediately.
- **Legless becomes a real build** with a real trade — mass budget freed for
  arms and armour, paid for in mobility. Not a broken mech, a different one.
- **The tension curve is automatic.** As a fight goes badly your movement
  degrades, which is exactly the drama the fight wants, with no scripting.
- **It ends where the game already goes.** Walk → drag → eject and run is one
  continuous ladder into the on-foot escape.

**Build order note — the fallback is easier than the primary.** Arm-drag is
physics-native: an anchor constraint plus an impulse, which Box3D does directly.
Physical bipedal walking is genuinely hard — an emergent rigid-body gait is
research-grade, and the realistic answer is a driven gait with physics reaction
rather than a truly simulated walk. So **build arm-drag first**: it's cheap, it's
what everything degrades to, and it makes a legless mech fully playable long
before walking works.

**Comfort:** stick-driven movement inside a cockpit is close to the best case
for VR locomotion. The cockpit frame is a static rest frame in your field of
view — the main mitigation for vection sickness — and a giant mech is slow with
low acceleration, which is the other. This is well-precedented in cockpit games.
The instinct is sound; it isn't something to design around.

**Roomscale is a separate axis.** Inside a cockpit your real steps move you *in
the cockpit* — leaning, dodging, looking around your instruments — and that works
alongside every row of the table above. Whether roomscale can additionally drive
the mech itself (your steps becoming giant steps) is only coherent for an
exposed, cockpit-less build and is bounded by guardian size. Left open.

**Consequence for hand tracking (Phase 1):** the anchor/release verb is now
load-bearing — it's the fallback locomotion for every mech that loses its legs.
On controllers it's the grip button; with hands it's a fist clench, detectable
but with no haptic confirmation of the moment the anchor bites. That's the
specific thing the spike has to answer.

---

## Phases

### Phase 0 — Foundations
> Plan: [`phase-0-foundations.md`](phase-0-foundations.md)

Bump the Box3D pin to the v0.1.0 release (joints and multi-shape bodies are
required by nearly everything downstream). Turn SIMD on, wire up Box3D's task
system, replace the per-body draw call with instancing, decouple physics from
the render rate, and build the perf HUD.

**Exit criterion is a number**, not a feature: the maximum dynamic body count
that holds a stable frame rate in-headset.

> ### The number: 400 measured, ~1000 expected native
>
> Measured on a Quest 3 through the browser build — scalar, single-threaded —
> **400 bodies colliding inside a 4.63 ms physics budget** at 72 Hz. The same
> case costs 1.86 ms on CI, so the Quest runs ~2.5× slower than CI for identical
> work. Applying that to the SIMD + 4-worker figures puts the native app at
> roughly **1000–1200 awake bodies**.
>
> **What it means for the design.** Phase 0.5 measured two fully-shattered mechs
> at 200 voxels each as exactly 400 bodies — so the browser build already runs
> that case, and native should manage ~500 voxels per mech fully shattered. But
> full shattering is the worst case, not the normal one: with intact structure
> merged into static chunks, the budget that actually binds is **live debris,
> and it sits around 600–800 fragments**. That is the number Phase 2 designs
> against, and the number a live-debris cap should enforce.

### Phase 0.5 — Worst-case spike
Before designing anything on top: spawn the genuine worst case — two
mech-sized voxel assemblies, smashed together — and read the HUD. This is the
hardest sustained load in the entire project. If it doesn't fit, it changes
voxel size, mech size and chunking *now*, rather than after three phases have
been built on the assumption. Cheap to run; potentially saves months.

**Model a heavyweight, not an average mech.** The top of the weight ladder is
the load the game has to survive, and sizing the engine against a mid-weight
means discovering the ceiling three phases too late.

### Phase 1 — Sandbox + spikes

> **Built in the browser, not native.** There is no PC available to sideload an
> APK, so anything that has to be *felt* is developed against the WebXR build,
> which reaches the headset through a URL. WebXR on Quest 3 exposes controllers
> and hand tracking, so every spike below can run there. The native app remains
> the performance target — it is roughly 2.5× faster — but it is no longer where
> the work happens.
The test room, minimal: flat room, spawn menu, object inspector, perf HUD always
visible. This is the development harness for everything that follows, so it comes
early and grows continuously.

Then three spikes:
- **Arm-drag locomotion** — anchor a fist, pull. Physics-native, and the
  fallback the whole locomotion ladder rests on. Cheapest of the three and the
  one with the most downstream value.
- **Piloting feel** — force-limited motor targets driving a mech arm from a hand
  pose. Smallest possible version: one arm, one target, one weight.
- **Hand tracking** — test the *hard* cases: fast, aggressive, occluded combat
  motion, and the fist-clench anchor. Hand tracking is good on Quest 3 for
  deliberate manipulation and weakest at exactly what a fight is made of. A
  legitimate outcome is "hands for cockpit, sandbox and on-foot; controllers for
  fights."

#### What the piloting spike cost, and why

The jointed arm took five rebuilds to get honest, and every fault in it was
**silent** — nothing errored, nothing warned, the machine just thrashed and the
page just went black. Worth writing down, because the same traps are waiting in
every other joint this game builds.

| What was wrong | Why nothing caught it |
| --- | --- |
| Spherical cone limit set to 2.2 rad | Box3D asserts a quarter turn is the maximum, and the release build compiles asserts out. The cone geometry silently went degenerate. |
| Elbow hinged along the arm's own length | Box3D revolute joints rotate about the frame's **+Z**; the frames were left as identity, which pointed the hinge down the limb. The elbow could only twist, so the arm was one rigid stick — the "two sticks coming off my shoulders". |
| Elbow limits fenced off the fold direction | With the axis finally right, the arm wanted negative angles and the limits allowed positive. It sat pinned at its stop, stretched to 0.85 m for a target 0.28 m away. |
| Aim and uprighting gains scaled by **mass** | Angular dynamics answers to *rotational inertia*. Against ~0.03 kg·m² a damping term of 22 is roughly eleven times what an explicit 72 Hz step tolerates, so the controller pumped energy in every frame. |
| Motors held at zero velocity | A motor with no target is a brake. The arm was hauled by the hand and braked by its own joints at 2300 N·m simultaneously. Joint *limits* give structure; motors held at zero are just a clamp. |
| Draw stride still 2 after the state went to 7 bodies | The right arm was drawn from the left attachment. |
| `chestY()` called but never defined | Inside `"use strict"` it throws on the first frame, before the draw call. Cleared buffer, no error, pure black — indistinguishable from a page that never loaded. |

Three rules came out of it, and they apply to every joint from here on:

1. **Derive gains from the quantity the dynamics actually uses** — inertia for
   torques, mass for forces — and express them as a period and a damping ratio,
   never as raw numbers picked by eye. A gain that looks reasonable and is
   fifty times too large behaves like a physics bug, not a tuning problem.
2. **Read the joints, don't infer them.** `w_mech_joints()` exists because
   guessing whether a joint was pinned at a limit burned more time than every
   other diagnosis combined. One reading — "elbow at −6°, limits −6..137" —
   ended it instantly.
3. **Isolate before tuning.** Two faults were live at once and each hid the
   other; nothing tuned because nothing *could*. `w_mech_pin()` bolts the torso
   down so an arm can be judged on its own. A steady drift toward a *fixed*
   target is proof of a controller adding energy, and is the single most useful
   signal found here.

#### Phase 2 first cut: the voxel core is real (`vox.html`)

A voxel is a **cell in a grid, not a physics body** — that one rule is what
makes destruction affordable. The solver only ever sees:

- one static body of greedy-merged runs — a 528-cell wall is **24 shapes** and
  costs ~66 µs/step untouched, in the slowest (scalar wasm) build;
- a debris pool with a **hard cap of 150 live cubes** — past it the oldest is
  teleported to the newest break. Before the cap, levelling a wall put 496
  live cubes in the solver and a step cost 8.8 ms; with it, **2.3 ms**;
- one dynamic body per detached chunk (ring of 10, oldest retired).

Damage is **momentum through contact hit events** — a fist, a thrown block and
a falling chunk all hurt the wall through one mechanism, mass × approach
speed. A fist counts the whole arm behind it, not just the end block (measured
first: a committed heavy punch chipped one cell where it should crater).
Measured after: 4 punches take out 6 cells light, 14 medium, 18 heavy, 28
very heavy; a 26-hp wall accumulates damage and chips slowly.

Structure is a flood fill from the ground row after any kill: orphaned regions
detach — small ones burst into debris, big ones become a single falling slab.
That query is the exact "is this limb still attached?" the mech needs in
Phase 3.

Not done yet, known: chunks don't take further per-cell damage (they are
plain rigid bodies once fallen); nothing re-merges to static (the caps bound
the body count instead); rendering runs split on a two-bucket damage
threshold only. Next Phase 2 work: per-cell damage on chunks, then the same
grid worn as a mech's own armour — which is where Phase 2 meets Phase 3.

#### Phase 3 opened: materials are a table, and the wall system is many walls

The voxel core is multi-grid (4 concurrent grids, shared debris budget and
chunk ring) and cells now belong to a **material**: density, toughness, colour,
debris colour. Wood 400 kg/m³ / 4 hp, stone 1600 / 9, steel 7800 / 26 —
measured with the same four punches: a light arm takes 8 cells of wood and
**zero** of steel; steel yields only to the heavy arms. Nothing is per-object
tuning — a punch is momentum, a material is a table row, and everything else
falls out. `vox.html` now stands three walls side by side so the difference is
felt, not read.

#### Armour on a moving body: the fight's core tech is proven (`dummy.html`)

A grid no longer has to stand on the ground — it can ride **any body**. Cells
live in the body's local space and only touch world space at the borders:
impacts come in through the body's transform, debris and detached slabs leave
through it with the body's pose and velocity. A wall is now just the
degenerate case (static body, identity pose), and all 51 wall checks pass
unchanged through the posed code path.

The proof is a plated punching bag: a swinging dynamic core wearing an 8×8
stone plate anchored on its mount face (anchor axis is per-grid — a wall's
foundation is the ground row, armour's is the layer bolted to the body).
Measured: the bag swings 26 cm under punches while the plate sheds 64 → 11
cells, debris flying with the motion, and a line cut across the plate mid-swing
tears the upper half off as one slab born at the body's pose. Strip the plate
and the bare core shows.

That is the shape of every fight to come: armour first, then the machine
inside. Next: the mech itself instanced (two machines in one world) and
wearing these plates — which is Phase 4's front door.

#### First playtest verdicts, and what they changed

The playtest said: can't see past my own chest, feels janky, no textures. All
three were real, and each exposed a class of defect:

- **The chest chassis** was the player's own armour plate drawn in their face —
  and worse, their own fists physically collided with it (the plate carried
  the default collision category). Machines now have team categories — own
  fists pass through own armour, everyone else's connect — and worn plates are
  simulated but hidden, reported as an instrument instead. Found in the same
  sweep: the enemy's swings had **never once landed** (its club hung along the
  wrong axis, so its cone limit pinned it sideways — 56 m/s of chatter and
  every swing half a metre short), and the damage the playtest saw was the
  player's fists grinding their own plate. The club is flipped, swings drive
  through the chest, and the fight is real both ways now.
- **The jank** was the pages stepping physics once per display frame: a 90 Hz
  headset ran the world 25% fast and every dropped frame stuttered it. The
  arena meters the world with a fixed-timestep accumulator now — 72 steps a
  second whatever the display does.
- **No textures** became a procedural material system in the shader — wood
  grain, stone mottle, brushed steel, floor grid, emissive — plus two-light
  wrap lighting, specular, rim, and distance fog. No image assets anywhere.
  The arena got its dressing the same day: destructible stone pillars (voxel
  grids like everything else), ring glow posts, a fogged skyline, and an
  enemy visor that burns brighter as it winds up — character and telegraph in
  one emissive slit.

#### The first fight is playable (`arena.html`) — the vertical slice

Phase 5's spine, in first playable form. One page, one complete loop:

**Calibrate → raise both fists → fight → win or lose → fists up to rematch.**

The enemy is built from what already existed — the same gravity-compensated
legs, the same inertia-derived uprighting, the same voxel armour riding its
hull — plus a small will: approach to fighting range, square up, telegraph
(club drawn high), swing through the player's chest. Its stone plate strips
under punches; the bared core takes momentum until the drives cut and the
machine drops, bursting what is left of its plate. The player wears steel,
loses it the same way, and has a hull damage limit — so the fight can be lost.

Presentation shipped with it, per the top-tier tracks: **all audio is
synthesised from the physics** (impact thuds and armour clangs scaled by
momentum, slab-tear booms, servo hum following arm speed, win/lose stings —
no recordings), **haptics** pulse on hits taken, a **1 cm / 160 ms view
shake** reads impacts without comfort risk, and the arena ring turns
green/red with the verdict.

Measured, headless: enemy closes from 3 m and strips the player's plate; ten
committed heavy punches kill it through its plate; death drops the machine to
the floor. The whole loop runs in the page harness: gesture start, damage on
both sides, verdict.

Known gaps for the next pass: the enemy has one attack and no footwork
variety; win/lose is signalled by colour and sound rather than text; one
arena, no props; no rounds/score. These are content and polish on a working
spine — exactly where the roadmap wants the project to sit.

#### The campaign: four chapters, a collapse you crawl out of, an ending

`arena.html` is the whole game now. Four opponents, each a place and a story
card: **SCRAPPER** (wood-armoured, small, quick — The Scrapfields),
**WARDEN** (stone, your equal — Milltown Ring), **JUGGERNAUT** (steel, a
quarter bigger, slow and committed — The Foundry), and **THE RETURNED**
(steel, bigger still, fast, and it does not stand on the earth — The High
Dark). The pillars of each arena are built from that chapter's material, so
the place changes with the fight.

The loop: win, and the world rumbles you five seconds down the road to the
next chapter — each win unlocks the next arm weight. Lose and it's the same
fight again. Beat the fourth and the sky is yours; fists up starts the
campaign over with everything you earned. Two systems make the fights
breathe: **heat** — actuator work cooks the arms, past the redline they
derate to a third strength until they cool — and the **second wind**: the
first time your hull gives out, the legs cut and you get twenty-five seconds
to knuckle-haul the dead machine to the repair pad, the same crawl the
crippled machine has owned since the drag spike. Reach it and you stand back
up whole. Miss it and that's the loss.

**What the look-at-it loop caught this round** — five real defects, none of
which any existing test saw:

- The flat preview's clock started at page load, not preview start, so the
  scripted pilot's fists-up window was already over before the first frame
  drew. The screenshots looked like a still life because they were one.
- The fight-start gesture counted *frames* as time — at preview frame rates
  it could never accumulate enough, and in a headset it was riding the edge.
- **Hit-stop froze the game solid.** The engine's hit fields only refresh
  when physics steps run; hit-stop stops the steps; the stale "core was hit"
  field re-armed hit-stop every frame, forever, from the first core strike.
  In a headset the first solid hit would have frozen the fight permanently.
- The collapse required the plate stripped *and* the hull dead — but clubs
  reach the hull around the plate's edges, so hull damage sailed past the
  death line with the plate still half on and nothing happened. The plate
  protects by absorbing, not by postponing the loss.
- The entire heat system ran inside the audio gate — a gameplay system that
  only existed once the player had clicked Enter VR with sound available,
  and never in any test.

The harness now records the sequence of match states a run passes through,
and an undefended run proves the chain end to end: READY → FIGHT →
COLLAPSED (countdown visibly draining) → LOST → rematch rebuilds the
chapter. The flat preview rode chapter 1's win, the travel, and into the
WARDEN fight live — the campaign advances, the unlock ladder pays out.

Where each phase stands in the shipped game: **1–5 are built as designed**
(spikes, voxel core, materials, the mech with heat, the fight). **6** is the
unlock ladder and the four-machine roster — the shop and reputation are
post-1.0. **7** is travel as a transition, not yet a world. **8** is the
story cards and the Returned — a god that hovers — with the ending state;
flight and space stay deferred. That is the full arc at first-playable
depth, honestly labelled.

Known gaps, named: the beaten enemy freezes while you crawl (read it as the
victor watching); travel is a rumble and a name, not a place; the pad
recovery trigger is machine-verified but has not been felt in a headset.
**Playing to win kept legislating.** A crawl script aimed at the repair
pad discovered, in order: that a gripping stroke mid-fight anchors the
moment a knock dips a fist to the floor and hauls the standing machine
out of the arena; that a collapsed hull can land on its own arms; and
that a crawling machine could grind its stroking fists into the frozen
victor and kill it risk-free. That last one forced the rule: **hit the
frozen victor and it wakes** — the freeze is mercy, and attacking ends
it. The collapsed kill stays possible as a knife-fight on the ground,
and the completability proof was rebuilt to win standing — centre
guard, punches at the bobbing core's height — so it holds under the
new rule. A permanent crawl instrument (anchors, hull position, tip
heights, pad bearing) joined the COLLAPSED report.

A property worth knowing about the ground fight: hull damage has no
further consequence during the collapse — the machine is already down —
so the provoked victor's blows cost the crawler **time and position**,
never death. Obstruction, not execution: it can shove you away from the
pad while the clock drains, but the knife-fight is never a trap.

The last reviewed-only path fell to a script that went for the win: an
aggressive metronome (high guard, fast punches at the hovering chest)
now beats the God in the harness every run, and the check asserts the
full chain — FIGHT → WON → ENDING → READY, the fourth win firing the
ending and begin-again booting the next lap. **The campaign is
completable, by proof, in CI.** The attempt also found a real rule
missing from the game: the script killed the God from its knees and
still lost, because the enemy's death only counted during FIGHT. It
counts from COLLAPSED now — beating the machine to death while your own
legs are gone is the best win the game can produce, and the harness run
that discovered it is the permanent proof it works.

**The polish round that followed found the biggest one by thinking like a
headset:** every number the page prints — heat, your own plate, the
collapse countdown — is DOM, and *the DOM does not exist inside VR*. The
player had no instruments at all. All of it is in the world now, diegetic,
no text: heat shows on the metal itself (the arms take on heat colour, the
actuator housings glow outright past the redline), your plate rides your
left wrist as six pips that turn hull-red once the plate is gone, and the
repair beacon *is* the countdown — the column drains like an hourglass and
its pulse quickens as time does. Plus: the victor's visor breathes while
you crawl so stillness reads as watching; the hauler ride thumps on an
uneven road-beat and arrival lands with the sting; the final loss cuts the
drives so your machine drops the way the enemy's does; the ending runs
gold light around the ring; and the README's front door opens into the
game instead of the tumbling-boxes hello-world.

The rule that fell out of it, for every VR page from here on: **if the
player needs it, it lives in the world.** The page card may repeat it; it
may never own it.

#### The deep-polish rounds: variety, truth at scale, measured balance

Fight variety went into the engine: a **second line of attack** (the
lateral sweep, drawn wide instead of high — the pose is the telegraph),
a **stagger** (a slab torn off the plate breaks whatever it was doing
into a long reel), and **blocks as an event** (club momentum landed on a
player arm rings a clang and a short haptic — the parry, working). The
stagger's first cut guarded during recovery, which parked the clubs
across its own core and made the machine accidentally unkillable — so
recovery now reels open-armed, and recovery is the punish window. Two
engine tests grew out of it (66 total).

Looking at chapters 3 and 4 (a `?flat=1&ch=N` preview exists now for
exactly this) found the renderer lying twice: the enemy drew at scale 1
while its physics ran up to 1.35×, landing hits past its visible club
tip; and debris wore the spike pages' toy palette. Both fixed — the
Gods loom at their true size, debris wears its material. Each chapter's
machine also got its own hull (rust, slate, foundry iron, violet dark),
the arenas their own skylines, floors and embers, and travel streams
the destination's skyline past both sides of the road.

**Balance is measured, not felt-out.** A headless probe asks each
chapter two questions: how fast does a steady metronome attacker kill
it, and how long does an undefended hull last. It caught the sweep
doing its job too well (undefended survival fell to 1–3 s mid-campaign,
because the sweep rounds both the guard and the plate). The tunes kept
the physics honest: the sweep telegraphs longer and comes three swings
in eight, and the hull hardens with each unlocked weight class (130 +
45/level) because a Juggernaut's club lands ~130 momentum in one clean
hit and a one-hit collapse is a coin flip, not a fight. After tuning
and the personality biases, measured against each chapter's real hull
limit (single deterministic samples, not averages): undefended survival
runs 19.9 s / 11.0 s / 21.3 s / 4.3 s across the ladder — teach, test,
endure, survive. The overhead-heavy Juggernaut is survivable-but-long
(its slow-crush fantasy: the plate takes what you can see coming); the
sweep-heavy God punishes passivity hardest, which is the finale's job —
with the second wind and the hardened hull as the player's answer, and
heat still taxing heavy-arm offense on the other side of the ledger.

**Each chapter lays its own ground.** Scattered heaps, the mill square,
foundry aisles — and the High Dark strips the cover entirely, because
the finale is a duel and a bare floor says so. The harness caught the
first Scrapfields layout immediately (a heap on the enemy's approach
lane meant the fight never started), which minted the rule: **the
spawn-to-spawn lane stays clear**, |x| under ~1.2 m between the
machines' starting marks.

**Frame cost, measured where it can be:** the full page pipeline —
physics, draw-list build, GL calls into the recording harness — runs
2.2 ms/frame upper bound on server CPU across the arena's whole test
run (13,500 frames including three cold boots). Against the 13.9 ms
budget at 72 Hz, even a conservative 5× Quest CPU slowdown fits; the
untested half is real GPU rasterisation, which only the headset shows.

Attack mix became **personality, derived from traits the machines
already carry**: a hoverer angles in with sweeps (the God comes around
you, four in eight), a slow heavyweight lives on the overhead crush
(two in eight), everyone else keeps the sweep rarer. No new
parameters — hover and tempo were already per-chapter.

Body language closed the round: the approach **weaves** (a tempo-scaled
drift across the bearing, so closing reads as circling, not a rail),
and when your hull gives out for good the victor **stalks to the wreck
and raises both clubs** — its brain runs during the loss for exactly
this, and it can still be killed mid-pose, because a machine
showboating over a live pilot has made a mistake. 68 engine checks and
57 page checks stand behind all of it.

**The second lap.** Finishing the campaign restarts it with unlocks
intact and every machine 15% faster per lap (capped three deep) — "they
remember you." The four-act structure holds; completion now opens a
harder mirror of itself. This is Phase 6's replay ladder in first form.
Completed laps stand as gold posts on the ring, and **the campaign
persists across sessions** — chapter, unlocked classes, current class
and laps save at every milestone, with a Reset campaign button beside
the calibration resets. The flat preview never touches the save, so
the screenshot loop stays deterministic.

**What only a headset can verify now** — the punch list for the next
playtest, in the order the game presents them:

1. The fists-up ghosts: did you understand the gesture without reading?
2. The weave: does the walk-up feel like being hunted?
3. The sweep: can you tell it from the overhead in time to answer it?
4. The parry: does catching a club on your arm feel GOOD?
5. The stagger and the knockdown: do you notice the window and use it?
6. The kill-window notes: did the fight audibly change key?
7. Heat: did the glowing arms warn you before the redline caught you?
8. The collapse: teal patches → grip → haul → beacon — was the second
   wind readable in the panic, and did standing back up feel earned?
9. The travel: does the streaming skyline + road-thump read as going
   somewhere? Is the half-amplitude rumble comfortable?
10. Chapters 2–4: does each machine feel like a different OPPONENT, not
    a bigger number? Does the God's hover + drone + stars land?
11. The triumph: did losing to a machine that stands over you make you
    want the rematch?
12. The ending: gold ring, motes, the long resolve — worth four fights?

**The teaching system, named.** Everything the player must know is
shown in the world, none of it in text: ghost markers float where the
fists go for every start/rematch gesture and brighten as the hold
progresses; teal patches pulse on the floor under each unplanted hand
during the collapse (the ground teaches the grip); the beacon *is* the
countdown; the wrist gauges are the health bars; heat is the colour of
the metal; the pose is the attack telegraph (high = overhead, wide =
sweep) and the visor is its tempo. The dead machine goes limp — drives
cut on the final loss — because a machine that keeps obeying your hands
after it dies was never dead. The hovering God bobs and drones. Comfort
stayed law throughout: travel's sustained rumble runs at half
amplitude, and every shake is centimetres for fractions of a second.

#### Phase 1 status: all three spikes built, awaiting headset verdicts

| Spike | Page | Machine-verified | Needs from the headset |
| --- | --- | --- | --- |
| Piloting feel | `mech.html` | IK arm reaches the hand to ±0.5 cm at every weight; elbows human; weight = momentum | Does it *feel* like driving something massive? |
| Arm-drag locomotion | `drag.html` | Two-fist heave moves the machine; weight grades 2.81 m → 0.53 m; comfort design is hand-caused motion | Does hauling feel physical, and is it comfortable? |
| Hand tracking | `handtrack.html` | Bare hands drive the same machine; pinch and fist gestures read; dropouts counted and blamed on fast motion | Punch fast: are the dropout numbers and the feel good enough to fight with, or are hands for the cockpit and sandbox? |

The hand-tracking verdict decides the split the roadmap anticipated ("hands for
cockpit, sandbox and on-foot; controllers for fights" is a legitimate outcome).
The test room comes next either way; its spawn-menu interactions are shaped by
which input wins, so it follows the verdicts rather than preceding them.

#### Locomotion: the fallback is proven, and it belongs to the crippled machine

The arm-drag spike is built and machine-verified (`docs/drag.html`). Three
findings worth keeping:

- **Geometry assigns the mechanic its role.** Standing at full height the fists
  cannot reach the ground — measured, the tip bottoms out 19 cm up even with the
  pilot crouched. So knuckle-hauling is not a general gait, it is what you do
  when the legs are gone and the hull is on the ground — which is exactly the
  fallback role the design gave it. `w_mech_legs(0)` drops the machine; real
  contact friction resists the drag; `X` toggles it on the page.
- **One planted fist is a push-up, two are a crawl.** Hauling on a single
  anchored fist levers the hull off the ground, friction vanishes, and the free
  arm's swing shoves the machine backwards (measured: −0.36 m in one swing).
  Both fists together keep the belly loaded while the arms haul. The page
  teaches the two-handed heave for this reason.
- **The rig offset is the machine's, not the player's.** Vertically the pilot
  rides a fixed height above the torso every frame — collapse sinks the
  cockpit, standing re-aligns the real floor with the drawn one. Horizontally
  the view is glued to the hull only while it is dragging itself, so real
  walking still moves you 1:1 the rest of the time.

Measured, four two-handed heaves, legs gone: light machine 2.81 m, medium
1.44 m, heavy 0.53 m — same effort, so weight is legible in locomotion exactly
as it is in punching. Grip in mid-air holds nothing. Over-stretched grips slip.
Legs restored, it stands back up to head height.

Comfort note: motion is strictly hand-caused (grab-and-heave), the same class
of locomotion Gorilla Tag ships without a vignette, because self-caused motion
is the most comfortable artificial locomotion known. A vignette pass comes with
the game shell.

#### The arm is solved, not dragged — and a machine must not fight itself

Dragging the far end of a jointed chain with a force and letting the joints
resolve the rest is not how VR arms are built, and several rounds went into
proving it. Every VR body rig solves the arm analytically: shoulder-to-wrist
fixes the elbow bend by the law of cosines, and the one remaining freedom —
where the elbow sits on its circle — is **chosen**, not left to chance. That
choice is what stops the elbow pointing at the ceiling.

What is deliberately *not* copied from those rigs is how the answer is applied.
They pose the skeleton directly, which would throw away the mass, the collisions
and the momentum this game runs on. Here the IK result is a **target**, and the
joints' own springs drive the real bodies to it. Box3D solves those implicitly,
which also removes the stability ceiling every hand-rolled controller in this
project has hit.

**The bug that hid behind all of it: the machine was colliding with itself.**
`collideConnected = false` only covers the two bodies of one joint, so the
shoulder pair was excluded and nothing else was. A folded arm pressed its own
forearm and mount into the torso, and a contact will hold a spring off its
target indefinitely. It presented as a spring that would not reach its target:
9° of error in one pose, 56° in another, worse the tighter the elbow folded, with
no joint limit binding and no controller fighting. Ruling out limits, friction,
the aim torque and the IK itself left only "something is physically in the way".

The tell was pose-dependence. A controller that is mistuned is wrong everywhere;
a controller that is right in one pose and hopeless in another is being
obstructed. Reaching down and away it was accurate to **1°**; folded it was 56°
out. Everything on the machine now shares a collision category that masks itself
out — it still hits blocks, the ground and another mech, it just cannot jam
against its own chest.

Measured after, all four weight classes:

| arm | held still | while moving | punch lag | overshoot | torso shove | elbow |
| --- | --- | --- | --- | --- | --- | --- |
| 3.2 kg | 0.6 cm | 1.6 cm | 7.9 cm | 1.8 cm | 1.3 cm | 0° |
| 7.3 kg | 0.6 cm | 1.8 cm | 14.5 cm | 3.0 cm | 2.3 cm | 0° |
| 14.6 kg | 0.5 cm | 2.0 cm | 22.6 cm | 3.3 cm | 3.0 cm | 0° |
| 25.6 kg | 0.5 cm | 2.3 cm | 28.5 cm | 3.4 cm | 3.3 cm | 0° |

Held still it is at your hand whatever it weighs. Punch and a heavy arm falls
3.6× further behind, overshoots when you stop, and drags the machine off its
footing. Mount jitter went from 5.0°/frame to 0.31. Weight lives entirely in the
momentum, which is what the combat design wanted all along.

Box3D's joint springs are mass-normalised, so a fixed hertz would make a 3 kg
and a 26 kg arm punch identically. Each joint's spring rate is derived from its
own inertia against a fixed actuator stiffness instead — `k = Iω²` solved for
ω — so a heavier limb gets a lower natural frequency for free. That is what
being heavy means.

#### Weight is momentum, not sag

The round after the above shipped, the headset verdict was that the arm still
did not follow the hand — it hung below it — and that the elbow stopped bending
at the heavy settings. Both were the same mistake, and it is worth recording
because it survived a full round of measurement while being defended as a
feature.

Nothing was carrying the arm's own weight. The only upward force was the pull
toward the hand, so the arm settled wherever the spring stretched far enough to
hold itself up: **13 cm below the hand at the lightest weight, 38 cm at the
heaviest, nearly all of it straight down**. That was measured, written up as
"the weight signal", and shipped. In VR it does not read as weight at all — it
reads as the arm ignoring you. Worse, at the heavy end the pull was entirely
spent holding the limb up, leaving nothing to fold the elbow, so the arm locked
out straight with 1–6° of travel.

**The rule this gives:** weight is *resistance to changing velocity*, never
static droop. Your arm does not sag when you hold it out — the muscles carry it,
and heavy is how hard it is to start moving and how hard to stop. Every powered
joint works the same way. So the machine carries its own arm, and mass shows up
through the **capped** pull force: heavier means slower to accelerate, slower to
arrest, and a bigger kick back into the torso. That is also exactly the
momentum-commitment the combat design is built on, so the two wants coincide.

A second, quieter version of the same error: joint friction at 60/40 N·m held
the arm ~12 cm short of the hand *by the same distance at every weight*. A
constant offset regardless of mass is a deadband, not weight. 25/16 N·m brings
it to 3 cm; stiffening the pull past 4000 N/m makes it worse, not better.

Measured on the working build, hands held still and then moved:

| arm | at rest | while moving | elbow travel | torso shove |
| --- | --- | --- | --- | --- |
| 7 kg | 4.0 cm | 7.0 cm | −138°..−101° | 6 cm |
| 17 kg | 3.0 cm | 9.8 cm | −138°..−116° | 8 cm |
| 34 kg | 4.1 cm | 12.4 cm | −138°..−116° | 9 cm |
| 59 kg | 4.2 cm | 14.5 cm | −138°..−117° | 9 cm |

Held still it reaches your hand whatever it weighs. Moving, heavier falls
further behind. The elbow keeps working at every weight. The earlier finding —
that a *mass-normalised* spring cannot convey weight, so the stiffness stays a
fixed N/m — still holds; damping is derived separately so stability never costs
the weight signal.

### Phase 2 — Voxel core
The unified static/dynamic system. **The deformable skin idea and the
loose-dynamic/fixed-static idea are the same system**, and it is the
highest-leverage piece of tech in the project — it's what makes destructible
mechs, buildings and terrain affordable at once.

- Static merged chunks — interior voxels cost near-nothing
- Per-voxel damage accumulation
- **Promotion**: a voxel that loses its bonds detaches and becomes a dynamic body
- **Demotion**: dynamic voxels that come to rest and are supported re-merge to
  static or get culled. Non-optional — without it you leak bodies and the frame
  budget dies ten seconds into every fight
- **A hard budget on live debris**, enforced by culling the oldest fragments
- Skin and interior layers fall straight out of this: the shell is
  dynamic-capable, the layers beneath stay merged until exposed
- **Structural queries** land here too — "is this limb still attached to the
  core?" is what the locomotion ladder reads to know a leg is gone

### Phase 3 — Materials and joint strength
A curated table of ~20 materials: mass, density, bond strength, thermal
properties.

> Use the periodic table as **flavour and a source of plausible numbers**, not
> as literal chemistry. Real bonding energies and alloy behaviour is a deep
> rabbit hole with almost nothing arriving at the player's end. Curated numbers
> give you the entire gameplay surface.

Joint strength belongs here: Box3D's joint motors carry force/torque limits and
the load on a joint can be read back, which is exactly what "strong enough to
move more massive blocks" and "snaps under load" are built from. It feeds
straight back into piloting feel from Phase 1.

### Phase 4 — The mech
A jointed voxel assembly. Build-your-mech from Phase 3 materials, against a
**mass budget set by weight class** — which is what makes a legless build a real
decision rather than a handicap: dropping the legs frees budget for arms and
armour, paid for in the locomotion ladder above.

- **The core** — a component at chest centre; destroy it and the mech dies.
  Cheap to implement, and it's the win condition, so it should exist early.
- **Oil / hydraulics** — build as a **graph flow** problem, not a fluid sim.
  Components are nodes, lines are edges, the pump is the root. Sever a line,
  recompute reachability, unreachable components degrade and fail. Deterministic,
  nearly free, and it reads to the player exactly like the intent. Leaking oil
  as particles and decals sells the rest. A real fluid sim would be beautiful
  and unaffordable.
- **Overheating** picks up the temperature spike from Phase 1 and gives it a job.
- **First audio and game-feel pass.** The mech is the first thing in the project
  that has to *feel* like something, so the quality tracks start here rather
  than at the end.

### Phase 5 — The fight *(playable spine complete)*
One arena, one opponent, win/lose. A complete game.

**Escaping your fallen mech on foot** belongs here: the last rung of the
locomotion ladder and a loss state that isn't a game over. Mechanically it's the
roomscale locomotion that already exists, which is why the scale contrast lands
so hard for so little work — and it's where the oxygen spike from Phase 1 finds
its purpose.

This is the first point where the game can be honestly judged by playing it, so
it's also the first real playtesting milestone.

### Phase 6 — Meta layer
Credits, upgrades, add-ons, reputation, opponent roster, arena and environment
variety. The *One Must Fall* circuit, organised by weight class — you climb the
ladder, and building back down a class for a different kind of fight stays a
legitimate choice rather than a step backwards.

Upgrades buy torque, which the player feels as responsiveness, so the shop has
a direct line to how the mech handles in their hands.

### Phase 7 — Travel and the open world
Town-to-town voxel world with fuel as the constraint.

> A streaming open voxel world on standalone hardware is a multi-month project
> **by itself**, larger than several earlier phases combined. Start with a hub
> and discrete destinations, prove fuel and the economy work, and only commit to
> seamless open world if the game is actually asking for it by then.

### Phase 8 — Story, gods, flight, space
Post-disclosure world; hybrids among us; other species on Earth and in the
oceans; the gods returning on a timer to wipe and reseed the world. The player
uncovers the plot and fights them in space. The timer flexes with how hard the
player pursues it — a player who only wants the fight circuit never has to
engage.

Almost none of this constrains architecture, which is why it defers safely. The
two parts that *are* technical:
- **Iron Man flight** — a new locomotion mode with a serious comfort risk. Spike
  it early even though you build it late.
- **Zero-g space combat** — a different physics config, cheap once flight works.

---

## Cross-cutting tracks

**Art direction, audio, game feel, comfort** — see the quality bar above. First
pass at Phase 4, continuous from there.

**Environment systems** — temperature (freeze the water, make people shiver) and
oxygen (remove it, watch them collapse). Cheap sandbox spikes; run them whenever
you want a break. They stay flavour until something needs them, and both find a
job later: temperature becomes mech overheating (Phase 4), oxygen becomes life
support when you eject or fight in space (Phase 5/8). Prototype now, promote
when the game gives them meaning.

**NPCs, animals, plants, water** — sandbox residents first, content later. Don't
build an AI system; build one creature that does one thing and see whether the
room feels alive.

**Tooling** — with no deadline, tools pay for themselves. A mech editor, a
material tuner and a scenario loader in the sandbox will each cost days and save
weeks. Build them when the third round of manual fiddling annoys you, not before.

**Template repo** — do it, but not first. You extract a template from a
foundation that's proven, and this one is about to be substantially rewritten in
Phases 0–2. Middle path: **write the setup notes now** while the pain is fresh
(an hour, and it's the part that gets forgotten), extract the code template once
Phase 2 stabilises.

---

## Open questions

| Question | Decide by | Notes |
|---|---|---|
| Voxel size | **After Phase 2, not before** | Phase 0.5 measured it: fully-shattered mechs bound out at ~300 voxels each, but merging changes the constraint entirely. Pick this once merging exists |
| Live debris budget | Phase 2 | The measurements say *this*, not voxel size, is the number worth designing against |
| Mech scale | Phase 0.5, empirically | "Giants" is a feeling, not a number; the number drives voxel count, arena size, and how good the on-foot escape feels |
| Target refresh rate | Phase 0 | 72 Hz rock-solid beats 90 Hz with drops for a physics-heavy game; revisit once the Phase 0 number is known |
| Hands or controllers for combat | Phase 1 spike | Split answers are fine and probably correct |
| How legged walking is driven | Phase 4 | Fully physical gait is research-grade; expect a driven gait with physics reaction |
| Roomscale driving the mech directly | Phase 4 | Only coherent for a cockpit-less build; bounded by guardian size |
| Cockpit fidelity | Phase 4 | Full interior sells scale and is the comfort rest frame, but costs frame budget |
| How close to 1:1 at the top of the upgrade curve | Phase 5 | Must stop short of perfect tracking or the best mech stops feeling like a mech |
| How many weight classes, and their spread | Phase 6 | Needs enough separation that each feels distinct, few enough that each gets content |
| Art direction | Phase 4 | Needs to survive a voxel mech, a town, and deep space |

---

## Idea index

Everything from the original notes, and where it lives.

| Idea | Phase |
|---|---|
| Bump Box3D to v0.1.0, SIMD, threading, instancing, perf HUD | 0 |
| Worst-case destruction load test | 0.5 |
| Test room / sandbox | 1, grows throughout |
| Hand tracking (responsiveness, no batteries, nothing to break) | 1 |
| Arm-drag locomotion (Underdogs-style anchor and pull) | 1 — spike; becomes the damage fallback at 4 |
| Deformable skin over static-until-revealed layers | 2 |
| Loose voxels dynamic, fixed voxels static | 2 (same system as above) |
| Voxel materials with bonding, mass, density | 3 |
| Joints with strength / power to move massive blocks | 3 |
| The heart / core at the chest | 4 |
| Oil and hydraulic fluid as a blood-like resource, arteries, failure | 4 |
| Build your mech from materials, including legless builds | 4 |
| Stick-driven walking; locomotion degrading with damage | 4 |
| Mass budget per weight class | 4 |
| Mech fight, arenas, opponents | 5 |
| Momentum and committed attacks — emergent, not scripted | 5 (falls out of 3) |
| Physical fights; intensity varying by weight class | 5 |
| Escaping your fallen mech on foot | 5 |
| Credits, upgrades, add-ons, reputation | 6 |
| Weight classes, climbing from light mechs to god-killers | 6 |
| Responsiveness as the upgrade axis, approaching 1:1 | 6 (mechanism from 3) |
| Town-to-town travel, fuel | 7 |
| Alien disclosure, hybrids, the returning gods, the flex timer | 8 |
| Iron Man flight, space combat | 8 |
| Temperature, oxygen | Cross-cutting; promoted at 4 and 5 |
| NPC AI, animals, plants, water | Cross-cutting |
| Art direction, audio, game feel, comfort | Cross-cutting; first pass at 4 |
| Template repo for future Box3D VR games | Cross-cutting; notes now, extraction after 2 |
