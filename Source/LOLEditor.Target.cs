// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.Collections.Generic;

public class LOLEditorTarget : TargetRules
{
	public LOLEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V7;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;
		ExtraModuleNames.Add("LOL");

		// ⚠️ Live Coding 别乱关：Win64 Development 的编辑器目标默认 `bWithLiveCoding = true`，
		// 而 LOLEditor 和引擎的 UnrealEditor 目标是【共用构建产物】的，UBT 会校验两边
		// 的目标规则属性必须一致 —— 单独把它改成 false 会直接
		//   "LOLEditor modifies the values of properties: [ bWithLiveCoding: False != True ]"
		// 构建在 CreateMakefiles 阶段就抛 BuildException（还没编译、更没链接）。
		// 真要关就得同时 `bOverrideBuildEnvironment = true`，那会把整个目标拎到独立构建环境里，
		// 等于全量重建一遍 —— 代价太大，别走。
		//
		// 副作用（已知、且是 UBT 的既定行为）：开着 LC 时 UBT 只链【补丁 DLL】
		// （Binaries/Win64/UnrealEditor-LOL-000X.dll），主 DLL 会被判"已最新"跳过。
		// ⇒ 构建完别看 Result: Succeeded，要看主 DLL 的 mtime/体积有没有变。
	}
}
