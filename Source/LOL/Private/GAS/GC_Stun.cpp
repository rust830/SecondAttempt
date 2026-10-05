// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GC_Stun.h"
#include "GAS/LOLGameplayTags.h"

AGC_Stun::AGC_Stun()
{
	// CDO 构造阶段字符串查标签拿不到（返回 None），必须直接用原生标签对象。
	GameplayCueTag = LOLGameplayTags::GameplayCue_Stun;
}
