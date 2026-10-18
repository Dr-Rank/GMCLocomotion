# GASP 5.8 locomotion update

This branch brings the GMC pawn to Game Animation Sample (GASP) 5.8 parity and adds a few systems built on top of it.
Everything stays GMC-replicated (prediction, replays, simulated proxies).

## What's new

**Locomotion (GASP 5.8)**
- State machine path driven by GASP 5.8's `CHT_MoverCharacterAnimations_PoseMatch` chooser (pose search column inside
  the chooser, one `StateName` per state). The old `MotionMatch` + `PSD_SM_*` path is removed.
- 5.8 animation data: Relaxed crouch set (starts, stops, pivots, spins, turns), per-foot jumps, idle break v03, 5.8
  pose search notify windows on the shared animations.
- Slopes: slope blend spaces picked by the chooser, `SlopeAngle` and blend space inputs, GASP 5.8 foot placement
  Control Rig (`CR_Biped_FootPlacement`).
- Moving platforms: based-movement delta fed to Offset Root Bone and to the foot placement rig
  (`UGMCMotion_AnimInstance::OnUpdateOffsetRootBone`). Test actor: `/Game/GMC/Tests/BP_Parity_MovingPlatform`.
- Crouched foley (`AM_FootSteps_Crouch`, crouch foley notifies) and a spatialized `AC_FoleyEvents` on the pawn.
- Ground velocity smoothing like Mover's `SmoothWalkingMode` (`bSmoothGroundVelocity`, acceleration / deceleration
  smoothing times, `GroundTurningStrength`), braking above max speed at the gait deceleration, GASP 5.8 gait speeds.

**Trajectory prediction**
- `UGMC_TrajectoryPredictor` replays the GMC move model (and `BP_GMCMovement` velocity adjustments) for the future
  trajectory, with the real angular velocity and the mesh position.

**Traversal**
- GASP 5.8 traversal chooser rows and start-time adjustment, anim-offset warping (hurdles and catches included), exit
  speeds, feet-float fix, smoother traversal entry / warp window exit, no network foot mismatch.
- Air traversal catches (GASP 5.8): grabbing ledges while falling.

**Slope slide** (GMC movement mode `Custom2`)
- **This project: crouch while sprinting** on a downhill floor of 20 deg or more starts it; releasing crouch (or the slide
  running out) ends it. Comes from `BP_GMCMovement`'s defaults: `bSlopeSlideAutoStart` off and
  `SlopeSlideRequestProperty = WantsToCrouch`, so the GMC-bound crouch input is the request, read inside the move.
- The slide keeps the speed the pawn ran in with (it used to lose a factor of cos(slope) on the first frame:
  375 -> 239 cm/s horizontal on a 35 deg slope).
- First person: the camera yaw is limited to +-90 deg of the facing while sliding, and held on the facing at the
  moment control was lost while knocked down or getting up (`SlopeSlideCameraYawRange`, `KnockdownCameraYawRange`,
  gated on the pawn's `bFirstPersonView`). Third person is unaffected.
- GMCMotion's own default is unchanged for other projects: with `bSlopeSlideAutoStart` on, the slide is involuntary on
  floors steeper than 35 deg (stops under 28 deg, run-out).
- Lateral steering, crouch-height capsule while sliding, slide pose on every client
  (`UGMCMotion::UpdateSimulatedProxyFloor`).

**Ragdoll, knockdown, character interactions, death** (GASP 5.8 port, `UGMCMotionRagdollComponent` in GMCMotion's
`GMCMotionRagdoll` module)
- Server-authoritative active ragdoll streamed to clients, motion-matched get-ups (stand / roll).
- `B` toggles the ragdoll, `Space` gets up. `,` shove / tackle, `.` takedown (two-character interactions).
- First person switches to the third-person ragdoll camera while down by default. `bFirstPersonRagdoll` on the ragdoll
  component (console `gmc.Ragdoll.FirstPerson [0|1]`) keeps first person on the head instead; the view is held on the
  facing at the knockdown, then turned to the direction the body gets up facing (`GetUpViewAlignTime`, 0.35 s).
- Debug (server world): `gmc.Ragdoll.Knockdown [self|others|all] [Impulse]`, `gmc.Ragdoll.GetUp [who] [Delay]`,
  `gmc.Ragdoll.Report [Delay]`, `gmc.Ragdoll.Trace [Seconds]` (per-frame phase, view, camera window).
- Knockdown and hit reactions from impacts: call `AddHitImpulse` from your damage code.
- GMAS death: `Health <= 0` -> `GE_Dead` (`State.Dead`) -> dead ragdoll, respawn after `RespawnDelay`.

**Player smart objects** (`UGMCGASPSmartObjectUser`)
- `E` near the bench (`SOTEST_Bench` in GMCMM) sits down, `E` or a move input stands up. Server-authoritative slot claim,
  montage path followed through GMC.

**Camera**
- Third-person shoulder camera rig (`/Game/GMC/Camera/BP_ShoulderCameraRig`): `F` toggles first / third person, `Tab`
  switches shoulder, `Alt` free look, mouse wheel zoom.

## Controls (QWERTY)

`WASD` move, `Shift` sprint, `C` walk, `Left Ctrl` crouch (slide while sprinting downhill), `Space` jump / traverse, `V` strafe, `B` ragdoll, `E` bench,
`,` shove, `.` takedown, `F` view, `Tab` shoulder, `Alt` free look.

## Code

- `Plugins/GMCMotion`: `UGMCMotion` (stance collision, slope slide, ground smoothing, traversal warp, ragdoll mode,
  camera yaw limit), `UGMCMotion_AnimInstance`, `UGMC_TrajectoryPredictor`; module `GMCMotionRagdoll`:
  `UGMCMotionRagdollComponent` (+ triggers). Specs: `GMCMotion.Unit`.
- `Source/GMCGASP`: `Interaction/GMCGASPSmartObjectUser` (+ bench
  interactable), `Tools/GMCGASPAssetTools` (editor helpers: property text import / export, reference replacement).
- Config: `Obstacle` object channel + `ObstaclePreset` profile (ragdoll obstacles), ragdoll / foley / locomotion tags.
- New engine plugin dependency: `PhysicsControl` (declared by GMCMotion).
- The ragdoll classes were renamed from `GMCGASP*` (game module) to `GMCMotion*` (plugin); `[CoreRedirects]` in
  `Config/DefaultEngine.ini` keeps existing assets loading.

## MCP

Unreal's MCP server starts with the editor (`http://localhost:8000/mcp`, `bAutoStartServer` in
`Config/DefaultEditorPerProjectUserSettings.ini`; plugins ModelContextProtocol, MCPClientToolset, AllToolsets).
`.mcp.json` points an MCP client opened in this folder at it.

## Test map

`/Game/Levels/GMCMM`: traversal blocks, slopes, the bench, moving platform test actor.
