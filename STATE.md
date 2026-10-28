# Volcano Client State
This document describes the current project state of Volcano Client. Bugs and feature descriptions are listed here, as well as testing sessions.

## Milestones
1. **Hello Triangle** — done.
2. **Basic survival experience** — a mostly complete survival game. Requires: inventory functionality, players, item/falling block entity models, and more humanoid/quadruped entity models.
3. **Working PvP experience.**
4. **A client people can use daily.**

## Roadmap
Remaining work, roughly in milestone order. **(M2)** marks what milestone 2 needs.
* [ ] Item and falling block entity models **(M2)**
* [ ] More humanoid and quadruped entity models **(M2)**: bats, fish, phantoms, the Nether and End mobs, bosses
* [ ] Absorption hearts and the status effect HUD (icons, plus effect-driven FOV and screen changes)
* [ ] Flowing fluid animation (flow direction textures)
* [ ] Elytra (rendering and flight)
* [ ] Chunk fade-in
* [ ] Fix combat (knockback direction, see PvP notes)
* [ ] Weather, biome tints and the Nether/End dimension visuals
* [ ] Dyed leather, armor trims and enchant glint
* [ ] TLS support with Mojang accounts
* [ ] Automatic singleplayer server generator

## Current State
Completion: **about 60% Complete.** (Estimate; milestone 2 is mostly entity coverage, weather and polish away.)

#### Implemented
* GUI framework (ImGui-based, themed)
* Basic renderer (Vulkan; terrain + entity + misc passes; texture atlas from minecraft-data)
* Render engine abstraction layer (`Engine::RenderEngine`, ordered `RenderPass` list, optional runtime shader packs with F9 reload) — see `docs/RenderingEngineRework.md`. **Not yet built/run on Windows.**
* Movement (fixed-tick simulation, AABB collision with auto step-up, sprint/sneak as client-side speed multipliers)
* Network client (login/configuration/play, compression, chunk streaming, chat/commands, death/respawn)
* Entity spawning/tracking (placeholder box + a real humanoid model; no other players' skins yet)
* Other players as full `PlayerEntity`s (uuid, name, game mode, equipment from Set Equipment, health/sneak/sprint/invisible from Set Entity Data), rendered like other humanoids with a red hurt/death flash
* PvP / melee combat: click-to-attack (Attack + Swing packets), vanilla's four hit types (normal, sprint-knockback, critical, sweep) predicted client-side, sprint-hit self-slowdown, and incoming knockback (Set Entity Motion / Explosion) applied to the local player
* Movement-state reporting: real sprint state (Player Command), held keys incl. sneak (Player Input), rotation in every position report, local attributes from Update Attributes
* Player-status HUD (health/hunger/saturation bars, hotbar + armor slot placeholders, selected-slot indicator, chat)
* Config system (settings.json, dot-path keys, self-defaulting)
* Interaction manager skeleton (mainhand/offhand tracking, attack cooldown bookkeeping, primary/secondary mouse actions wired — no ray tracer yet, so nothing actually dispatches an attack/block-break/use/place)
* Unloading chunks
* Per-block lighting data from the server
* Inventory UI and packet handling
* Block selection
* Tab list
* Block breaking/placing with vanilla hold-to-repeat pacing
* Settings screens (Video, Window, Controls), writing straight to settings.json and applied live
* Window modes: windowed, borderless, exclusive fullscreen (VK_EXT_full_screen_exclusive when the driver has it)
* Hi-Z occlusion culling of chunk sections (GPU depth pyramid, CPU test a couple of frames behind)
* Buffered raw mouse input (Windows `GetRawInputBuffer`; GLFW raw motion on Linux)
* Crash reporter: dialog + `crash-report.txt` with a symbolized stack trace
* Platform layer (`src/platform/OS*`) for Windows and Linux
* Container screens (crafting, furnace, chests and the rest), inventory and tooltips (colors, food, durability, attack stats)
* Fluids: water and lava surfaces with corner-averaged heights (flowing animation still missing)
* The formerly excluded multipart blocks (fences, walls, panes, doors, trapdoors, beds, redstone, vines)
* Block entities (chests, signs, banners, skulls, beds, shulker boxes and more)
* Player physics: water, lava, ladders, vines, cobwebs, honey, slime, swimming and crawling, riding (everything but elytra)
* HUD: real heart, hunger and armor icons, XP bar, air bubbles, damage flashing, boss bars, title and action bar, tab list, chat formatting
* Server-sent sounds (positional, with mob, hurt, death and ambient sounds) and block/step sounds; disconnect and kick reasons shown in a screen
* Nametags (capped and LOD'd), and horse and bee models next to the ~20 data-driven mobs in `mobs.json`
* Paper servers: movement and packet checks pass (position reporting, attacks and placements all accepted)
* Shadow mapping (cascaded, received by terrain and by textured entities)
* Hand model: sway, swing, attack-charge height (held for 85% of the swing), eating, swimming, left-handed mode, view bobbing, and the server's light on the arm and held item
* Third-person camera with the local player model; armor and held items on other players, zombies, husks and skeletons
* Entity lighting: textured mobs take the server's sky and block light at their position through the terrain's curve
* Particles: `ParticleSystem` and `ParticlePass` (Level Particles packet, crits, 28 profiled types), sized independent of pack resolution
* Chat: restyled to the design, command syntax highlighting, Tab completion and a tooltip from the server's command tree (`Commands` packet) and `resources/commands.json`, history on Up/Down, Shift+Enter queueing
* First-launch onboarding (graphics, audio, keybinds) in `src/Onboarding.hpp`

#### Not implemented:
* Music, jukebox records and note blocks
* Smooth lighting/SSAO (LATE)
* Resource pack reloading from server-pushed packs (they are currently ignored entirely)
* Mod loading (LATE): loader, versioned C API, events, packet/sound listeners, HUD elements and example mod (sdk/) exist; client commands, persisted keybindings, mod settings entries and item-behavior registry do not
* Blocking with shields, bows/tridents
* Combat visuals: hurt camera tilt, hit particles (crits are in)
* Dynamic motion sampled animations
* Ctrl+Enter to send a command as plain chat; server-side command suggestions (ask_server parsers)
* Block-break and ambient particles
* Elytra, absorption hearts, status effect HUD

## Bugs
* When falling, your velocity will reset about 70% of the way down for no reason.
  * **Lead, unconfirmed:** `TickLoop::FixedStep` zeroes velocity unconditionally on every server position correction (`TickLoop.cpp`, the `pendingTeleport` branch). The server corrects most often mid-fall, which would produce exactly this symptom. Worth checking before looking elsewhere — fix would be to only zero velocity when the correction is a real teleport, not a routine resync.
* Chunks have faces in between each other. **Fix implemented (untested):** MeshingThread now queues the 8 loaded neighbors' non-empty sections after InsertChunk (deduped); World::SetBlockImpl also dirties the diagonal chunk. **Root cause confirmed:** `MeshingThread` never remeshes an already-loaded chunk's neighbors when a new chunk arrives next to them. A chunk meshed before its neighbor exists asks `World` for that neighbor, gets "no chunk there" (read as air), and permanently bakes in a face at that boundary — even after the real neighbor streams in a moment later and (correctly) doesn't draw its own matching face. Real fix needs two things this client doesn't have yet: a way to find/replace a specific chunk's existing mesh in `renderList`/`nonCubicRenderList` (currently append-only) and a way to free its old slab allocation when replaced (`SlabBuffer` is bump-allocate-only, no reuse story). Same underlying gap as "Fix the greedy meshing system" below and "Unloading chunks" in Not Implemented — worth tackling together.
* Splash screen is fitted to the window instead of cropped to fit.
* Stairs render backwards on the X axis; corner stairs render backwards on both X and Z. Reported live, not yet investigated — likely `NonCubicMesher`'s stair-rotation logic (the same code path the "chunks have faces in between each other" and depth-write fixes already touched this session).
* ~~Player entity skins render as visual noise / corrupted pixel data~~ — **retracted, was a misdiagnosis.** The user correctly identified the real bug: the face texture was just UV-mapped wrong (see Fixed below — `HumanoidModel.hpp`'s per-face UV corners), not corrupted at rest. Last session's "confirmed via two independent decoders" claim was itself the error — both `steve.png`/`zombie.png` are 8-bit indexed/palette PNGs, and whatever ad-hoc preview was used for that check most likely read the raw index bytes without applying the PLTE palette, which produces exactly this kind of noise for a perfectly valid image. Left struck through rather than deleted so the wrong turn stays on record.

## Fixed since last update (2026-09-15)
* **Spawn Entity packets failed to parse 100% of the time** (49/49 in a 20s session — no entity ever spawned client-side). `lpVec3` (the velocity field) turned out to be variable-width, not the fixed 3×i16 the code assumed: 1 byte for a stationary entity, 6 for a moving one. The parser now resolves the width per packet against where the tail actually ends, rather than guessing. Verified live: 0 failures across a full session afterward. -- *`NetworkClient.cpp`*
* Cross-thread chat/command/respawn sends could race the network thread's own socket cancel (`WaitForReadable`) and corrupt the packet stream with a partial write. `Connection` now takes an outbound queue; only the network thread ever touches the socket. -- *`Connection.hpp`, `Connection.cpp`, `NetworkClient.cpp`*
* `GlobalState::activeConnection` held a raw pointer into a stack-local `NetworkClient`; another thread could load it, get descheduled, and use it after it was destroyed during shutdown. Now an aliasing `shared_ptr` that keeps the client alive for as long as any thread holds a copy. -- *`GlobalState.hpp`, `NetworkThread.cpp`*
* Horizontal wall collisions froze the player short of the actual contact surface instead of snapping flush to it (same class of bug as the vertical landing-height fix, just with a smaller, walk-speed-bounded gap). Added the horizontal analogue of `FindGroundContactHeight`. **Not yet verified in the real client** — needs a live "moved wrongly" check walking into terrain. -- *`TickLoop.cpp`, `TickLoop.hpp`*
* Vulkan device selection can now honor a `Graphics.PreferredDevice` name from settings.json (falls back to the default pick with a logged warning if nothing matches). -- *`VulkanInit.cpp`*
* Humanoid model's "down" face UV now matches vanilla's vertical flip (visible on the underside of the head/feet). -- *`HumanoidModel.hpp`*
* Crosshair renderer added — draws `resources/crosshair.png` centered on screen every HUD frame, as a standalone Vulkan texture outside TextureManager's block/item atlas (own image/view, no mip chain, loaded once in `GUIController::Init`). Added `GlobalState::lookingAtEntity` as the tracking field this task asked for, so a future raycast can swap in `resources/crosshair-entity.png` — nothing sets it yet, so only the plain crosshair shows for now. Verified live: renders crisp and exactly centered against the dev server. -- *`GUIController.hpp/.cpp`, `GlobalState.hpp`*
* Hotbar slot switching — number keys "1".."9" now select hotbar slots 0-8 (`Hotbar.0`..`Hotbar.8` actions, gated the same as other HUD-affecting keys so typing in chat doesn't also switch slots), updating `InventoryManager::selectedHotbarSlot` locally and reporting it to the server via a new serverbound `PlayC2S::SetHeldItem` (0x35) packet. Verified live: pressing a number key moves the held-slot highlight to the correct slot and only that slot. -- *`VolcanoClient.cpp`, `RenderThread.cpp`, `NetworkClient.hpp/.cpp`, `PacketIds.hpp`, `VarInt.hpp`*

## Fixed since last update (2026-09-16)
* **Side textures were upside-down** in both mesh passes. Texture V runs down the image (V=0 is the top row), but both meshers fed it a coordinate that runs *up* the world on vertical faces, so the bottom of every wall sampled the top of its texture. Flipped V for side faces in `ChunkMesher` (terrain) and for side faces plus cross billboards in `NonCubicMesher` (slabs, stairs, plants — flowers were upside-down too). Up/Down faces are unchanged. **Not yet checked in-game** against a grass block side. -- *`ChunkMesher.cpp`, `NonCubicMesher.cpp`*
* **Stairs and slabs rendered inside-out, with flicker and apparent duplicate faces.** The non-cubic pass had depth *writes* off, on the idea that it's the transparent pass. But stairs, slabs, carpets and snow layers are opaque, and `misc.frag` already discards transparent texels, so it's really a cutout pass. With writes off and backface culling off (the cross billboards need that), a shape's own back and underside faces painted over its front faces whenever they were emitted later. Depth writes are now on, so the depth test sorts this out per fragment. The face-culling logic itself (`IsFaceFlush` against opaque neighbours) and the stair rotation code checked out as correct. Trade-off: truly translucent texels (stained glass) now write depth too, which will need its own pass once those render. **Not yet checked in-game** against actual stairs/slabs. -- *`VulkanInit.cpp`, `RenderThread.cpp` (comment only)*
* **Rubber-banding when standing on certain blocks.** Collision was built from each block's render model, so any block with no drawable geometry (shulker boxes, chests, beds, signs, skulls; also fences/walls, whose multipart blockstates aren't resolved) had no collision client-side. The player sank into them and the server teleported them back every tick — 722 corrections in one session standing on the red shulker box at spawn (0, 82, 0). Collision now comes from minecraft-data's `blockCollisionShapes.json` (real per-state shapes, up to 15 boxes, up to 1.5 tall), and the collision scans look one cell lower for the tall ones. Verified live: the next session had 7 corrections, spread across different spots rather than all at spawn (the first two were lag). Block-entity blocks are still invisible — rendering them is separate work. -- *`BlockRegistry.hpp/.cpp`, `Block.hpp`, `ChunkParser.cpp`, `TickLoop.cpp`*
* **Terrain lit flat at light level 15 everywhere, even under tree canopy.** The client already parsed real per-block sky/block light from the server, but shaded with a straight `rawLevel/15` multiply — under vanilla's real (non-linear) response curve, light levels 11-15 all read as nearly full brightness, which is exactly what made shaded ground look the same as open sky. Both fragment shaders now apply vanilla's own curve (`level/(4-3*level)`, from `Lightmap.getBrightness`), remapped to a floor of 0.05 (half of the Nether's 0.1 ambientLight constant, per this task's own spec — the overworld's *own* floor is 0 either way, so this only matters as a stylistic "never quite crushed black" floor) and the normal ceiling of 1.0. Sky-only light is additionally capped at 0.85 so a real block-light source (torch, lava, glowstone) can still reach the full ceiling and read as visibly glowing against its sky-lit surroundings — a flat stand-in for real bloom. This needed sky and block light kept as two separate channels all the way to the shader instead of pre-merged (`Chunk` now stores two arrays; `MaskCell`/`MiscVertex::Pack` carry both). **Verified only that it builds and runs without visual regression in a lit courtyard** — not yet checked against actual under-canopy shade or a real light source, since that spawn didn't have either. -- *`Chunk.hpp`, `World.hpp`, `ChunkMesher.cpp`, `NonCubicMesher.cpp`, `MiscVertex.hpp`, `terrain.vert/.frag`, `misc.vert/.frag`*
* **Humanoid faces (head, body, arms, legs) were UV-rotated on 3 of 6 box faces (down/north/east)**, most visibly on the head — Steve's front-face texture (eyes/nose/mouth) rendered transposed instead of upright. Root cause: `HumanoidModel::BuildMesh` paired each face's 4 UV corners with its 4 geometric corners the same way for every face (`(u0,v0),(u1,v0),(u1,v1),(u0,v1)`), but that pairing is only correct when a face's local tangent axes (`FaceDef::u`/`v`) happen to align with the same world axes the texture-rect math (`FaceUVRect`) assumes — true for up/west/south, false for down/north/east (verified directly against vanilla's real `ModelPart.Cube`/`Polygon` vertex-to-UV remap in the bundled `resources/minecraft-code`, not guessed). Swapping `FaceDef::u`/`v` themselves to fix it was not an option — it flips the sign of `u×v`, handing those faces backface-culling-breaking winding on any box where width≠depth. Added `HumanoidModel::detail::FaceUVCorners`, which gives each face its own correct corner pairing (a mirror for up/west/south, a transpose for down/north/east) instead of the one-size-fits-all pattern. Also separately: the humanoid mesh's local "front" turned out to sit on local -Z (matches vanilla's own vertex order), but this engine's own yaw convention (`TickLoop`'s `forward{ sin(yaw), 0, cos(yaw) }`, proven correct for the local player's own movement) treats +Z as forward — so a remote player/zombie's whole body was yaw-rotated 90°-ish off from where it should face for a given position, which is almost certainly the "Steve is to the right of the podium instead of in front of it, like it was rotated around a pivot" report. Fixed by turning the mesh 180° around the vertical axis inside `BuildMesh` itself, rather than touching the yaw-rotation matrix both `entity.vert` and `entity_textured.vert` share (that matrix already matches `TickLoop`'s proven-correct convention for a +Z-forward mesh; touching it would've meant also flagging the placeholder-cube shader, where a sign error is currently invisible only because untextured cubes have no facing to look wrong). **UV-corner fix is verified against vanilla's own source, high confidence. The 180° facing fix is the best analysis available without a live entity to rotate in front of the camera — please check Steve/a zombie's face lines up with their walking direction.** -- *`HumanoidModel.hpp`*
* **`Helpers.hpp` didn't compile** (missing semicolons after `Quat3`/`Quat4`, non-static `constexpr` members on the `Matrix3i`/`3f`/`4i`/`4f` structs) — only surfaced now because nothing had included the file since these structs were added to it until `InteractionManager.hpp` did. -- *`Helpers.hpp`*
* **Step-up applied while airborne, not just on the ground.** `MoveAxis` tried `TryStepUp` on any blocked horizontal move regardless of `grounded`, so jumping/falling into a solid face let the player climb it a `STEP_HEIGHT` (0.6-block) hop at a time instead of colliding with it like a wall. Now gated on `grounded`. -- *`TickLoop.cpp`*
* **Ghost chunk geometry after a dimension change** (e.g. dying and respawning back at the world spawn) — the old dimension's chunks, entities, and both render lists just kept rendering forever, since nothing ever cleared them. The server's `Respawn` packet (sent on every dimension change, not only death) was previously unhandled entirely. The disconnect-screen reset (world/render-list/entity clear, already existed, only ever ran when returning to the connect screen) is now a shared `GlobalState::ResetWorldState()`, called on `Respawn` too — same reset, but without reopening the connect screen, so the session stays connected while the new dimension's chunks stream in. **Not yet verified against a live respawn/dimension change** — only that it builds and the reset is a safe no-op mid-connect-screen-return (its original call site). -- *`GlobalState.hpp`, `VolcanoClient.cpp`, `NetworkClient.cpp`, `PacketIds.hpp`*

## Fixed since last update (2026-10-27)
* **Mobs vanished after being hit or landing.** Position sync, teleport, vehicle move and explosion packets built a vector from three `ReadDouble()` calls in one argument list. C++ leaves that evaluation order unspecified and clang in MSVC mode runs it right to left, so x and z were swapped and the mob landed about 4,200 blocks away. A `ReadVec3` helper reads them as separate statements at all six sites. Probably also the cause of the old "entities inside walls" note. -- *`NetworkClient.cpp`*
* Teleport (0x7d) now parses the 26.x layout (position, velocity, yaw/pitch, relative-flags int, onGround). -- *`NetworkClient.cpp`*
* Particle quads are scaled by the pack's resolution (8px vanilla art is 1.0) and the atlas layers are 64px, so a 2x pack no longer draws double-size or drops its 64px explosion frames. -- *`ParticleSprites`, `ParticleSystem`*
* Hi-Z entity culling is off by default (`EntityOcclusionCulling`), and item meshes are no longer back-face culled so extruded sprites keep every face under mirrored display transforms (root cause of the hand-model face bug not confirmed). -- *`EntityRenderer.cpp`, `ItemRenderer.cpp`*
* Debug builds are optimized like Release and still carry debug symbols (`VOLCANO_DEBUG_CHECKS` turns the checks back on). -- *`CMakeLists.txt`*

## Fixed since last update (2026-09-28)
* **Remote entities faced mirrored east/west.** Minecraft's yaw turns the opposite way from this engine's (vanilla faces `(-sin, cos)`, the entity shaders/camera face `(sin, cos)`); `AngleByteToRadians` passed it through unnegated, so an entity facing east rendered facing west. North/south looked right, which is likely why the earlier live check passed. Now negated. **Needs a live check** with a player/zombie walking east or west. -- *`NetworkClient.cpp`*
* **Spawn Entity velocity decoding** replaced the "try 1 or 6 bytes" width heuristic with an exact port of vanilla's `LpVec3.read` (round-trip tested against a port of `LpVec3.write`). The heuristic couldn't handle velocities above ~1 block/tick (7+ bytes, continuation varint) and skipped those entities. -- *`NetworkClient.cpp`*
* **Holding left mouse reset the attack cooldown every frame**, so it could never charge. Attacks now happen on click only (`PrimaryActionPress`), like vanilla. -- *`InteractionManager.cpp`, `VolcanoClient.cpp`*
* `PlayerAttributes` is now mutex-guarded, since the network thread writes server attribute values into it while the render thread reads it. -- *`PlayerAttributes.hpp/.cpp`*

## Fixed since last update (2026-09-30)
* **Slabs filled up and non-opaque blocks vanished.** `SlabBuffer` was bump-allocate-only, so every remesh leaked its old region. It now has a free list; replaced/removed meshes hand their space back after a 3-frame delay. -- *`SlabBuffer.hpp/.cpp`, `MeshingThread.cpp`*
* **Chunks never unloaded.** The Unload Chunk packet (0x25) is now handled: the chunk leaves `World` and every render list. -- *`NetworkClient.cpp`, `MeshingThread.cpp`*
* **Crash on close:** `Cleanup` double-destroyed staging and slab buffers through a global buffer list. Callers of `CreateBuffer` now own their buffers. -- *`VulkanInit.cpp`*
* **ImGui assert when a frame was skipped** mid swapchain rebuild. -- *`RenderThread.cpp`, `GUIController.cpp`*
* **FPS stuck at ~63:** `sleep_for` rounds to Windows' 15.6 ms tick; frame pacing now uses a high-resolution waitable timer, and Max FPS applies live. -- *`RenderThread.cpp`, `OSWindows.cpp`*
* **Holding break/place cleared everything in reach:** repeat delay was 1 tick; now vanilla's 5 ticks (break) / 4 ticks (place), and a click no longer places twice. -- *`InteractionManager.cpp`*
* Terrain backface culling re-enabled (X-axis faces had flipped winding). -- *`ChunkMesher.cpp`, `VulkanInit.cpp`*
* Menu screens are vertically centered. -- *`GUIController.cpp`*

## PvP notes (2026-09-28)
Built against vanilla 26.2's own code in `resources/26.2.zip` (`Player.attack`, `MultiPlayerGameMode.attack`, `ClientPacketListener`, `LpVec3`). **None of it has been tested against a live server yet.** Things to check first:
* Sprint-hitting another player knocks them back further than a standing hit, and slows you down (you have to re-press sprint/W-tap for the next sprint hit to count).
* Being hit knocks you back. The first tick of knockback now moves at full strength: `TickLoop::FixedStep` applies friction after the move instead of before, as vanilla does. Walking speed is mathematically unchanged, but check that walk/sprint/jump still feel the same.
* Jump-and-fall hits land as crits (the server decides from our reported position/onGround, so this also exercises position reporting).
* Sword hits while standing still sweep.
* The server's own sprint modifier is filtered out of `generic.movement_speed` in Update Attributes because TickLoop applies the sprint multiplier itself. If sprinting feels too fast or slow, check this first.

## Tasks
[x] Contemplate my life choices.
[x] Add key actions for switching hotbar slots.
[x] Fix stair and slab rendering.
[x] Fix side textures being upside-down.
[x] Add the crosshair to the GUI and add relevant tracking fields, such as the attack indicator. Use resources/crosshair.png for the source image.
[x] Take a nap.
[x] Add the block overlay, using settings.json fields for styling and the collision model for the shape.
[ ] Fix the greedy meshing system.
[ ] Replace the implicit Vulkan backface culling with basic compute shader backface culling.
[x] Eat lunch.
[x] Take a shower.
[ ] Re-create the splash screen and fix the scaling.
[x] Create an `src/gui/screens` directory and move screen classes there.
[x] Create an SettingsScreen class in `gui/screens` that displays links to other (future for now) options screens.
[x] Create a GraphicsSettingsScreen class in `gui/screens` that allows you to control graphics settings found in settings.json.
[ ] Pick an icon font (FontAwesome or Material) and drop it in `resources/fonts/` so `GUIController::Init` can merge its glyphs into the atlas — blocks a real heart/hunger icon HUD instead of the current labeled bars.
[ ] Verify the new horizontal wall-collision snap (`TickLoop::FindWallContactCoordinate`) feels right in the real client — walk into a wall and watch for a "moved wrongly" kick, same test the old TODO asked for.
[x] Commit `STATE.md` to git — it's currently untracked, so there's no history of it and it's one `git clean` away from gone.
[x] Create `InteractionManager` (`src/interaction/`) — mainhand/offhand tracking (snapshotted from `GlobalState::inventory` each frame, not held as raw pointers into it — see its own comment on why), attack-cooldown bookkeeping, `PrimaryAction`/`SecondaryAction` mouse-button input actions (added mouse-button support to `InputHandler` itself, which had none before — see `MOUSE_BUTTON_LEFT`/`RIGHT`). `RayTraceSelection()` is a deliberate stub — that's the next task below — so `PrimaryTrigger`/`SecondaryTrigger` currently only manage the cooldown gate, nothing dispatches an attack/break/use/place yet.
[x] Build the ray tracer: a block/entity raycast that resolves the camera's look ray against `state->world`/`state->entities` out to `InteractionManager::blockReachDistance`/`reachDistance`. Wire it into `InteractionManager::RayTraceSelection()` (for `PrimaryTrigger`/`SecondaryTrigger` to dispatch against, and the selection-outline renderer to draw) and into `GlobalState::lookingAtEntity` so the crosshair's attack-indicator variant actually swaps in on target.
[x] Once the ray tracer exists: wire `InteractionManager::PrimaryTrigger`/`SecondaryTrigger` to actually attack the targeted entity / start breaking the targeted block, and use the held item / place a block against the targeted face, respectively.
[ ] Verify PvP live against the dev server — see "PvP notes" above.
[ ] Verify the east/west entity-facing fix live (see Fixed, 2026-09-28).
[ ] Fix chunk-boundary faces for real: give `renderList`/`nonCubicRenderList` a find-and-replace path (not just append) and `SlabBuffer` a way to free/reuse a chunk's old allocation, then have `MeshingThread` remesh a chunk's already-loaded neighbors whenever a new chunk lands next to them.
[x] Fix the humanoid face-texture rotation (was a UV-corner-pairing bug, not a corrupted skin file — see Fixed).
[x] Verify live that a remote player/zombie's face now points the same way they're walking (the 180°-facing fix in the same change above couldn't be checked without a live entity).
[x] Verify the dimension-change reset (`GlobalState::ResetWorldState` on `PlayS2C::Respawn`) against a real respawn/dimension change — confirm the old world's geometry is actually gone and the new one streams in cleanly.
## Survival playtest roadmap (2026-10-15)
Ordered by how much each hurts play. Do one or two at a time.
[x] 1. Bucket water/lava (and boats, thrown items): send UseItem after the block click, as vanilla does when useOn passes. Untested live.
[x] 2. Mining: another face of the same block no longer restarts the break timer. Untested live.
[x] 3. Block break, place and in-progress (hit) sounds: BlockSounds (src/audio/). Untested live.
[x] 4. Step sounds: every 8 ticks walking (was 10), 6 sprinting. Untested live.
[x] 5. (linked; verified by the user: linked, text untested) GUI text: 18px like vanilla (currently 2px too small); no white-on-white shadow problem on light GUIs - draw black text on white panels, white on dark or transparent ones.
[x] 6. (untested live) Heart and hunger icons from the resource pack; saturation shown as an outline recolor, like AppleSkin.
[x] 7. (untested live; custom item names and enchantments not shown yet, the slot skipper discards them) Inventory tooltips: color support, food hunger/saturation, equipment durability, weapon/tool attack cooldown and charged damage.
[x] 8. (linked; untested) Animation helper library: src/renderer/gui/Animation.hpp (Animated, AnimationSet, Transform2D, AddImageTransformed); the hotbar selection box uses it. Animation helper library (transform of images: hotbar selection, item hover and swap, GUI open/close). Build before 9 so the hand can use it.
[x] 9. (linked; untested live) Hand model: look-angle sway (lag halves per tick, tilt 0.1x) and sub-tick attack charge. Hand model interpolation by attack speed, plus a global lerp from look-angle deltas.
## Playtest fixes (2026-10-16, all untested live)
[x] Slot parsing: SlotSkipper (src/network) skips every data component using protocol.json, so a damaged tool no longer drops the rest of a container (the vanished hotbar). ItemStack now has damage/maxDamage.
[x] Set Player Inventory (0x6C) handled: bucket fill/empty and pickups now update the inventory.
[x] Sync Entity Position (0x23) handled; dropped items fall client-side between server updates (TickLoop::StepDroppedItems).

## Still needed for a standard survival experience (updated 2026-10-27)
What was listed here on 2026-10-16 has mostly landed (see Current State). What is left:

### Entities
[ ] Models and textures for mobs that still draw as boxes: bats, fish, phantoms, wandering traders, the Nether and End mobs, bosses.
[ ] Baby variants, sheep dye colors, spider and enderman eyes, villager professions, the slime outer cube.
[ ] Entity animation beyond walk, head look and swing: bow draw, creeper swell, death and damage tilt.
[ ] Object entities: arrow and other projectile models, item frames, armor stands, paintings, minecarts, end crystals, leashes, fishing bobbers.
[ ] Dropped items: the vanilla bob and spin, the pickup fly-to-player animation (Collect packet).
[ ] Entity metadata beyond a few fields: on fire, glowing, sleeping.
[ ] Armor: dyed leather (dye is a data component the client doesn't read), trims, enchant glint, elytra, baby/horse armor.

### World rendering
[ ] Flowing fluid textures and submerged fog/tint.
[ ] Block-break crack overlay; selection outline polish for non-cubes.
[ ] Particles: block break and hit, damage, smoke, explosions, rain splashes, portals, ambient.
[ ] Weather (rain, snow, thunder), stars, sky color per biome.
[ ] Biome tints for grass, foliage and water (tints.json is bundled; check it is applied everywhere).
[ ] Nether and End: dimension change visuals, portal overlay, world border.
[~] Light: client-side light for block edits is in (LightEngine); verify live, plus emissive blocks and server corrections.

### Player and physics
[ ] Elytra flight.
[ ] Status effects on screen: effect-driven FOV and overlays (blindness, nausea, night vision).
[ ] Check the death screen with respawn end to end.

### HUD and screens
[ ] Absorption hearts, heart variants (poison, wither, frozen, hardcore), hunger-effect icons, mount health and jump bar.
[ ] Scoreboard sidebar, advancement toasts.
[ ] Recipe book and recipe click-to-fill.
[ ] Chat: hover and click events, signed chat for secure-profile servers.
[ ] Main menu extras: server list with ping and MOTD, world select, language and resource pack screens.

### Audio
[ ] Note blocks, jukebox records, music discs.

### Networking and platform
[ ] Online-mode login (Mojang session auth) and secure chat keys.
[ ] Resource pack prompts and downloads.
[ ] Linux build and test pass (an Alpine/Weston test is planned; `execinfo.h` needs a musl guard); controller support.

## Rendering additions (2026-10-16, untested live)
[x] Light: LightEngine (src/renderer/terrain) re-spreads sky and block light on break/place and on server block updates; the server's Update Light corrects it.
[x] Fluids: vanilla corner-averaged heights, side faces, bottom faces; lava now renders (opaque, fullbright). Flow direction textures and fluid fog are not done.
[x] Chests (chest, trapped, ender, double halves): closed lid only, drawn by ModeledBEPass; the lid is merged into the chest mesh, so animation needs a mesh split. Not tested: facing and texture orientation.
[x] Mobs (untested live): data-driven textured models. A mob is a JSON model in resources/volcano/models/entity/ plus one line in mobs.json (model, skin, optional scale and layers); both are read at startup. Done: creeper, spider, cave_spider, chicken, pig, cow, mooshroom, sheep (white wool layer), enderman, villager, zombie_villager, husk, drowned, wolf, slime, magma_cube, cat, ocelot. Missing: bat, horses, slime outer cube, villager professions, enderman eyes, spider eyes, sheep dye colors, baby variants, all animation.

## Controls
[x] Rebindable keys: Controls.Keys.* in settings.json and Settings -> Controls -> Key Binds (`KeyBindings.hpp`). Defaults are vanilla; Esc, F2, F3 and F11 stay fixed.
[x] Drop key: tap drops one item, hold (0.35 s) or Ctrl drops the stack (`DropKey.hpp`). Not tested against a server yet.
[x] Inventory clicks: Shift+Click, hotbar-key swap and drop-over-slot send the protocol click modes 1/2/4 with no local prediction, so slots update when the server answers. Not tested against a server yet.
[ ] Inventory double-click gather, drag-split, creative inventory clicks.
