# GMCMotion

GMCMotion provides trajectory prediction and deterministic motion-warped
traversal for GMC pawns.

## Standalone installation

Get GMCMotion from the [standalone repository](https://github.com/Dr-Rank/GMCMotion).
You do not need to download or use the GMCLocomotion example project.

For a project installation, place the plugin in `Plugins/GMCMotion`, with
`GMCMotion.uplugin` directly inside that folder. Install the required plugins
listed below, enable GMCMotion, then rebuild the project. GMCMotion provides
movement and animation support code; your pawn and animation assets still need
configuration for your project.

## Required plugins

Install and enable these plugins in the project where you will use GMCMotion:

- **GMC core**: your separately licensed General Movement Component installation.
- **GMCAbilitySystem (GMAS)**: obtain the latest files from the
  [Dr-Rank clear-fixes branch](https://github.com/Dr-Rank/DeepWorlds_GMCAbilitySystem/tree/clear-fixes).
  For a project installation, put the repository contents inside
  `Plugins/GMCAbilitySystem`, so that `GMCAbilitySystem.uplugin` is directly inside
  that folder. Enable the plugin and rebuild the project with a compatible C++ toolchain.
- **PoseSearch, MotionWarping, AnimationWarping and PhysicsControl**: Unreal Engine
  plugins declared as dependencies by GMCMotion.

GMCMotion requires GMAS to be installed and enabled because its animation class
uses GMAS types and gameplay-tag bindings. You do not have to implement gameplay
abilities or adopt GMAS as your game's ability system. Animation properties that
would normally be driven by GMAS tags still need appropriate values when those
bindings are not used; installing the plugin alone does not provide those values.

## Modules and features

- **GMCMotion**: `UGMCMotion` (traversal warp, stance collision, ground smoothing,
  slope slide, ragdoll capsule-follow mode, camera yaw limit), `UGMCMotion_AnimInstance`,
  `UGMC_TrajectoryPredictor`.
- **GMCMotionRagdoll**: `UGMCMotionRagdollComponent`, a server-authoritative active
  ragdoll (PhysicsControl) streamed to clients, with motion-matched get-ups, knockdowns
  and character interactions.

**Slope slide.** By default the slide starts by itself on floors steeper than
`SlopeSlideStartAngle`. Turn `bSlopeSlideAutoStart` off for a requested slide: call
`SetSlopeSlideRequested`, or name a GMC-bound input bool in `SlopeSlideRequestProperty`
(for example a crouch input), and the slide starts while requested on a floor of at least
`SlopeSlideTriggerMinAngle` moving downhill at `SlopeSlideTriggerMinSpeed`, and ends on
release (`bSlopeSlideEndOnRelease`).

**Camera yaw limit.** `SlopeSlideCameraYawRange` and `KnockdownCameraYawRange` (0 = off)
limit the local player's view yaw around the pawn's facing while sliding or out of control,
optionally only while a bool on the pawn is true (`CameraYawLimitConditionProperty`, e.g.
a first-person flag).

**Get-up view.** When a get-up starts, the local player's view turns to the direction the
body gets up facing (`GetUpViewAlignTime`, 0.35 s), in any view. Otherwise a pawn that faces
its view snaps round to wherever the camera pointed the moment control comes back.

**First-person ragdoll.** `bFirstPersonRagdoll` on `UGMCMotionRagdollComponent` keeps a
first-person pawn on its head camera while down and getting up, instead of switching to the
third-person ragdoll camera. Console: `gmc.Ragdoll.FirstPerson [0|1]`; debug
`gmc.Ragdoll.Knockdown`, `GetUp`, `Report`, `Trace`.

**Version.** `Version` in `GMCMotion.uplugin`, and the `GMCMOTION_VERSION` define, is 2
from this release (requested slide, camera limit, GMCMotionRagdoll).

GMCLocomotion is an example project, not an installation requirement for GMCMotion.
The latest upstream dev branch changes over time. Each supported integration
release should identify the GMC and GMAS revisions used in its qualification;
installation of a newer revision does not establish compatibility by itself.

## Licence

GMCMotion's source code is available under the [MIT License](LICENSE).
You may use it freely in your own projects, including commercial projects,
and modify, redistribute or sell it. Keep the copyright and licence notice
with copies or substantial portions of the code. No royalties are required
under this licence.

This licence applies to GMCMotion only. It does not grant a licence to GMC,
GMCAbilitySystem, Unreal Engine, or any other third-party dependency or asset.
Those remain subject to their respective licences.
