# Codex Robot Stable Punch and Physical Recovery Design

## Problem

The red robot can topple while punching and, once down, can repeat the same floor-thrashing motion without ever reaching a standing state. The current automated checks miss both failures: the punch check samples only one endpoint, and the recovery check accepts a small pelvis bounce rather than a completed recovery.

Trace evidence shows the recovery reaches leg-extension phase while the pelvis and head are both near floor height. Because the feet remain loaded, that phase does not abort. The controller keeps extending a horizontal body, producing the visible “worm.” The attack controller also begins a 30 Hz one-arm strike without first confirming that both feet and the capture point form a stable base.

## Chosen approach

Use an orientation- and contact-driven get-up state machine, plus a pre-strike stability gate. This keeps the existing physics invariant: all motion comes from joint targets and equal-and-opposite internal torques reacting through real contacts. There are no root forces, gravity cancellation, position warps, or invisible supports.

The rejected shortcuts are weakening every punch until it cannot disturb the body, globally stiffening the ragdoll, or applying a corrective root torque. Those approaches would hide the failure or violate the purpose of the comparison robot.

## Punch stability

A punch may enter windup only when both feet carry meaningful load, the capture-point error is inside the support area, and the body is upright. Windup is cancelled if support is lost before the strike. During the strike, the non-punching arm and torso take a modest counter-pose so the equal reaction is carried toward the stance rather than rotating the whole body.

This does not guarantee the robot can never be knocked over. A player collision, damaged leg, bad footing, or blocked arm may still topple it. It prevents the controller from voluntarily throwing a full-power punch from an already unrecoverable base.

## Recovery state machine

Recovery uses measured body orientation and contact loads:

1. **Settle and classify** — allow the fall to settle, then classify the torso as prone, supine, or side-lying from its world orientation.
2. **Roll** — use asymmetric shoulder, abdomen, hip, and knee targets to roll prone. Alternate the roll direction after a timed failed attempt.
3. **Plant** — extend the arms toward the floor and tuck the knees. Advance only after real hand/forearm and knee or foot reactions are measured.
4. **Kneel** — push the chest clear while bringing the pelvis over the knee/hand support polygon. If the torso orientation or height does not improve within a timeout, return to roll with the opposite strategy.
5. **Crouch** — transfer load from hands/knees to both feet while keeping the centre of mass inside the foot support area.
6. **Stand** — extend the leg columns gradually, retain arm support until the torso is upright, and require a short quiet two-foot interval before returning to combat.

Each phase requires measurable progress in torso angle, pelvis/head height, or support transfer. A phase that has contact but makes no progress exits and changes strategy; it cannot remain in the same pose forever. Missing or broken required limbs cause a physically honest failed attempt and retry using remaining support, not a forced stand.

## Testing

The engine regression will exercise complete sequences rather than snapshots:

- Over repeated punches, every strike must begin from an upright, two-foot stable base; an intentionally unstable robot must withhold or cancel the punch.
- After an intact robot is knocked down in multiple directions, at least one deterministic scenario must return to ordinary fighting state with pelvis/head height and quiet foot support held for a meaningful interval.
- No recovery phase may remain unchanged without measurable progress beyond its timeout.
- Root force and root torque must remain exactly zero throughout recovery.
- Existing collision, damage, walking, attack reach, zero-friction, and page tests remain green.

The flat page will then be run through the standard scripted pilot, its state output and console checked, and gameplay screenshots visually inspected before deployment.
