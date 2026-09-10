# Three-hit GAS passive setup

1. In your hero Blueprint, change the parent class to `AHeroCombatCharacter`. Set its Player State class to `AMyPlayerState`.
2. Create `DA_ThreeHitPassive` from `UThreeHitPassiveData`; assign the three montages and adjust each hit/window time. Defaults are 100%, 105%, 110%; stage 2's shorter window arms stage 3 knockback.
3. Create `GE_BasicAttackDamage` as an Instant GameplayEffect with an Execution/Modifier that consumes SetByCaller `Data.Damage`, then assign it to the data asset. This keeps damage resolution target-authoritative and reusable.
4. Assign `DA_ThreeHitPassive` to the granted `GA_ThreeHitPassive` ability (make a Blueprint child if you want each hero to own its data reference).
5. Bind the basic-attack Enhanced Input `Started` pin to `BasicAttackPressed`. Set the character's `AttackDamage`, `BaseAttackSpeed`, `AttackSpeedRatio`, and `BonusAttackSpeedPercent` using GameplayEffects.  `0.50` BonusAttackSpeedPercent means +50%.

The QTE is deliberately data-driven: set stage 2 `ChainWindowOpenTime` and `ChainWindowCloseTime` to a short interval. Input there queues the third hit and enables its configured launch. Input outside every window never advances a combo.
