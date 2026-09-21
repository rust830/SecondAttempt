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
			"GameplayAbilities",
			"GameplayTags",
			"GameplayTasks",
			"Niagara"
        });

		// RenderCore：GC_Stealth.cpp 的诊断函数用了 Substrate::IsSubstrateEnabled()（RENDERCORE_API），
		// 只在 .cpp 里用，所以放私有依赖。
		PrivateDependencyModuleNames.AddRange(new string[] { "RenderCore" });

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });

		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
