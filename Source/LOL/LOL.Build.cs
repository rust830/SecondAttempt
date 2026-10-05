// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class LOL : ModuleRules
{
	public LOL(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"AIModule",
			"StateTreeModule",
			"GameplayStateTreeModule",
			"UMG",
			"Slate",
			// SlateCore：UHeroHUDWidget.h 这个【公开头文件】用到了 SlateCore 的类型
			//（#include "Types/SlateEnums.h" + TEnumAsByte<EVerticalAlignment> 的 UPROPERTY）。
			// UHT 会为此生成对 Z_Construct_UEnum_SlateCore_EVerticalAlignment 的引用，
			// 而那个符号在 UnrealEditor-SlateCore.lib 里 —— 不在链接行上就是
			// LNK2019「无法解析的外部符号 ..._UEnum_SlateCore_EVerticalAlignment」。
			//
			// 【为什么不能靠 Slate 传递】Slate 的 PublicDependencyModuleNames 里确实有 SlateCore
			//（Slate.Build.cs:18-26），所以【头文件路径】能传过来（#include 编得过）；
			// 但【导入库】不会 —— 实测旧的 UnrealEditor-LOL.dll 只 import 了
			// UnrealEditor-Slate.dll，没有 SlateCore（dumpbin /imports）。
			// 结论：公开头文件用到哪个模块的类型，就显式把那个模块列进 Public，别赌传递。
			"SlateCore",
			"GameplayAbilities",
			"GameplayTags",
			"GameplayTasks",
			"Niagara"
        });

		// RenderCore：GC_Stealth.cpp 的诊断函数用了 Substrate::IsSubstrateEnabled()（RENDERCORE_API），
		// 只在 .cpp 里用，所以放私有依赖。
		//
		// AnimGraphRuntime：HeroAnimInstance.cpp 的方向计算用了 UKismetAnimationLibrary
		//（CalculateDirection）—— 5.8 里它在这个模块，不在 Kismet。
		PrivateDependencyModuleNames.AddRange(new string[] { "RenderCore", "AnimGraphRuntime" });

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });

		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
