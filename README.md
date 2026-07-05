Full GMC GASP support, with a new Motion Matching and Motion Warping solution, with native Unreal Engine 5 Manny support, initially set up by noon and further developed by noon and Dr Rank. 

GMCGASP                                                                                                                                                         
                                                                  
  A sample project built in Unreal Engine 5.8 showcasing the General Movement Component (GMC) v2 plugin ecosystem from GRIMTEC. It serves as a reference          
  implementation for building advanced character movement, traversal, and animation systems with proper, GMC powered, multiplayer replication.                                  
                                                                                                                                                                  
  Features                                                                                                                                                        
                                                                                                                                                                  
  Locomotion & Traversal                                                                                                                                          
  - Full locomotion system with walk, run, sprint, crouch and the full GASP traversal suite (slide may be added later)                                                                                    
  - Vaulting with height variations (low, medium, high)
  - Climbing with animated entries at different speeds
  - Mantling and ledge grabs
  - Hurdle traversal with height-aware montages
  - Motion Warping and PoseSearch trajectory prediction for smooth, responsive animation

  Ability System
  - GMC Ability System (GMAS) integration for managing abilities

  Included GMC Demo Plugins

  The project bundles several standalone demo plugins illustrating specific GMC features:

  - GMCBasedMovementDemo - Server-authoritative movement patterns
  - GMCBaseCharacters - Template characters for GMC (the pawn used for GMC motion matching, BP_GMC_Pawn, currently inherits from this but of course this can be easily changed)
  - GMCCatchThrowDemo - Replicated physics interactions
  - GMCCrouchProneDemo - Crouch and prone functionality
  - GMCCustomMovementComponentDemo - Custom movement components
  - GMCHitscanDemo - Server-side hitscan weapons
  - GMCLadderDemo - Ladder climbing mechanics
  - GMCMassMovementDemo - Large-scale NPC movement
  - GMCNavMovementDemo - AI pathfinding and navigation
  - GMCPushDemo - Server-side character pushing

  Key Dependencies

  ┌──────────────────────┬───────────────────────────────────────────────────┐
  │        Plugin        │                      Purpose                      │
  ├──────────────────────┼───────────────────────────────────────────────────┤
  │ GMC v2               │ Core movement component                           │
  ├──────────────────────┼───────────────────────────────────────────────────┤
  │ GMAS 1.3             │ Ability system built for GMC                      │
  ├──────────────────────┼───────────────────────────────────────────────────┤
  │ GMCMotion            │ Trajectory prediction and motion-warped traversal │
  ├──────────────────────┼───────────────────────────────────────────────────┤
  │ OnlineSubsystemSteam │ Steam platform support - added to local plugins   │
  └──────────────────────┴───────────────────────────────────────────────────┘
