# Animated Armoury quarterstaff donor subset

Source: [Animated Armoury - New Weapons with animations SSE Version](https://www.nexusmods.com/skyrimspecialedition/mods/35978) by NickNak, version 2.3.

The author permits reworking, reanimation, and reuse with credit. This folder intentionally contains only seven third-person quarterstaff action donors from DAR condition folder 13:

- block anticipate
- bash intro
- power bash
- block hit
- block idle
- equip
- unequip

`tools/BuildFlightAnimationStack.py` preserves each donor's upper-body action and annotations while replacing root/lower-body tracks with DAF's flight pose. The full Animated Armoury package is not bundled. Its normal `2hw_blockbash.hkx` donor is excluded because it uses an interleaved animation representation that the verified compositor cannot load; DAF uses the compatible vanilla normal-bash source for that one original.
