# The record

What this project is, what has been measured, and what is known to be wrong.

This document was reset when the project was. The version before it described a
five-chapter mech campaign — weight classes, heat, a second wind, a core you
broke to win, four opponents and a god that hovered — which was built, shipped
and then judged the wrong direction by the only person whose judgement counts.
Nearly all of it is gone. What follows is what was kept, why, and what has been
measured since.

---

## What was kept, and what was left

**Kept, because it worked:**

| | |
|---|---|
| **Deformable mesh** | A real subdivided mesh whose vertices cave in around each contact point, with a raised rim, permanently, normals recomputed from the crumpled triangles. The one piece of the old project that was unambiguously right. It used to be a practice box you punched; now it is what the entire opponent is made of. |
| **Voxel destructible matter** | A voxel is a cell in a grid, not a physics body. Intact cells greedy-merge into runs, damage is momentum through contact hit events, live debris is hard-capped, and a region that loses its structural connection detaches. This is what makes per-bone damage affordable. |
| **Passthrough** | `immersive-ar`, transparent clear, nothing drawn but the fight. It was one build's work in the old project and it is now the default way in. |
| **Machine-check everything except feel** | The engine suite and the page harness. A page can crash on its first frame and look identical to a page that never loaded — black, no error. That happened three times before the harness existed. |

**Left behind:**

- The five-chapter campaign, travel, unlock ladder, laps, NG+, saved progress.
- **The core.** The old game was called CORE BREAKER: strip the armour, smash
  the component at chest centre, win. The whole idea is gone. There is no
  special point to find; you take the body apart.
- The player's own mech: IK arm chains, torque curves, weight classes, heat and
  the redline, the collapse and the knuckle-haul crawl, the repair beacon.
  Your hands are your hands now.
- Arena dressing: pillars, ring posts, skylines, floors, embers, story cards.
- Every spike page (`mech`, `drag`, `handtrack`, `vox`, `dummy`, `pilot`,
  `follow`, `hands`, `bench`). Each proved something; each was then superseded
  or abandoned, and they were nine pages of maintenance for a project with one.
- **The native OpenXR Android app, and the headless benchmark that sized it.**
  There is no PC to sideload from, so it had never once run on the target
  hardware, and the browser build is what actually reaches the headset. Both are
  in git history if they are ever wanted back.

Line count went from roughly 18,000 to 4,700.

---

## What it is now

Passthrough VR. One opponent, standing on the player's real floor, sized to the
player, built on a human skeleton.

### The skeleton

Seventeen bones, sixteen joints, laid out on standard anthropometry — every
length is a fraction of standing height (ankle .039, knee .285, hip .530,
waist .620, shoulder .820, chin .845, crown 1.000). That is why the silhouette
comes out human without anyone art-directing it.

```
pelvis - abdomen - chest - neck - head          the spine, five bones
chest  - upper arm - forearm - hand             each arm, three
pelvis - thigh - shin - foot                    each leg, three
```

Shoulders, hips, spine, neck, wrists and ankles are ball joints with cones.
**Elbows and knees are hinges**, and they bend the way a person's bend — the
elbow forward, the knee back. That is most of why it reads as a body.

The whole thing is built in the rest pose with every body at identity rotation,
which is what makes an identity joint target mean "stand" and a pose a rotation
away from standing.

**Measured, at 1.75 m stature:** 55.3 kg, hip at 0.927 m, shoulder-to-fingertip
reach 0.779 m, crown within 10 cm of stature while standing, ankles at 0.069 m
(the floor). Stature is taken live from the tallest the player's head has been
since entering, divided by 0.936 — no calibration gesture, and it self-corrects
if they started seated.

### Damage, and what breaking means

Every bone is a voxel grid riding its own body, hidden from the engine's box
renderer and drawn by the page as a deformable mesh. Cells *are* the collision
shapes, so a bone genuinely gets less there as it is beaten away.

A bone battered past a third of itself **breaks**: its joint to its parent is
destroyed and it falls, taking every bone below it. Break an elbow and the
forearm and the hand go together — nothing in the code says so, the skeleton
does.

It goes down when it cannot stand: both legs gone, or the spine broken. **There
is no core.** Losing one leg halves the weight it can carry, so the hip target
drops and it goes down on that knee and stops walking — a distinct state, not a
debuff. Losing both arms means it can no longer throw anything.

**Measured toughness** — a scripted straight-line punch, 4 kg of arm behind a
1.4 kg fist, aimed square at one bone:

| bone | cells | 20 aimed punches leave it | broke at |
|---|---|---|---|
| hand | 2 | 100% beaten in | 5 punches |
| upper arm | 3 | 100% | 11 |
| thigh | 3 | 71% | 15 |
| head | 8 | 41% | 49 |
| foot | 8 | 12% | 58 |
| forearm | 4 | 60% | not in 60 |
| pelvis | 2 | 34% | not in 60 |
| abdomen | 8 | 23% | not in 60 |
| chest | 12 | 20% | not in 60 |
| shin | 4 | 17% | not in 60 |
| neck | 8 | 0% | not in 60 |

In a real fight (the page harness, a pilot who guards, watches it come, then
works it over) it goes down in about **32 landed blows, four bones broken**,
while landing 25 on the player.

Two things in that table are known to be wrong and are recorded rather than
quietly fixed:

- **The shin is tougher than the thigh**, which is backwards — the shin is the
  more exposed target and sits at a comfortable height. It is an artefact of the
  cell count: four cells need three dead to break where three cells need two,
  and a thinner bone is also simply harder to hit.
- **The neck reads 0%** because the head is next to it and the damage router
  picks the nearest grid. Punches aimed at a neck land on a head.

Both are numbers a headset session can move; neither is a structural problem.

### Its will

`WAIT` (guard, sway) → `STEP` (closes, weaving across the bearing) → `WINDUP`
(the arm draws back — the pose is the entire telegraph) → `STRIKE` → `RECOVER`
(open, and deliberately long enough to punish) → back to `WAIT`. Plus `FALLING`
and `DOWN`, which nothing recovers from.

No parry system, no stagger, no heat, no corner, no triumph pose. One line of
attack, left and right, readable from the arm.

### Frame budget

A step of the whole fight — physics, the figure's controller, damage routing —
measures **well under 1.4 ms on CI**. A Quest 3 runs this build roughly 2.5×
slower and the budget at 72 Hz is 13.9 ms for everything including the GPU. The
untested half is real rasterisation, which only a headset shows.

---

## What was learned building it

Recorded because every one of these cost a round, and every one of them was
silent — nothing errored, nothing warned.

**1. A gain belongs to the body the force is applied to.**
The balance controller went through three wrong shapes. Sized against the whole
assembly's inertia and applied to the pelvis, it multiplied the loop gain by the
ratio between them — about forty — and the figure span through two radians a
second while standing still. Sized against the pelvis alone it was stable and
far too weak to turn a 55 kg body: 10 N·m every step for three seconds moved the
heading a sixteenth of a radian. Pelvis stiffness with assembly damping needs
`c·dt/I < 2` to survive an explicit step and came out at 3.2.

There is no gain that satisfies all three, because the premise was wrong. A body
does not stand and turn by torquing its own pelvis. **Solve for the acceleration
the whole body should have — linear and angular — and ask each bone for its
share: force is its mass times that acceleration, torque is its inertia times
it.** Stability then depends only on the chosen period against the timestep,
which is a number you can check by eye.

**2. Joint springs are muscle, and muscle is stiff.**
The first cut ran the spine at 5–9 Hz. Every joint sagged a little under gravity,
the sags added down the chain, and the figure folded forward over its own hips
and face-planted inside four seconds without being touched — chest 25 cm low,
head 68 cm low. It never errored. It just slowly lay down.

**3. The leg chain and the stand height must agree to the millimetre.**
The cell count sets the cell *size*, so it decides a bone's other two dimensions
too. The foot's count of 2 gave a correct 27 cm long foot and a 13 cm **thick**
one. Six centimetres of extra ankle drove the soles through the floor, the leg
chain jammed, and floor friction pinned the figure so hard it could not turn: 10
N·m of heading torque went in every step for three seconds and moved it 0.06
radians. The symptom looked nothing like the cause.

**4. An explicit spring has a stiffness ceiling; an implicit one does not.**
The player's fists were first driven by a force, `k(target − p) − cv`, applied
every step. At 72 Hz that is stable only while `c·dt/m < 2`, and a fist crisp
enough to feel like your own hand needs stiffness well past it: the numbers that
felt right measured 2.5, and the fist crept 58 mm through a target it was
supposed to be sitting on. Softening it until the arithmetic was safe capped
tracking at about 5 cm of lag per metre per second — mush, which is a verdict
this project has already collected once. Box3D solves joint springs implicitly,
so a **motor joint** has no such ceiling. The force cap survives and now does
something honest: press your fist into something immovable and it stops.

**5. A fast fist needs continuous collision.**
The moment the fists got stiff enough to keep up with a hand, the entire damage
suite went silent. At 20 m/s a fist covers 28 cm in one step and a limb is 14 cm
thick — it was passing straight through. `isBullet` is exactly what that flag is
for.

**6. A punch has to stop at the surface.**
Driving the target through to the far side sounds harmless — a real punch does
follow through — but a stiff continuous fist ends the stroke wedged inside the
body, and the retract drags it back out through whatever is in the way. Logged
hit by hit, punches aimed squarely at one thigh were landing on the other one,
on the pelvis and on the abdomen. Damage is momentum at the moment of contact;
where the stroke ends afterwards contributes nothing but mess.

**7. Look at it.**
Every measurement passed while the two legs read as a single uninterrupted
column with feet on the bottom — same cell size for thigh and shin means the
same thickness, so there was no knee in the silhouette, and they were 16 cm
apart and 14 cm thick so they touched all the way down. One render showed it in
a second. The taper comes from the distal segment's cell count; the separation
comes from a stance with the feet wider than the hips.

The same loop caught the preview camera building its right vector with the wrong
sign — a clean 180° roll, floor across the ceiling, figure hanging off it.
Nothing errored, because an upside-down camera is a perfectly valid camera.

**8. Read the joints, don't infer them.**
`w_fig_joints`, `w_fig_rig`, `w_fig_pose` and the per-bone damage figure all
exist because guessing was more expensive than reading. The damage figure in
particular: a cell only leaves the alive count when it is *completely* gone, so
four solid punches into a tough limb read as no progress at all. Everything
between "untouched" and "destroyed" was invisible from outside until it was
exported.

---

## What only a headset can answer

In the order the game presents them:

1. **The gesture.** Fists above your head — did you understand it without
   reading anything?
2. **Scale.** It is built to your height. Standing in front of it, does it feel
   like a person your size, or like a prop?
3. **Your fists.** Do the gauntlets feel like your hands, or like something
   trailing them?
4. **The punch.** When you land one, does it feel like you hit something? Try
   `?power=0.6` and `?power=2` and say which is closer.
5. **The dents.** Can you see where you have been hitting it? Does the metal
   record the fight?
6. **The break.** When a bone comes off and the arm below it drops — does that
   land, or does it just vanish?
7. **The telegraph.** The arm draws back for 0.26 s. Is that enough to read and
   answer, or too much, or not enough? `?tempo=` moves it.
8. **The legs.** Taking one drops it to a knee. Is that readable as a thing you
   did, or does it just look like it fell over?
9. **Passthrough.** Does a figure standing on your actual floor land differently
   from one in a void? That is the whole bet.

Anything about **proportions or silhouette** — whether it looks like the right
kind of body — wants a reference image rather than a guess.

---

## Where it could go

Nothing here is committed. Written down so it is not re-invented.

- **Materials.** The matter system already has a table (density, toughness,
  colour); the figure uses one row of it. Different figures made of different
  stuff is a table edit, not code.
- **More than one.** Nothing in the engine says there is one figure; the grid
  budget currently says so (20 grids, 17 per figure).
- **Hands.** Hand tracking was proven on the old project and is a natural fit
  for a game whose only input is your fists.
- **Where the blows land on you.** The figure's reach is measured and reported;
  nothing is done with it beyond a jolt and a buzz. There is no player health,
  deliberately.
- **Weight in the fist.** Your fist currently sits exactly where your hand is.
  Whether a *heavier* fist that lags is better is a headset question, and the
  old project's answer was that lag read as mush rather than as mass.

---

## Next: the body has to take a punch

**Asked for, in his words:** *"take the hit and recover like a boxer, but also go
down when knocked down or out."* So: an active ragdoll. A blow makes the body
give, it gathers itself back into its stance, and when it is finished it stops
gathering and falls.

### What was already tried, and why it is not the answer

`w_fig_apply` accelerates the whole body back toward a stance point every frame.
That looked like the cause — hit it, and the controller hauls it straight back
the same frame, which is what "floating around a midpoint" describes.

Fading that controller out on impact and back in over a stagger **makes it
worse**, measured on a scripted straight punch into a thigh:

| | hip displacement |
|---|---|
| stance controller untouched | **104 mm** |
| controller faded on impact | **68 mm** |

Less, not more — because that same controller is what walks it toward you, so
switching it off mostly stopped it closing. The stance point is not the lump.

### What the lump actually is

Every bone is held in its pose by a joint spring standing in for muscle, and
those springs are stiff by design — see *"Joint springs are muscle, and muscle is
stiff"* above, where a soft spine folded the figure onto its own face in four
seconds. Stiff everywhere means the whole skeleton answers a punch as one solid
assembly swinging about a point, instead of a body that gives where it was hit.

**So the ragdoll lives in the joint springs, not in the stance controller.**
Slacken them on impact — hardest at the bone that was struck and falling off
along the chain away from it — then stiffen back over roughly the length of a
stagger. The head snaps, the arm whips, the torso gives, and then it gathers
itself up. That is the boxer.

### Going down is probably already there

`w_fig_apply` returns early on `FIG_FALLING` and `FIG_DOWN`, so a finished figure
already stops driving itself and falls. If the joint springs also go slack — and
stay slack — on entry to those states, "goes down when knocked out" comes out of
the same mechanism as the hit reaction rather than needing its own.

### Also outstanding, from the same session

- **The telegraph is too fast to read.** The arm draws back for 0.26 s and the
  verdict was that it could not be seen at all. Lengthen it. This is a number,
  not a design question.
- **Taking a leg works.** Punch one off and it drops to that knee — confirmed in
  the headset, leave it alone.
- **Do not ask him to type URL parameters.** `?tempo=` and `?power=` were offered
  as a way to find the feel and are invisible to someone who does not edit web
  addresses. If a number needs finding by hand, it needs to be reachable from
  inside the headset.

### Standing on its feet: what the naive version does

The lift is currently sprayed over every bone — each one gets force = its mass
times the body's wanted acceleration. That is why the figure reads as suspended
rather than standing, and why a severed limb hangs in the air and a broken bone
stands itself up off the floor: a loose piece keeps drawing its own slice of
lift and its own share of the righting torque.

The obvious fix is to put the weight in at the FEET and let it travel up the
skeleton through the joints. **Applying an upward force to the foot bodies does
not do this.** Measured, with the whole body's weight split between both feet:

```
FAIL  it stands, and keeps standing        hip 0.885, want 0.927
FAIL  its feet are on the floor            ankle at 0.206 m
FAIL  its head is at your eye line         crown at 0.970 m
37 passed, 9 failed
```

The feet get lifted off the floor while the body sags underneath them — force
applied to a foot accelerates the foot, it does not push the body up. Real
standing takes its upward push from the ground pressing BACK, which only happens
if the legs actively extend against the floor. So this wants leg actuation —
drive the knee and hip to hold the hip at its target height, and let contact
supply the reaction — not a relocated force. Bigger job than it looks, and it
interacts with the joint stiffness the impact-slack now reduces.
