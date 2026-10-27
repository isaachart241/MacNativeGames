# Tears of the Kingdom abilities in Breath of the Wild (experimental)

Breath of the Wild **1.6.0** on the Mac can use **Ascend** and **Recall** from Tears of the Kingdom. They're off unless
you turn them on, and with them off the game runs exactly as before. 1.9.0 and the iPhone build don't have them.

## Turn it on

```sh
defaults write org.macnativegames.botw TOTK_ABILITIES -bool YES
```

`defaults delete org.macnativegames.botw TOTK_ABILITIES` turns it off again. `TOTK_ABILITIES=1` in the environment
does the same for one run and wins over the setting.

## Ascend

Stand under a ceiling: a roof, a cave or a shrine's rock. When Ascend can take you up, the abilities line in the
corner panel (key 1: all) turns yellow and says **(ZL+L: Ascend)**. Hold **ZL** and press **L**. Link rises through the
ceiling at 4 m/s and lands on top of it. **B** while rising takes him back down.

- The ceiling has to be within 12 m above his head, and its top within 64 m above that, with room to stand.
  `TOTK_ASCEND_REACH` and `TOTK_ASCEND_THICK` change these.
- While Ascend is ready, ZL + L is Ascend's, so it doesn't fire a rune. Everywhere else ZL + L is BotW's as always.
- Tested on 2026-10-26: out of the Shrine of Resurrection onto the plateau above it (about 39 m), walking on after,
  and cancelling halfway.
- Puzzles and scripted scenes weren't made for this. Shrines can be skipped, and a scene can get stuck. Turn it off for
  those.

## Recall

Anything with physics that moved in the last 30 seconds near Link can be sent back along its path: a crate, a barrel,
a boulder, a pot. Recall works as it does in Tears of the Kingdom: it's a mode.

- **ZL + R** enters Recall. The abilities line says **Recall: look at something that moved**.
- Look at the object with the camera; nothing needs holding. When it's recallable the line turns yellow and says how
  many seconds of motion it has, for example **Recall: 3.1 s of motion, 18 m (R: recall, B: leave)**.
- **R** sends it back along its path at the speed it went. **R** or **B** lets it go early. Either way physics takes
  over at the end: in the air, it falls.
- **B** leaves Recall without using it.

Details:
- Aim doesn't have to be exact: the nearest recallable object within 15° of where the camera looks, and in its sight,
  is taken.
- The object is driven like a moving platform (physics carries it), so whatever stands on it goes along.
- Only objects with physics count. Keyframed things (doors, platforms moved by animation) and the ground don't.
- In the mode, R and B are Recall's. Outside it, ZL + R is the only press it takes; everything else is BotW's.
- Tested on 2026-10-26 in the Shrine of Resurrection, on a small physics object knocked across the next room by a test
  kick (aimed through the closed door with `TOTK_RECALL_SEE_ALL`). Riding a recalled object isn't tested yet: there's
  nothing to stand on there.

## Tears of the Kingdom's sounds

Ascend and Recall play **Tears of the Kingdom's own sounds**, decoded from your TotK dump. In TotK's data Ascend is
"Toreroof" and Recall "Modoreco". Make the TotK work folder with `build_app.py` from your TotK dump, then:

```sh
python3.14 games/botw/scripts/extract_totk_ability_sounds.py
```

This writes 26 WAV files to `~/Library/Application Support/MacNativeGames/BotW/abilities/sound`. Without them the
abilities are silent; nothing else changes. The repository holds no sound from either game.

- **Ascend:** the start (`Toreroof_Start`, `Toreroof_Green_Start`, the jump), going into the ceiling (`…_In` with the
  ground and background loops), coming out on top (`…_Out`, `…_Out_Eff`, `Toreroof_Green_End`), and cancelling.
- **Recall:** entering the mode with its search loop, the rewind's start and spread, its loop following the object
  (panned and quieter with distance), the rewind's end, and leaving the mode.
- They're mixed into the game's output at the game's volume, at half level for now
  (`TOTK_ABILITY_SOUND_VOLUME`, default 0.5): TotK gives each sound its own level in its sound tables, which aren't
  read yet, and at full level four sounds together clipped.
- `TOTK_ABILITY_SOUND=0` turns them off.

Still to come for 1:1: TotK's own effects (Ascend's `Tooreroof_*`, Recall's) and Link's TotK animations (Ascend's
`WarpHeightIn` / `WarpHeightOut`, Recall's `Modoreco_*`), converted from the TotK dump the same way.

## How it works

`games/totk/runtime/botw_abilities.c`, entered from two places:

- **The game's controller poll** (`input.c`), on the game's main thread once a frame. It casts rays with the game's own
  `RayCast::worldRayCast`, which needs a thread Havok was set up on, so it can't run from the present. It decides
  whether Ascend is ready, takes ZL + L, and moves Link.
- **The present**, which only draws the abilities line.

Moving Link uses the same method as `world_survey.c`'s tested `survey move`: every copy of his position in his actor,
his Havok body and the camera keeps its offset from him and is written every frame. His Havok velocity is zeroed with
them. He ends a metre above the top and is held there for a moment, so the contacts Havok kept from passing through the
ceiling expire before he drops onto it.

Recall records, every frame, the centre of mass and rotation of each moving physics body within 60 m. The bodies come
from the Havok world's active simulation islands, using layouts from the decompilation's Havok headers. Recording costs
about 30 µs a frame. A recalled body gets, each frame, the linear and angular velocity that take it to the next point
of its path. That's how a keyframed body moves, so physics carries it and anything on it.

The 1.6.0 addresses, and how each was found and checked, are at the top of the source file. The work is planned in
ThorNative's `BOTW_ABILITIES_SCOPE.md`. The tools that found the addresses and the test driver are in
`ThorNative/botw-abilities/tools/`.

## Diagnostics

- `TOTK_ABILITY_RAY=0`: no ray casts and no Ascend; just the abilities line.
- `TOTK_ASCEND_DUMP=<folder>`: during a rise, saves Link's actor and Havok blocks, logs which collision layers are above
  him, and logs his height and velocity for a few seconds after landing.
- `TOTK_ABILITY_FX=botw`: BotW effects stand in for TotK's at the same moments (tests of the plumbing only).
- `TOTK_XLINK_PROBE=name,…` (then ZL + Down) and `TOTK_XLINK_LIST=<file of names>`: which xlink events Link (and, in a
  recall test, the recalled object) has.
- `TOTK_RECALL_DEBUG=1`: ZL + Y kicks the movable body nearest the aim (within 25°) up and away, and ZL + X recalls the
  nearest body with motion. The log lists the bodies around the camera, follows a kicked body, says what the aim hits
  and what recording costs.
- `TOTK_RECALL_SEE_ALL=1`: the aim also takes bodies out of sight (for tests behind closed doors).
