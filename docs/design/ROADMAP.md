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
2. **Every milestone ends with something testable in the headset.** Not a
   passing unit test — something you put the Quest on and try.
3. **Measure before you design.** The perf HUD (Phase 0) exists so that every
   later decision is made against real numbers instead of intuition. Quest 3 is
   a fixed budget; the numbers decide the design, not the other way round.
4. **Shippable at Phase 5.** Two destructible mechs, a killable core, one arena
   is a complete game. Everything from Phase 6 on is expansion on something
   that already exists. Protect that property.

---

## Decisions already made

**Piloting: you *are* the mech, with diegetic controls.** Modelled on
[Underdogs](https://www.roadtovr.com/underdogs-vr-mech-preview-quest-pc-vr/) —
you sit in a cockpit as a pilot avatar visibly gripping controls, and your real
arm motion maps 1:1 to the mech's arms at scale. This is the single most
important decision in the project and it pays off three ways:

- **Mass is felt, not displayed.** Your hand becomes a target; the mech's arm is
  a motorised Box3D joint chasing that target with a *limited* torque. A heavy
  arm lags behind your hand. Upgrading a joint's strength changes how the mech
  feels in your body, which turns the material and joint-strength systems
  (Phase 3) into core game feel rather than a stats screen.
- **Locomotion is physics-native.** Underdogs' grip-to-anchor movement — plant a
  fist, pull yourself along — is a constraint plus an impulse, which Box3D does
  natively. No separate character controller, and no artificial stick locomotion,
  which means no nausea.
- **The cockpit hides the seams.** A pilot avatar holding grips gives tracking
  latency and hand/mech mismatch somewhere to live. Direct un-mediated limb
  mapping has no such cover.

**Consequence for hand tracking (Phase 1):** the anchor/release verb is the
whole locomotion scheme, and on controllers it's the grip button. With hands
it becomes a fist clench — detectable, but with no haptic confirmation of the
moment the anchor bites. That is the specific thing the spike has to answer.

---

## Phases

### Phase 0 — Foundations
> Plan: [`phase-0-foundations.md`](phase-0-foundations.md)

Bump the Box3D pin to the v0.1.0 release (joints and multi-shape bodies are
required by nearly everything downstream). Turn SIMD on, wire up Box3D's task
system, replace the per-body draw call with instancing, decouple physics from
the render rate, and build the perf HUD.

**Exit criterion is a number**, not a feature: the maximum dynamic body count
that holds 72 Hz in-headset. That number is an input to every phase below.

### Phase 0.5 — Worst-case spike
Before designing anything on top: spawn the genuine worst case — two
mech-sized voxel assemblies, smashed together — and read the HUD. This is the
hardest sustained load in the entire project. If it doesn't fit, it changes
voxel size, mech size and chunking *now*, rather than after three phases have
been built on the assumption. Cheap to run; potentially saves months.

### Phase 1 — Sandbox + hand tracking
The test room, minimal: flat room, spawn menu, object inspector, perf HUD
always visible. This is the development harness for everything that follows, so
it comes early and grows continuously.

Then two spikes:
- **Hand tracking** — test the *hard* case (fast, aggressive, occluded combat
  motion and the fist-clench anchor), not the easy case. Hand tracking is good
  on Quest 3 for deliberate manipulation and weakest at exactly what a fighting
  game is made of. A legitimate outcome is "hands for cockpit, sandbox and
  on-foot; controllers for fights."
- **Piloting feel** — force-limited motor targets driving a mech arm from a
  hand pose. The smallest possible version: one arm, one target, one weight.

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
- Skin and interior layers fall straight out of this: the shell is
  dynamic-capable, the layers beneath stay merged until exposed

### Phase 3 — Materials and joint strength
A curated table of ~20 materials: mass, density, bond strength, thermal
properties.

> Use the periodic table as **flavour and a source of plausible numbers**, not
> as literal chemistry. Real bonding energies and alloy behaviour is a deep
> rabbit hole with almost nothing arriving at the player's end. Curated numbers
> give you the entire gameplay surface.

Joint strength belongs here: Box3D's joint motors carry force/torque limits and
the load on a joint can be read back, which is exactly what "strong enough to
move more massive blocks" and "snaps under load" are built from. It also feeds
directly back into piloting feel from Phase 1.

### Phase 4 — The mech
A jointed voxel assembly. Build-your-mech from Phase 3 materials.

- **The core** — a component at chest centre; destroy it and the mech dies.
  Cheap to implement, and it's the win condition, so it should exist early.
- **Oil / hydraulics** — build as a **graph flow** problem, not a fluid sim.
  Components are nodes, lines are edges, the pump is the root. Sever a line,
  recompute reachability, unreachable components degrade and fail. Deterministic,
  nearly free, and it reads to the player exactly like the intent. Leaking oil
  as particles and decals sells the rest. A real fluid sim would be beautiful
  and unaffordable.
- **Overheating** picks up the temperature spike from Phase 1 and gives it a job.

### Phase 5 — The fight *(shippable)*
One arena, one opponent, win/lose. The complete game.

**Escaping your fallen mech on foot** belongs here: the loss state that isn't a
game over. Mechanically it's the roomscale locomotion that already exists, which
is why the scale contrast lands so hard for so little work — and it's where the
oxygen spike from Phase 1 finds its purpose.

### Phase 6 — Meta layer
Credits, upgrades, add-ons, reputation, opponent roster, arena and environment
variety. The *One Must Fall* circuit.

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
- **Iron Man flight** — a new locomotion mode with a serious comfort/nausea
  risk. Spike it early even though you build it late.
- **Zero-g space combat** — a different physics config, cheap once flight works.

---

## Cross-cutting tracks

**Environment systems** — temperature (freeze the water, make people shiver) and
oxygen (remove it, watch them collapse). Cheap sandbox spikes; run them whenever
you want a break. They stay flavour until something needs them, and both find a
job later: temperature becomes mech overheating (Phase 4), oxygen becomes life
support when you eject or fight in space (Phase 5/8). Prototype now, promote
when the game gives them meaning.

**NPCs, animals, plants, water** — sandbox residents first, content later. Don't
build an AI system; build one creature that does one thing and see whether the
room feels alive.

**Template repo** — do it, but not first. You extract a template from a
foundation that's proven, and this one is about to be substantially rewritten in
Phases 0–2. Middle path: **write the setup notes now** while the pain is fresh
(an hour, and it's the part that gets forgotten), extract the code template once
Phase 2 stabilises.

---

## Open questions

| Question | Decide by | Notes |
|---|---|---|
| Voxel size | Phase 0.5, empirically | Drives destruction fidelity, body counts, mech silhouette, and whether Phase 7 is possible at all |
| Mech scale | Phase 0.5, empirically | "Giants" is a feeling, not a number; the number drives voxel count, arena size, and how good the on-foot escape feels |
| Hands or controllers for combat | Phase 1 spike | Split answers are fine and probably correct |
| Cockpit fidelity | Phase 4 | Full interior sells scale but costs frame budget; measure against the Phase 0 number |
| Fight pacing | Phase 5 | Underdogs is physically exhausting by design. Decide whether that's the goal |

---

## Idea index

Everything from the original notes, and where it lives.

| Idea | Phase |
|---|---|
| Bump Box3D to v0.1.0, SIMD, threading, instancing, perf HUD | 0 |
| Worst-case destruction load test | 0.5 |
| Test room / sandbox | 1, grows throughout |
| Hand tracking (responsiveness, no batteries, nothing to break) | 1 |
| Deformable skin over static-until-revealed layers | 2 |
| Loose voxels dynamic, fixed voxels static | 2 (same system as above) |
| Voxel materials with bonding, mass, density | 3 |
| Joints with strength / power to move massive blocks | 3 |
| The heart / core at the chest | 4 |
| Oil and hydraulic fluid as a blood-like resource, arteries, failure | 4 |
| Build your mech from materials | 4 |
| Mech fight, arenas, opponents | 5 |
| Escaping your fallen mech on foot | 5 |
| Credits, upgrades, add-ons, reputation | 6 |
| Town-to-town travel, fuel | 7 |
| Alien disclosure, hybrids, the returning gods, the flex timer | 8 |
| Iron Man flight, space combat | 8 |
| Temperature, oxygen | Cross-cutting; promoted at 4 and 5 |
| NPC AI, animals, plants, water | Cross-cutting |
| Template repo for future Box3D VR games | Cross-cutting; notes now, extraction after 2 |
