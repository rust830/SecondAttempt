// Copyright Epic Games, Inc. All Rights Reserved.
#include "GAS/HeroCombatCharacter.h"
#include "GAS/MyAbilitySystemComponent.h"
#include "GAS/MyGameplayAbility.h"
#include "GAS/AbilitySet.h"
#include "GAS/BlockComponent.h"
#include "GAS/MyPlayerState.h"
#include "GAS/HeroCombatAttributeSet.h"
#include "GAS/GE_Death.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "GAS/LOLGameplayTags.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"

AHeroCombatCharacter::AHeroCombatCharacter()
{
	bReplicates = true;
	// 原生标签：CDO 构造阶段用字符串 RequestGameplayTag 拿不到（返回 None），必须直接用原生标签对象。
	PassiveSlotTag = LOLGameplayTags::Ability_Slot_Passive;
	BasicAttackInputTag = LOLGameplayTags::Event_Input_BasicAttack;

	// 格挡/免疫判定组件。ASC 不在这里拿（挂在 PlayerState 上），等 InitializeAbilityActorInfo。
	BlockComponent = CreateDefaultSubobject<UBlockComponent>(TEXT("BlockComponent"));

	// 死亡状态 GE。放默认值而不是要求每个 BP 都填：漏填的表现是「血到 0 什么都不发生」。
	DeathEffect = UGE_Death::StaticClass();
}

void AHeroCombatCharacter::BeginPlay()
{
	Super::BeginPlay();

	// 布娃娃会把网格的相对变换整个交给物理（位置/旋转都被写烂），重生时要还原成默认值 ——
	// 这个默认值必须在任何一次死亡之前存下来。Character 默认是 (0,0,-96)/(0,-90,0)，
	// 但 BP 里可能调过，所以不能硬编码。
	if (const USkeletalMeshComponent* MeshComp = GetMesh())
	{
		DefaultMeshRelativeLocation = MeshComp->GetRelativeLocation();
		DefaultMeshRelativeRotation = MeshComp->GetRelativeRotation();
		DefaultMeshCollisionProfileName = MeshComp->GetCollisionProfileName();
	}

	// 胶囊的碰撞配置死亡时会改，同样存一份原值（默认是 "Pawn"，但别写死）。
	if (const UCapsuleComponent* Capsule = GetCapsuleComponent())
	{
		DefaultCapsuleCollisionProfileName = Capsule->GetCollisionProfileName();
	}
}

void AHeroCombatCharacter::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);
	InitializeAbilityActorInfo();
}

void AHeroCombatCharacter::OnRep_PlayerState()
{
	Super::OnRep_PlayerState();
	InitializeAbilityActorInfo();
}

void AHeroCombatCharacter::InitializeAbilityActorInfo()
{
	AMyPlayerState* PS = GetPlayerState<AMyPlayerState>();
	if (!PS) return;
	AbilitySystemComponent = PS->GetAbilitySystemComponent();
	AbilitySystemComponent->InitAbilityActorInfo(PS, this);

	// 把 ASC 交给格挡组件。放这里而不是组件的 BeginPlay：ASC 是从 PlayerState 上来的，
	// PossessedBy / OnRep_PlayerState 的时序不保证 BeginPlay 时它就绪。
	if (BlockComponent)
	{
		BlockComponent->BindToAbilitySystem(AbilitySystemComponent);
	}

	// 死亡入口。属性集内部已经只认权威端（见 PostGameplayEffectExecute），这里不用再判一次。
	// 先摘再挂：这个函数会被调两次（服务端 PossessedBy / 客户端 OnRep_PlayerState），
	// 挂两层的话一次死亡会广播两次。
	if (UHeroCombatAttributeSet* Attributes = const_cast<UHeroCombatAttributeSet*>(AbilitySystemComponent->GetSet<UHeroCombatAttributeSet>()))
	{
		Attributes->OnOutOfHealth.RemoveDynamic(this, &AHeroCombatCharacter::HandleOutOfHealth);
		Attributes->OnOutOfHealth.AddDynamic(this, &AHeroCombatCharacter::HandleOutOfHealth);
	}

	// State.Dead 的标签事件。两端都要听，理由不同：
	//   服务端 → 重生（回出生点 + 回血）
	//   所有端 → 布娃娃 + 停移动（表现不能等服务端那一份复制到才做）
	// 同样先摘再挂。
	if (DeadTagDelegateHandle.IsValid())
	{
		AbilitySystemComponent->UnregisterGameplayTagEvent(DeadTagDelegateHandle, LOLGameplayTags::State_Dead, EGameplayTagEventType::NewOrRemoved);
	}
	DeadTagDelegateHandle = AbilitySystemComponent->RegisterGameplayTagEvent(LOLGameplayTags::State_Dead, EGameplayTagEventType::NewOrRemoved)
		.AddUObject(this, &AHeroCombatCharacter::OnDeadTagChanged);

	// MoveSpeed 属性 → CharacterMovement->MaxWalkSpeed 的桥。理由见头文件那段注释。
	// 同样先摘后挂：这个函数会被调两次。
	if (MoveSpeedDelegateHandle.IsValid())
	{
		AbilitySystemComponent->GetGameplayAttributeValueChangeDelegate(UHeroCombatAttributeSet::GetMoveSpeedAttribute())
			.Remove(MoveSpeedDelegateHandle);
	}
	MoveSpeedDelegateHandle = AbilitySystemComponent->GetGameplayAttributeValueChangeDelegate(UHeroCombatAttributeSet::GetMoveSpeedAttribute())
		.AddUObject(this, &AHeroCombatCharacter::OnMoveSpeedChanged);

	// 绑完立刻同步一次。属性比这个绑定先存在（ASC 带着属性集一起建出来），
	// 不同步的话要等到第一次减速生效才会用上属性值 —— 在那之前角色还是
	// 构造函数里那个硬编码的 MaxWalkSpeed。
	ApplyMoveSpeedToMovement();

	// 服务端授予英雄技能组（被动 + QWER）。召唤师技能在 PlayerState 里授予。只授一次。
	if (HasAuthority() && !bAbilitiesGranted && ChampionKit)
	{
		ChampionKit->GiveToAbilitySystem(AbilitySystemComponent);
		bAbilitiesGranted = true;
	}
}

void AHeroCombatCharacter::OnMoveSpeedChanged(const FOnAttributeChangeData& Data)
{
	ApplyMoveSpeedToMovement();
}

void AHeroCombatCharacter::ApplyMoveSpeedToMovement()
{
	UCharacterMovementComponent* Movement = GetCharacterMovement();
	if (!Movement) return;

	if (!AbilitySystemComponent) return;

	// 读【当前值】而不是基础值：减速是乘算的修正符（UGE_Slow 用 MultiplyCompound），
	// 它只改聚合后的当前值，基础值一直是数值表里那个数。GetNumericAttribute 拿到的
	// 就是含全部修正符的当前值。
	const float Speed = AbilitySystemComponent->GetNumericAttribute(UHeroCombatAttributeSet::GetMoveSpeedAttribute());

	// 属性还没初始化就写会把角色钉死在原地 —— 宁可沿用构造函数里的值。
	// 属性集里 MoveSpeed 已经钳到 >= 0，但 0 同样是不能走的（钳到 0 = 定身，
	// 那是「禁锢」类效果该干的事，不该由这里顺手做掉）。
	if (Speed <= 0.f) return;

	Movement->MaxWalkSpeed = Speed;
}

bool AHeroCombatCharacter::IsDead() const
{
	return AbilitySystemComponent && AbilitySystemComponent->HasMatchingGameplayTag(LOLGameplayTags::State_Dead);
}

void AHeroCombatCharacter::HandleOutOfHealth(AActor* DamageInstigator, AActor* DamageCauser)
{
	// 属性集里已经在权威端门过一次。这里再判是为了防御：OnOutOfHealth 是 BlueprintAssignable 的，
	// BP 里也能挂，从别的路调进来时不该在客户端挂死亡 GE。
	if (!HasAuthority() || !AbilitySystemComponent) return;
	if (AbilitySystemComponent->HasMatchingGameplayTag(LOLGameplayTags::State_Dead)) return;

	LastDamageInstigator = DamageInstigator;
	LastDamageCauser = DamageCauser;

	// 写成两句而不是三目：TSubclassOf<> 和 UClass* 在 ?: 里推不出共同类型（C2445）。
	TSubclassOf<UGameplayEffect> EffectClass = DeathEffect;
	if (!EffectClass)
	{
		EffectClass = UGE_Death::StaticClass();
	}

	FGameplayEffectContextHandle Context = AbilitySystemComponent->MakeEffectContext();
	Context.AddSourceObject(this);

	FGameplayEffectSpecHandle Spec = AbilitySystemComponent->MakeOutgoingSpec(EffectClass, 1.f, Context);
	if (!Spec.IsValid())
	{
		// 静默失败的表现是「死了但什么都没发生」——人站着不动、血是 0、还能继续打。必须吵。
		UE_LOG(LogTemp, Error, TEXT("[Death] %s 造不出死亡 GE 的 Spec（GE=%s）→ 死亡状态没挂上"),
			*GetNameSafe(this), *GetNameSafe(EffectClass.Get()));
		return;
	}

	// GE_Death 的时长是 SetByCaller（FSetByCallerFloat 没有默认值），不填就是 0、挂上立刻过期，
	// 表现成「死了马上原地复活」。
	Spec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_RespawnDelay, FMath::Max(0.f, RespawnDelay));

	// 只在本端（权威）施加：State.Dead 会随 GE 复制到各端，各端再各自跑 EnterDeathState。
	// 走 ApplyGameplayEffectSpecToSelf 而不是 ToOwner —— 这是服务端自己身上挂，不涉及预测。
	AbilitySystemComponent->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());

	UE_LOG(LogTemp, Warning, TEXT("[Death] %s 挂上死亡状态，%.1fs 后复活（凶手=%s）"),
		*GetNameSafe(this), RespawnDelay, *GetNameSafe(DamageCauser));
}

void AHeroCombatCharacter::OnDeadTagChanged(const FGameplayTag ChangedTag, int32 NewCount)
{
	if (ChangedTag != LOLGameplayTags::State_Dead) return;

	if (NewCount > 0) EnterDeathState();
	else              ExitDeathState();
}

void AHeroCombatCharacter::EnterDeathState()
{
	UE_LOG(LogTemp, Warning, TEXT("[Death] %s 进入死亡状态（权威=%d）"), *GetNameSafe(this), HasAuthority() ? 1 : 0);

	// 死的时候若还按着 R，选择窗口得关掉：不然左键会一直被它吃掉（人死了左键也没别的用，
	// 但标签留着会让「重生后第一次左键不普攻」这种事发生）。松开键本来也会关，这里只是补一条更早的路。
	EndManualTargetSelect();

	// ① 停住 + 关移动。两端都做：服务端那份随 CharacterMovement 复制下去，
	//    客户端这份让本地当帧就站住，不用等复制（否则会「尸体还能滑两步」）。
	//    顺带把「死亡期间不能走」也一起解决了 —— ALOLCharacter::DoMove 还有一道 IsDead 的门，
	//    那一道是给「客户端预测已经停但输入还在来」这种情况兜底的。
	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->StopMovementImmediately();
		Movement->DisableMovement();
	}

	// ② 胶囊关碰撞。布娃娃之后所有碰撞都该由物理体负责；胶囊还开着的话尸体会卡在半空、
	//    而且别的角色会撞在一根看不见的柱子上。
	if (UCapsuleComponent* Capsule = GetCapsuleComponent())
	{
		Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}

	// ③ 布娃娃。纯本地表现：骨骼网格的相对变换不复制，各端各自模拟 —— 看起来都是「倒下去」，
	//    具体姿势每端略有出入。这是常规取舍，不是 bug。
	if (bRagdollOnDeath)
	{
		if (USkeletalMeshComponent* MeshComp = GetMesh())
		{
			// 先换碰撞配置再开物理：反过来的话会有一帧用 CharacterMesh（QueryOnly）去模拟。
			MeshComp->SetCollisionProfileName(TEXT("Ragdoll"));
			MeshComp->SetAllBodiesSimulatePhysics(true);
			MeshComp->WakeAllRigidBodies();
			MeshComp->bBlendPhysics = true;
		}
	}
}

void AHeroCombatCharacter::ExitDeathState()
{
	UE_LOG(LogTemp, Warning, TEXT("[Death] %s 复活（权威=%d）"), *GetNameSafe(this), HasAuthority() ? 1 : 0);

	// ① 收布娃娃。顺序要紧：先关物理，再挂回胶囊，最后写相对变换。
	//    反过来（先写变换再 Attach）的话 Attach 会把刚写进去的值覆盖掉，网格会歪。
	if (bRagdollOnDeath)
	{
		if (USkeletalMeshComponent* MeshComp = GetMesh())
		{
			MeshComp->SetAllBodiesSimulatePhysics(false);
			MeshComp->SetSimulatePhysics(false);
			MeshComp->bBlendPhysics = false;
			MeshComp->AttachToComponent(GetCapsuleComponent(), FAttachmentTransformRules::SnapToTargetNotIncludingScale);
			MeshComp->SetRelativeLocationAndRotation(DefaultMeshRelativeLocation, DefaultMeshRelativeRotation);
			MeshComp->SetCollisionProfileName(DefaultMeshCollisionProfileName);
		}
	}

	// ② 碰撞和移动还原。
	if (UCapsuleComponent* Capsule = GetCapsuleComponent())
	{
		Capsule->SetCollisionProfileName(DefaultCapsuleCollisionProfileName);
	}
	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		// DisableMovement 把模式设成了 MOVE_None，这里恢复成走路。
		// 只做这一步、不整份还原 CMC 状态：死亡期间本来也不该有别的东西改它。
		Movement->SetMovementMode(MOVE_Walking);
	}

	// ③ 回出生点 + 回血。只有服务端做，其余端跟着复制走。
	if (!HasAuthority()) return;

	FTransform RespawnTransform;
	if (FindRespawnTransform(RespawnTransform))
	{
		SetActorLocationAndRotation(RespawnTransform.GetLocation(), RespawnTransform.GetRotation(),
			/*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);
	}

	if (AbilitySystemComponent)
	{
		if (const UHeroCombatAttributeSet* Attributes = AbilitySystemComponent->GetSet<UHeroCombatAttributeSet>())
		{
			// 直接写基础值：复活重置不是「可被减免/驱散的治疗」，不该走 GE
			// （走 GE 的话 BlockComponent 可能把它算成伤害、以后加了重伤也会被削）。
			// PreAttributeBaseChange 会把值钳进 [0, MaxHealth]，这里传 MaxHealth 正好落回来。
			AbilitySystemComponent->SetNumericAttributeBase(UHeroCombatAttributeSet::GetHealthAttribute(), Attributes->GetMaxHealth());
			AbilitySystemComponent->SetNumericAttributeBase(UHeroCombatAttributeSet::GetEnergyAttribute(), Attributes->GetMaxEnergy());
		}
	}
}

bool AHeroCombatCharacter::FindRespawnTransform(FTransform& OutTransform) const
{
	UWorld* World = GetWorld();
	if (!World) return false;

	AGameModeBase* GameMode = World->GetAuthGameMode();
	if (!GameMode) return false;

	// 用引擎自己的选点入口（内部走 ChoosePlayerStart：优先挑【没被占用】的 PlayerStart）。
	// 不用 GetAllActorsOfClass + RandRange 自己随机：那样会挑到已经被别人站着的出生点。
	AActor* Start = GameMode->FindPlayerStart(GetController());
	if (!Start)
	{
		// 关卡里一个 PlayerStart 都没有（或全被占用且不允许复用）。
		// 不是致命错误 —— 原地满血复活，但要说一声，不然会被当成「重生没生效」。
		UE_LOG(LogTemp, Warning, TEXT("[Death] %s 找不到 PlayerStart → 原地复活"), *GetNameSafe(this));
		return false;
	}

	OutTransform = Start->GetActorTransform();
	return true;
}

void AHeroCombatCharacter::BasicAttackPressed()
{
	// ① 瞄准态：左键语义 = 确认投掷，拦截，不普攻。
	// 【这一段必须留在最前面】：投掷优先级高于普攻（含破隐强化普攻）。直接 return 掉，
	// 下面的 RouteBasicAttackInput 不会跑 → GA_ThreeHitPassive 不激活 → 它起手时那段
	// 「破隐 + 挂强化」也不跑，投掷那一击既不破隐也不消耗 State.EmpoweredAttack，
	// 投完之后的普攻照样吃强化。
	if (AbilitySystemComponent &&
		AbilitySystemComponent->HasMatchingGameplayTag(LOLGameplayTags::State_Throw_Aiming))
	{
		UE_LOG(LogTemp, Warning, TEXT("[ThrowDagger] 左键被瞄准态拦截 → 发投掷确认"));
		RouteThrowConfirmInput();
		return;
	}

	// ② 按住 R 选目标中：左键语义 = 选中准星上的人，同样是【拦截】，不普攻。
	// 和上面那条一样必须在 RouteBasicAttackInput 之前 return：不然这一下会先激活三连击被动。
	// 判据读的是标签（不是成员变量）：技能真激活时能力会摘掉它，这里就自然恢复成普攻。
	if (AbilitySystemComponent &&
		AbilitySystemComponent->HasMatchingGameplayTag(LOLGameplayTags::State_DeathHarvest_Selecting))
	{
		RouteManualTargetConfirm();
		return;
	}

	// ③ 破隐不在这里做，在 GA_ThreeHitPassive::ActivateAbility 的起手处：
	// 那边能看到「起手这一刻是否还在隐身」，且客户端/服务端各自的激活都会跑到，
	// 不依赖跨端时序。见那里的注释。
	UE_LOG(LogTemp, Warning, TEXT("[Passive] BasicAttackPressed 入口到达"));
	RouteBasicAttackInput();
	if (!HasAuthority()) ServerSubmitBasicAttackInput();
}

void AHeroCombatCharacter::RouteThrowConfirmInput()
{
	if (!AbilitySystemComponent) return;

	FGameplayEventData EventData;
	EventData.EventTag = LOLGameplayTags::Event_Input_ThrowConfirm;
	EventData.Instigator = this;
	EventData.Target = this;
	AbilitySystemComponent->HandleGameplayEvent(EventData.EventTag, &EventData);

	// GameplayEvent 只在本地 ASC 里派发，不会过网络。而 UThrowDaggerAbility 是 LocalPredicted：
	// 客户端 TryActivateAbility 会通过 CallServerTryActivateAbility 在服务端也建一份实例并进入瞄准态，
	// 那份实例同样在 WaitGameplayEvent 等这个确认。不镜像过去的话，服务端永远收不到确认 →
	// FireDagger 里 K2_HasAuthority() 挡着客户端、服务端又不投 → 谁都看不到匕首（listen server 非主机端的现象）。
	if (!HasAuthority())
	{
		UE_LOG(LogTemp, Warning, TEXT("[ThrowDagger] 客户端确认投掷 → 镜像到服务端"));
		ServerSubmitThrowConfirmInput();
	}
}

void AHeroCombatCharacter::ServerSubmitBasicAttackInput_Implementation()
{
	// 这里不用管破隐：服务端的 GA_ThreeHitPassive 激活（ServerTryActivateAbility）里，
	// 起手处会自己摘一次隐身并挂上强化普攻 —— 那是服务端的权威副本，和客户端的本地预测各自独立完成。
	RouteBasicAttackInput();
}

void AHeroCombatCharacter::ServerSubmitThrowConfirmInput_Implementation()
{
	// 服务端跑同一套本地路由：到这里 HasAuthority() 已为 true，不会再回发，无递归风险。
	UE_LOG(LogTemp, Warning, TEXT("[ThrowDagger] 服务端收到投掷确认"));
	RouteThrowConfirmInput();
}

void AHeroCombatCharacter::RouteBasicAttackInput()
{
	UMyAbilitySystemComponent* ASC = Cast<UMyAbilitySystemComponent>(AbilitySystemComponent);
	if (!ASC) { UE_LOG(LogTemp, Warning, TEXT("[Passive] RouteBasicAttackInput: ASC 为空")); return; }

	// 普攻 → 激活「被动槽位」里的技能，同时发 GameplayEvent（被动靠它推进连段）。
	// 直接用原生标签对象：CDO 构造阶段设的成员可能因蓝图覆盖/时序是 None，运行时用原生标签最稳。
	const FGameplayTag PassiveTag = LOLGameplayTags::Ability_Slot_Passive;
	const FGameplayTag AttackTag = LOLGameplayTags::Event_Input_BasicAttack;

	const FGameplayAbilitySpecHandle Handle = ASC->GetHandleForSlot(PassiveTag);
	UE_LOG(LogTemp, Warning, TEXT("[Passive] 槽位[%s] 标签有效=%d Handle有效=%d"), *PassiveTag.ToString(), PassiveTag.IsValid(), Handle.IsValid());
	if (Handle.IsValid())
	{
		const bool bActivated = ASC->TryActivateAbility(Handle, true);
		UE_LOG(LogTemp, Warning, TEXT("[Passive] TryActivateAbility 返回=%d"), bActivated);
	}

	FGameplayEventData EventData;
	EventData.EventTag = AttackTag;
	EventData.Instigator = this;
	ASC->HandleGameplayEvent(AttackTag, &EventData);
}

void AHeroCombatCharacter::AbilityInputTagPressed(FGameplayTag SlotTag)
{
	UMyAbilitySystemComponent* ASC = Cast<UMyAbilitySystemComponent>(AbilitySystemComponent);
	if (!ASC) return;

	// 「按住选目标」的能力（GA_DeathHarvest 的 R）：按下【不激活能力】，只开一个本地的选择窗口。
	// 这一刻什么都不发生：不进冷却、不锁移动、不发 cue、不破隐 —— 真正施法要等左键点中目标那一下
	// （见 RouteManualTargetConfirm）。判据读能力 CDO 上的标记，输入层不认识任何具体技能。
	if (const UMyGameplayAbility* Ability = ASC->GetAbilityForSlot(SlotTag);
		Ability && Ability->bManualTargetSelect)
	{
		BeginManualTargetSelect(SlotTag);
		return;
	}

	ASC->AbilityInputTagPressed(SlotTag);
}

void AHeroCombatCharacter::AbilityInputTagReleased(FGameplayTag SlotTag)
{
	// 松开键只关「手动选目标」的窗口。刻意【不】转发给 ASC 的 AbilityInputTagReleased：
	// 那个会广播 Event.Input.Released，项目里现在没有任何监听者（而且它还在用字符串查标签的旧写法）。
	if (ManualTargetSlotTag.IsValid() && ManualTargetSlotTag == SlotTag)
	{
		UE_LOG(LogTemp, Log, TEXT("[ManualTarget] 松开 %s → 关掉选择窗口（这次没放出去）"), *SlotTag.ToString());
		EndManualTargetSelect();
	}
}

void AHeroCombatCharacter::BeginManualTargetSelect(const FGameplayTag& SlotTag)
{
	ManualTargetSlotTag = SlotTag;

	// 本地 loose 标签：BasicAttackPressed 靠它把左键改判成「选目标」。
	// 只挂在按键这一端（不复制），也不参与任何 GAS 判定 —— 服务端压根不知道有这个窗口。
	// 幂等：按住不放重复回调（或按了两次）也只是重新挂一次同一个标签。
	if (AbilitySystemComponent)
	{
		AbilitySystemComponent->AddLooseGameplayTag(LOLGameplayTags::State_DeathHarvest_Selecting);
	}

	UE_LOG(LogTemp, Log, TEXT("[ManualTarget] 按住 %s → 可以左键点目标了（松开取消）"), *SlotTag.ToString());
}

void AHeroCombatCharacter::EndManualTargetSelect()
{
	const bool bWasSelecting = ManualTargetSlotTag.IsValid();
	ManualTargetSlotTag = FGameplayTag();

	if (bWasSelecting && AbilitySystemComponent)
	{
		AbilitySystemComponent->RemoveLooseGameplayTag(LOLGameplayTags::State_DeathHarvest_Selecting);
	}
}

AActor* AHeroCombatCharacter::TraceManualTargetUnderCrosshair() const
{
	const APlayerController* PC = Cast<APlayerController>(GetController());
	UWorld* World = GetWorld();
	if (!PC || !World)
	{
		return nullptr;
	}

	// 相机视角点（不是角色位置）：鼠标被永久捕获做转视角，屏幕上没有光标可选 ——
	// 「左键点中谁」= 准星（屏幕中心）那条射线打到谁。相机挂在弹簧臂上，射线还忽略自己，
	// 所以从相机往前打不会先撞到自己身上。
	FVector ViewLocation;
	FRotator ViewRotation;
	PC->GetPlayerViewPoint(ViewLocation, ViewRotation);

	// 射线给得足够长就行：它只决定「能找到多远的候选」，真正的射程判定在技能里
	// （UDeathHarvestData::LockRange，服务端复核），所以这里不另开配置项。
	const FVector TraceEnd = ViewLocation + ViewRotation.Vector() * 10000.f;

	FCollisionQueryParams Params(SCENE_QUERY_STAT(ManualTargetPick), /*bTraceComplex=*/false, this);
	FHitResult Hit;

	// 通道用 ECC_Pawn，不是 ECC_Visibility。
	// 引擎默认那两个角色碰撞配置都把 Visibility 设成 Ignore（BaseEngine.ini）：
	//   Pawn          → CustomResponses=((Channel="Visibility",Response=ECR_Ignore))
	//   CharacterMesh → CustomResponses=((Channel="Pawn",Response=ECR_Ignore),(Channel="Visibility",Response=ECR_Ignore))
	// 所以打 Visibility 的射线会从英雄身上【穿过去】，打到背后的墙/地 —— 表现就是「怎么点都点不中人」，
	// 而且拿到的 HitActor 是场景、看起来像「目标没有 ASC」。
	//
	// Pawn 通道没被这两条 profile 覆盖（默认 Block），所以：
	//   英雄胶囊 → 命中（判定要的就是这个体积，网格那条 profile 反而忽略 Pawn，穿过去正好落在胶囊上）
	//   墙 / 地面 → 也 Block（默认响应），所以「隔着墙点不到人」依然成立
	// 想改成「只打英雄、不满地图乱穿」的话，正确姿势是加一条自定义 Trace 通道，而不是回退到 Visibility。
	if (!World->LineTraceSingleByChannel(Hit, ViewLocation, TraceEnd, ECC_Pawn, Params))
	{
		UE_LOG(LogTemp, Log, TEXT("[ManualTarget] 准星射线没打到任何东西（起点 %s，方向 %s）"),
			*ViewLocation.ToCompactString(), *ViewRotation.Vector().ToCompactString());
		return nullptr;
	}

	AActor* HitActor = Hit.GetActor();
	UE_LOG(LogTemp, Log, TEXT("[ManualTarget] 准星命中 %s（组件 %s，距离 %.0f）"),
		*GetNameSafe(HitActor), *GetNameSafe(Hit.GetComponent()), Hit.Distance);

	// 只认「有 ASC 的别人」：没有 ASC 的东西（墙、地面、木桩）不是这个技能的目标。
	// 打中了墙就返回 null —— 隔着墙点不算，「让服务端挑一个墙后面的人」更是另一种语义了。
	if (!HitActor || HitActor == this)
	{
		UE_LOG(LogTemp, Log, TEXT("[ManualTarget] 命中的是自己或无效 actor → 不算目标"));
		return nullptr;
	}

	// 用项目自己的解析器（接口 → 所属 Pawn 的 PlayerState → 组件），不用蓝图库那个：
	// 蓝图库 Cast 到接口就直接返回接口的返回值，而英雄身上那个是从 PS 缓存来的、可能还是空指针。
	UAbilitySystemComponent* TargetASC = UMyAbilitySystemComponent::FindAbilitySystemComponent(HitActor);
	if (!TargetASC)
	{
		UE_LOG(LogTemp, Log, TEXT("[ManualTarget] 命中的 %s 上没有 ASC（接口/PlayerState/组件三条路都没找到）→ 不算目标"),
			*GetNameSafe(HitActor));
		return nullptr;
	}

	UE_LOG(LogTemp, Log, TEXT("[ManualTarget] 准星锁定 %s（ASC=%s）"), *GetNameSafe(HitActor), *GetNameSafe(TargetASC));
	return HitActor;
}

void AHeroCombatCharacter::RouteManualTargetConfirm()
{
	UMyAbilitySystemComponent* ASC = Cast<UMyAbilitySystemComponent>(AbilitySystemComponent);
	if (!ASC || !ManualTargetSlotTag.IsValid())
	{
		// 窗口状态和标签不该出现这种不一致；真出现了就当场关掉，别让左键一直哑着。
		EndManualTargetSelect();
		return;
	}

	AActor* Picked = TraceManualTargetUnderCrosshair();
	if (!Picked)
	{
		// 点空了（空地/墙/自己）：什么都不做，窗口继续开着 —— 可以接着瞄、再点，或者松开键取消。
		// LoL 里对着空地放指向性技能也是这个手感：不放，也不退出施法准备。
		UE_LOG(LogTemp, Log, TEXT("[ManualTarget] 左键没点到带 ASC 的目标 → 不施法，继续等"));
		return;
	}

	const FGameplayTag SlotTag = ManualTargetSlotTag;

	// 施法破隐的「本地那一半」：时机从「按下技能键」挪到了「真的点中目标」这一刻
	// （按住 R 只是选目标，还没施法）。非权威端摘的是本地预测副本，服务端那份由下面那条链自己摘。
	ASC->BreakStealthForCast(ASC->GetAbilityForSlot(SlotTag));

	// 服务端那一半：破隐（权威）+ 用这个目标当载荷激活能力。
	// 主机自己就是服务端，直接走同一条路；客户端由引擎把 RPC 发过去。
	if (HasAuthority())
	{
		ASC->SubmitManualTargetOnServer(SlotTag, Picked);
	}
	else
	{
		ServerSubmitManualTarget(SlotTag, Picked);
	}

	UE_LOG(LogTemp, Warning, TEXT("[ManualTarget] 左键选中 %s → 交给服务端激活"), *GetNameSafe(Picked));

	// 窗口【不】在这里关：关的时机是「技能真的激活了」（GA_DeathHarvest::ActivateAbility 摘标签）。
	// 这样服务端判定不过（超距/冷却/沉默）时窗口还开着，玩家可以走近了再点一次 ——
	// 而技能一激活标签就没了，剩下的左键自动恢复成普攻。
}

void AHeroCombatCharacter::ServerSubmitManualTarget_Implementation(FGameplayTag SlotTag, AActor* Target)
{
	// 服务端跑同一套：到这里 HasAuthority() 已为 true，不会再回发，无递归风险
	// （和 ServerSubmitThrowConfirmInput 一个写法）。
	if (UMyAbilitySystemComponent* ASC = Cast<UMyAbilitySystemComponent>(AbilitySystemComponent))
	{
		ASC->SubmitManualTargetOnServer(SlotTag, Target);
	}
}
