// 形态切换技能的实现。架构理由见头文件。

#include "GAS/GA_FormSwitch.h"
#include "GAS/GE_FormUnarmed.h"
#include "GAS/HeroAnimInstance.h"
#include "GAS/HeroCombatCharacter.h"
#include "GAS/LOLGameplayTags.h"
#include "GAS/MyAbilitySystemComponent.h"
#include "Abilities/Tasks/AbilityTask_PlayMontageAndWait.h"
#include "Animation/AnimMontage.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"

UGA_FormSwitch::UGA_FormSwitch()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;

	// LocalPredicted：切换是本地手感的操作，动画必须当帧播，不能等服务端回发。
	// 标签本身走 GE 复制（见头文件），所以预测这一层只影响动画、不影响状态同步。
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;

	ActivationPolicy = EMyAbilityActivationPolicy::OnInputTriggered;

	// 形态切换【不破隐】。理由：收刀/拔刀是个不涉及攻击的动作，隐身后偷偷收刀
	// 合理但打断节奏（玩家按了就得等），而且形态切换本身不产生攻击。
	// 真要做「显形收刀」的玩法时再改回 true。
	bBreaksStealthOnCast = false;

	// 挡重入。State.Form.Switching 是本地标签（只挂在按键这一端），挡的是
	// 「切换动画还没播完又按了一下」—— 那样会起第二个 montage 打断第一个，
	// 而标签切换在 OnMontageFinished 里，第二个 montage 播完会切到【起始态】
	//（下面 bTargetUnarmed 是起手时锁的）⇒ 连按两下 = 白切一次。
	ActivationBlockedTags.AddTag(LOLGameplayTags::State_Form_Switching);

	// 死亡/硬控在基类（UMyGameplayAbility 构造函数）已经挡了。
	// 沉默【不】加：形态切换不是法术（沉默只挡法术，见 MyGameplayAbility.h）。

	FormUnarmedGE = UGE_FormUnarmed::StaticClass();
}

void UGA_FormSwitch::ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	Super::ActivateAbility(Handle, ActorInfo, ActivationInfo, TriggerEventData);

	UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo();
	if (!ASC)
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	// 【不动 FormGEHandle】—— 它记的是「当前这一态是空手时挂着的那份 GE」，
	// 是跨激活的状态，激活时重置就等于「永远以为自己不是空手」。
	// 该重置的是本次的目标态和「有没有播到动画」这两个一次性标记。
	bPlayedMontage = false;

	// 目标态 = 当前态的相反。起手这一刻锁定 —— 中途不改，
	// 否则「连按两下」会切回原形（见 ActivationBlockedTags 那段）。
	bTargetUnarmed = !ASC->HasMatchingGameplayTag(LOLGameplayTags::State_Form_Unarmed);

	// 门住重入。只挂在按键这一端（不复制）：服务端不需要知道「某人正在切形态」——
	// 标签切换是服务端做的，客户端动画靠预测，重复激活的风险只有客户端这边有。
	// ⚠️ EndAbility 里必须摘掉（蒙太奇被打断时 EndAbility 才是唯一收尾点）。
	ASC->AddLooseGameplayTag(LOLGameplayTags::State_Form_Switching);

	// Commit 放在这里：形态切换不要 CD（见头文件），Commit 只做「消耗资源 + 施加 CooldownGE」，
	// 这里 CooldownDuration = 0 ⇒ 不挂冷却 GE ⇒ 纯粹走个流程。
	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	// 选动画。留空 = 不播直接切（会瞬切，见头文件里 ToUnarmedMontage 的说明）。
	UAnimMontage* Montage = bTargetUnarmed ? ToUnarmedMontage.Get() : ToArmedMontage.Get();

	if (!Montage)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[FormSwitch] %s 没配 %s Montage → 不播动画直接切形态"),
			*GetNameSafe(GetAvatarActorFromActorInfo()), bTargetUnarmed ? TEXT("ToUnarmed") : TEXT("ToArmed"));
		ApplyForm(bTargetUnarmed);
		EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
		return;
	}

	// 【不再锁移动】（2026-10-02 定稿）：收刀/拔刀期间允许照常移动——
	// 「按一下切形态人定住」比动画和位移并存更破坏手感。montage 是
	// UpperBody 之外的 slot 姿势，跑动中播也不脱节（slot 覆盖本来就与移动无关）。

	UAnimInstance* AnimInstance = ActorInfo ? ActorInfo->GetAnimInstance() : nullptr;
	if (!AnimInstance)
	{
		// 配了 Montage 但角色没有 AnimBP（专用服务器 / 角色没配 ABP）：
		// 服务端跑这条路 ⇒ 直接切形态（不能因为「本机没动画」就卡住状态切换）。
		UE_LOG(LogTemp, Warning, TEXT("[FormSwitch] 没有 AnimInstance（权威=%d）→ 跳过动画直接切形态"),
			K2_HasAuthority() ? 1 : 0);
		ApplyForm(bTargetUnarmed);
		EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
		return;
	}

	// 【拔刀强制亮剑】藏剑判定在 HeroAnimInstance 里看 FormUnarmed > 0.5，而权重是
	// 平滑逼近的 —— 切回持刀的 montage 前半段剑还是隐形的，「拔刀」却看不见剑。
	// 拔刀方向在这里把标记置上，montage 一启动剑就出现；结束回调里摘（收刀方向
	// 从不置位，空手期间剑保持隐藏 = 现状不变）。
	if (!bTargetUnarmed)
	{
		if (UHeroAnimInstance* HeroAnim = Cast<UHeroAnimInstance>(AnimInstance))
		{
			HeroAnim->SetSwordForceVisible(true);
		}
	}

	UAbilityTask_PlayMontageAndWait* Task = UAbilityTask_PlayMontageAndWait::CreatePlayMontageAndWaitProxy(
		this, NAME_None, Montage, 1.f, NAME_None);
	// 五个回调全绑：切换动画被打断（被控/死亡/切场景）时也必须走收尾，
	// 否则标签停在「切换中」，而那个标签又会挡住下一次切换 ⇒ 「再也切不了」。
	Task->OnCompleted.AddDynamic(this, &UGA_FormSwitch::OnMontageFinished);
	Task->OnBlendOut.AddDynamic(this, &UGA_FormSwitch::OnMontageFinished);
	Task->OnInterrupted.AddDynamic(this, &UGA_FormSwitch::OnMontageFinished);
	Task->OnCancelled.AddDynamic(this, &UGA_FormSwitch::OnMontageFinished);
	Task->ReadyForActivation();

	bPlayedMontage = true;
}

void UGA_FormSwitch::OnMontageFinished()
{
	// OnBlendOut 与 OnCompleted 会对同一段动画各触发一次 ⇒ 这里自己加一道幂等门。
	if (!bPlayedMontage) return;
	bPlayedMontage = false;

	// 摘拔刀亮剑标记。置位只在拔刀方向发生过，这里无条件清（幂等）——
	// 摘完之后 FormUnarmed 还在往 0 平滑，剑渐隐 = 收刀蒙太奇的视觉过渡。
	if (UHeroAnimInstance* HeroAnim = Cast<UHeroAnimInstance>(GetCurrentActorInfo()->GetAnimInstance()))
	{
		HeroAnim->SetSwordForceVisible(false);
	}

	// 动画播完（或被打断）才真的切形态 —— 这就是「顺序纪律」的执行点。
	// 被打断时也切：已经播了一半了，标签还停在旧态的话视觉会停在半截姿势上
	//（ABP 权重按旧值继续平滑，slot 释放后回到状态机 = 持刀姿势），
	// 而人已经拔刀一半 —— 下一个动作序列读到的形态和看到的不一致。
	ApplyForm(bTargetUnarmed);

	// ★ 必须在这里显式 End：montage task 播完只 EndTask（销毁任务本身），
	//   【不会】结束 GA —— 不调的话 GA 永远 active，后果是：
	//   ① bMovementLocked 永不恢复（恢复逻辑只在 EndAbility）⇒ 角色定在原地不能动；
	//   ② State_Form_Switching 门永不摘（ActivationBlockedTags 挡重入）⇒ 切不回形态。
	//   （2026-10-02 实锤的两个症状，根因都是这一行缺失。）
	EndAbility(GetCurrentAbilitySpecHandle(), GetCurrentActorInfo(),
		GetCurrentActivationInfo(), true, false);
}

void UGA_FormSwitch::ApplyForm(bool bToUnarmed)
{
	UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo();
	if (!ASC) return;

	// ★ 只在权威端改状态：标签走 GE 复制到各端，本地施加会多出一份本地 GE，
	//   而非权威端的 RemoveActiveGameplayEffect 是**静默 no-op**（引擎里就是
	//   `if (IsOwnerActorAuthoritative()) {...} return 0;`）⇒ 两端状态永久分叉。
	if (!K2_HasAuthority())
	{
		return;
	}

	if (!bToUnarmed)
	{
		// 切回持刀 = 摘掉空手那份 GE。
		if (FormGEHandle.IsValid())
		{
			ASC->RemoveActiveGameplayEffect(FormGEHandle);
			FormGEHandle.Invalidate();
			UE_LOG(LogTemp, Log, TEXT("[FormSwitch] 摘掉 FormUnarmedGE → 持刀"));
		}
		else
		{
			// 没有句柄可摘 = 本来就不是空手。这是正常路径（连着按两次回到持刀），
			// 不吵。但「明明标签在却没有句柄」才是异常，值得喊出来。
			if (ASC->HasMatchingGameplayTag(LOLGameplayTags::State_Form_Unarmed))
			{
				UE_LOG(LogTemp, Warning,
					TEXT("[FormSwitch] 标签显示是空手，但 FormGEHandle 无效（技能实例被复用了？）→ 用标签兜底摘一次"));
				UMyAbilitySystemComponent::RemoveGrantedTagEffects(ASC,
					FGameplayTagContainer(LOLGameplayTags::State_Form_Unarmed));
			}
			else
			{
				UE_LOG(LogTemp, Log, TEXT("[FormSwitch] 切回持刀（本来就是持刀，无事可做）"));
			}
		}
		return;
	}

	// 切到空手：先摘掉可能存在的旧份（重复切换/句柄失效时不留两层），再挂。
	if (FormGEHandle.IsValid())
	{
		ASC->RemoveActiveGameplayEffect(FormGEHandle);
		FormGEHandle.Invalidate();
	}

	if (!FormUnarmedGE)
	{
		UE_LOG(LogTemp, Warning, TEXT("[FormSwitch] FormUnarmedGE 为空 → 切不了空手"));
		return;
	}

	// 用 MakeEffectContext + ApplyGameplayEffectSpecToSelf 而不是 ApplyGameplayEffectToOwner：
	// 后者取 GetPredictionKeyForNewAction()，而这个调用点在 montage 回调里、**不在预测窗口内**
	// ⇒ 客户端那边拿到无效键 → 静默不生效（和 ApplyEmpoweredAttack 里记的同一个坑）。
	// 这里本来就只在服务端调（ApplyForm 开头那道门），所以直接用裸的施加速度即可。
	FGameplayEffectContextHandle Context = ASC->MakeEffectContext();
	Context.AddSourceObject(this);

	FGameplayEffectSpecHandle Spec = ASC->MakeOutgoingSpec(FormUnarmedGE, 1.f, Context);
	if (!Spec.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("[FormSwitch] FormUnarmedGE(%s) 的 Spec 无效 → 切不了空手"),
			*GetNameSafe(FormUnarmedGE));
		return;
	}

	FormGEHandle = ASC->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
	UE_LOG(LogTemp, Log, TEXT("[FormSwitch] 挂上 FormUnarmedGE（句柄有效=%d）→ 空手，标签现在=%d"),
		FormGEHandle.IsValid() ? 1 : 0,
		ASC->HasMatchingGameplayTag(LOLGameplayTags::State_Form_Unarmed) ? 1 : 0);
}

void UGA_FormSwitch::EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled)
{
	// 摘重入门。montage 被打断时 OnMontageFinished 可能没走到（EndAbility 先到），
	// 所以这里是【必须】的收尾点，不是「顺手摘一下」。
	// （移动恢复已随「切形态不锁移动」一起移除——这里不再碰 MovementComponent。）
	if (UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo())
	{
		ASC->RemoveLooseGameplayTag(LOLGameplayTags::State_Form_Switching);
	}

	// 拔刀亮剑标记兜底清。OnMontageFinished 里摘过的话这里是 no-op；
	// montage 被打断且回调没走到的极端路径靠这里保证剑不永久卡在强制可见。
	if (ActorInfo)
	{
		if (UHeroAnimInstance* HeroAnim = Cast<UHeroAnimInstance>(ActorInfo->GetAnimInstance()))
		{
			HeroAnim->SetSwordForceVisible(false);
		}
	}

	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}
