# Three-hit GAS passive setup

1. In your hero Blueprint, change the parent class to `AHeroCombatCharacter`. Set its Player State class to `AMyPlayerState`.
2. Create `DA_ThreeHitPassive` from `UThreeHitPassiveData`; assign the three montages and adjust each hit/window time. Defaults are 100%, 105%, 110%; stage 2's shorter window arms stage 3 knockback. For the stage 2 → 3 perfect window, see the note at the bottom.
3. Create `GE_BasicAttackDamage` as an Instant GameplayEffect with an Execution/Modifier that consumes SetByCaller `Data.Damage`, then assign it to the data asset. This keeps damage resolution target-authoritative and reusable.
4. Assign `DA_ThreeHitPassive` to the granted `GA_ThreeHitPassive` ability (make a Blueprint child if you want each hero to own its data reference).
5. Bind the basic-attack Enhanced Input `Started` pin to `BasicAttackPressed`. Set the character's `AttackDamage`, `BaseAttackSpeed`, `AttackSpeedRatio`, and `BonusAttackSpeedPercent` using GameplayEffects.  `0.50` BonusAttackSpeedPercent means +50%.

## The stage 2 → 3 perfect window

The QTE is deliberately data-driven. Two nested windows on stage 2 decide what the third hit is:

```
stage 2 montage ────────────────────────────────────────────────►
        │                      │              │
        ChainWindowOpen   PerfectWindow    ChainWindowClose
        └──── input: normal 3rd hit ──┴─ input: EMPOWERED 3rd hit ─┘
        (input before ChainWindowOpenTime or after ChainWindowCloseTime: combo just ends)
```

- `PerfectWindowOpenTime` / `PerfectWindowCloseTime` are in **authored montage seconds**, same units as `HitTime` and the chain window (both get divided by the attack-speed play rate).
- The **first** input inside the chain window decides. Pressing early and mashing in the perfect window afterwards does *not* upgrade it — otherwise holding the button would always be perfect and the window would be meaningless.
- Leaving both at `0` (or `Close <= Open`) means "not configured" and falls back to **the whole chain window**, i.e. the old behaviour. That fallback and any window with no overlap with the chain window are logged — nothing here fails silently.
- Only the effective intersection with the chain window counts, so keep the perfect band inside `[ChainWindowOpenTime, ChainWindowCloseTime]`.
- Because only stage 2's input can arm stage 3, tick `bPerfectWindowEnablesNextHitKnockback` on stage 2 only. (The name is historical: the flag arms the whole empowered hit — damage multiplier, knockback and launch — not just knockback. It is left as-is so existing data assets keep their value.)
- Give the chain window enough slack that "press too early / too late" still lands a normal third hit; the perfect band is what should be tight. E.g. chain window `[0.15, 0.55]` with a perfect window of `[0.30, 0.42]`.

Input outside every window never advances a combo.
