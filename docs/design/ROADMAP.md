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
3. **Measure before you design.** The perf HUD (Phase 0) exists so that every
   later decision is made against real numbers instead of intuition. Quest 3 is
   a fixed budget; the numbers decide the design, not the other way round.
4. **Keep a playable spine.** From Phase 5 on there is always a complete
   game — two destructible mechs, a killable core, one arena — and every later
   phase is an addition to something that already works. This isn't about
   shipping early; it's so quality can be judged by *playing* rather than by
   imagining the finished thing.
5. **Take the time.** There is no deadline. Where there's a fast path and a
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
