# Codex Physics Robot Design

## Goal

Add a visibly red Codex robot to the first fight alongside the existing grey
Claude robot. Preserve the existing robot's controller and behaviour while the
Codex robot uses a separate, physics-first controller. Both robots target the
player, occupy the same Box3D world, and collide with one another without
deliberately fighting each other.

## Constraints

- Keep the existing `w_fig_*` controller behaviour unchanged.
- Reuse the existing anthropometric body, voxel damage, deformable meshes, and
  breakage rules where they remain physically appropriate.
- Do not hold the Codex robot upright with direct pelvis forces, distributed
  world-space forces, gravity cancellation, teleportation, or airborne
  righting torque.
- All intentional Codex movement must be produced by torque-limited internal
  joint actuators and external contact with the floor or other bodies.
- Prefer physical honesty over guaranteed success. The robot may stumble, miss,
  fail to recover, or remain unable to rise after damage.
- Keep the fixed 72 Hz simulation and remain within the Quest 3 frame budget.

## Scene and engine separation

The existing grey robot remains in the `w_fig_*` namespace with its existing
state and controller. The Codex robot receives a new `w_codex_*` namespace and
independent state. Both are created in the same Box3D world so contact impulses
between them are real.

Each articulated body uses its own collision category. Self-collision remains
disabled within a robot, while grey-to-red, robot-to-player-fist, robot-to-floor,
and robot-to-debris collisions are enabled. Voxel grids record their owning
robot so damage, breakage, mesh deformation, reports, and debris remain
independent.

The page maintains separate mesh collections for the two figures. Shared mesh
construction and deformation helpers operate on a figure descriptor rather
than on the grey singleton. The Codex descriptor supplies its own grid IDs and
a red material; the grey descriptor continues to use its current colour and
exports.

The existing overhead-fists gesture creates both figures at offset positions in
front of the player. Each controller targets only the player. Their bodies can
obstruct, trip, shove, knock down, or accidentally intercept blows intended for
the player, but neither AI chooses the other as a target.

## Codex skeleton and actuation

The Codex robot starts from the existing 17-body anthropometric skeleton:
pelvis, three-part trunk and head, three bodies per arm, and three bodies per
leg. It adds a deformable forefoot/toe body and hinge on each side. The toe
hinges provide a real rocker and push-off point for gait, stumbling, kneeling,
and rising. No other joint is added unless testing identifies a specific
missing physical degree of freedom.

Shoulders, hips, spine, neck, wrists, and ankles use bounded spherical joints.
Elbows, knees, and toes use bounded revolute joints. Anatomical limits prevent
invalid poses. Box3D joint motors act as muscles with finite torque, compliance,
and damping. Their equal-and-opposite reactions remain internal to the
skeleton. Torque limits scale with stature, segment inertia, leverage, and the
remaining material in the actuated bone chain.

The root is always dynamic. There is no root target, kinematic animation body,
or external pose correction.

## State estimation and balance

Every fixed step, the Codex controller measures rather than assumes:

- per-foot and per-toe contact and normal impulse;
- centre of mass and linear/angular momentum;
- the current support polygon;
- joint positions and velocities;
- torso orientation;
- remaining structural connections and actuator strength.

While standing, ankle, toe, hip, and spine motor targets regulate the capture
point inside the support area. These are joint commands only. The floor reaction
under the loaded feet is the sole source of external support and horizontal
acceleration. If the requested correction exceeds reachable joint torque or the
support area, the robot must step or fall.

## Locomotion

The controller uses a contact-driven gait rather than a timed root animation:

1. Select a reachable capture point from current momentum and the desired
   direction toward the player.
2. Shift load through the stance leg using ankle, toe, hip, and trunk torque.
3. Confirm that the swing foot is unloaded before lifting it.
4. Swing the leg toward the capture point using hip and knee motors.
5. Accept the first physically valid foot or toe contact.
6. Transfer load, reassess momentum, and either stabilize, take another step,
   stumble, or fall.

With floor friction removed, the controller must be unable to translate itself
forward. When airborne, the centre of mass must follow a ballistic trajectory
apart from external collisions.

## Attacks and hits

A Codex punch is a coordinated physical action: stance and weight transfer,
trunk rotation, shoulder drive, and elbow extension. The hand has no world-space
target joint and is never dragged to the player. Reach, speed, accuracy, recoil,
and recovery emerge from available joint torque and contact.

Damage is derived from solver contact data: contact point, normal, relative
velocity, effective mass, and impulse. Gentle robot-to-robot contact pushes
without damage. A concentrated high-speed accidental impact may dent or break
material. The controller does not check attack-state flags to decide whether a
contact counts.

## Falling, recovery, and life

A fall is not a scripted state transition that places the body on the floor. It
is the result of momentum leaving the support region when available joint and
contact authority cannot recover it. During a fall, the robot may brace with an
available arm and bend to reduce impact, but it cannot right itself in the air.

While alive and not standing, the robot continually attempts a recovery chosen
from measured torso orientation and available contacts:

- prone: brace on forearms or hands, draw a knee underneath, bring the pelvis
  over the base, plant a foot, and rise;
- supine: roll or sit using asymmetric arm and leg torque, then transition to a
  kneeling recovery;
- side: brace, rotate the chest toward prone or seated, then continue through
  the corresponding recovery.

Recovery phases advance only after required contacts and joint configurations
are achieved. A blocked or failed attempt times out into a different strategy
or retries; it never snaps upright. Damage removes available braces and steps.

There is no abstract health bar or hidden core. The Codex robot remains alive
while its connected pelvis-spine-chest control structure can actuate. Losing a
limb removes physical options. Breaking the central spine chain ends active
control and leaves a true ragdoll. An alive robot with insufficient limbs may
keep trying without succeeding.

## Rendering, reporting, and failure isolation

The red robot reuses the existing deformable mesh algorithm. Contact dents,
raised rims, recomputed normals, missing voxel cells, detached descendants, and
debris are retained independently for both figures. Remaining material updates
collision geometry, mass/inertia, and Codex actuator strength.

The on-page report names grey and red separately and includes Codex contact,
support, momentum, action, fall, recovery, damage, and landed-hit state. A Codex
initialization or update fault disables only the red robot where practical and
is reported without hiding the grey robot's existing report.

The voxel, mesh, body, and debris capacities are raised only as far as required
for two figures and are hard-capped. Rendering continues through the existing
shader and draw path.

## Deterministic test surface

Flat mode exposes:

- `window.render_game_to_text()`, returning concise state for the player and
  both figures, including coordinates, velocity/momentum, contacts, damage,
  current action, and recovery state;
- `window.advanceTime(ms)`, advancing fixed 72 Hz steps without depending on
  display timing.

The scripted flat preview contains observable standing, obstruction/collision,
fall, and recovery windows suitable for screenshots.

## Verification

Engine tests must demonstrate:

- all existing grey-figure tests remain green;
- Codex actuators create no net external force or torque;
- the red robot's weight is carried by its contacts;
- zero floor friction prevents powered translation;
- airborne motion is ballistic and has no righting torque;
- finite pushes can be recovered and larger pushes cause a genuine fall;
- prone, supine, and side get-up attempts use actual contacts;
- damaged or absent limbs remove corresponding recovery actions;
- a broken central spine disables active control;
- grey and red collide while retaining the player as their target;
- damage and breakage ownership is independent;
- the two-figure fixed step stays inside the measured CPU budget.

Page tests must demonstrate:

- the gesture creates one grey and one red figure;
- both render through deformable meshes with distinct colours;
- player fists collide with and damage either figure;
- grey/red collision produces physical response without retargeting;
- textual state matches the rendered state;
- no frame or console fault is introduced.

The final visual loop captures and inspects flat-preview screenshots of both
figures standing, colliding, falling, and the red robot attempting recovery.
Headset validation then answers the remaining feel question: does the red robot
feel like a heavy mechanism finding balance through its feet rather than a
puppet being held upright?
