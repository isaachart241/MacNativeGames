# Tears of the Kingdom abilities in Breath of the Wild (experimental)

Breath of the Wild **1.6.0** on the Mac can use **Ascend** from Tears of the Kingdom. It's off unless you turn it on,
and with it off the game runs exactly as before. 1.9.0 and the iPhone build don't have it.

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

The 1.6.0 addresses, and how each was found and checked, are at the top of the source file. The work is planned in
ThorNative's `BOTW_ABILITIES_SCOPE.md`. The tools that found the addresses and the test driver are in
`ThorNative/botw-abilities/tools/`.

## Diagnostics

- `TOTK_ABILITY_RAY=0`: no ray casts and no Ascend; just the abilities line.
- `TOTK_ASCEND_DUMP=<folder>`: during a rise, saves Link's actor and Havok blocks, logs which collision layers are above
  him, and logs his height and velocity for a few seconds after landing.
