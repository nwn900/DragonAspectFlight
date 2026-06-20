Dragon Aspect Flight uses Behavior Data Injector by default, so users do not need to run Nemesis or Pandora.

The included Pandora_Engine patch registers the same Dragon Aspect Flight graph variables and events for users who explicitly choose a generated-behavior workflow instead of BDI.

Nemesis can consume mod folders in this shape, but it does not support Pandora's append-only XML patch format. Do not ship handcrafted #NNNN behavior fragments here unless they are generated from the exact target 0_master behavior graph, because stale full-object fragments can overwrite unrelated behavior mods. If a future change needs real new graph topology that BDI cannot inject, generate the Nemesis #NNNN fragments from the matching behavior source and place them beside this info.ini.
