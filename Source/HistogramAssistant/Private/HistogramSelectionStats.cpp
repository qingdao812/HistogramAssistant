#include "HistogramSelectionStats.h"

#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Serialization/ArchiveCountMem.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/LightComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/MapBuildDataRegistry.h"
#include "StaticMeshComponentLODInfo.h"
#include "StaticMeshResources.h"
#include "SceneInterface.h"
#include "LightMap.h"
#include "ProfilingDebugging/ResourceSize.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstance.h"
#include "Engine/Texture.h"
#include "Misc/PackageName.h"

namespace
{
	/** 一张贴图的统计（给提示列表排序用） */
	struct FMeshTextureEntry
	{
		FString Name;
		FString Owner;			// 来自哪个材质槽
		FString Param;			// 是哪个贴图参数（"AO"/"color"…；空 = 材质里固定的贴图）
		FString Folder;			// 贴图所在目录
		bool bForeign = false;	// true = 不在网格资产自己的目录里（共用母材质/引擎默认带来的）
		int32 Width = 0;
		int32 Height = 0;
		double KB = 0.0;

		int64 Area() const { return (int64)Width * Height; }
	};

	/**
	 * 材质贴图统计：把**组件实际使用的材质**（含 Actor/蓝图 覆盖）里
	 * **材质真正采样到的**贴图收集起来，找最大的一张 + 去重资源占用。
	 *
	 * 两个都踩过的坑：
	 * 1) 材质来源：必须用 `Component->GetMaterial(槽)`，不是 `Mesh->GetStaticMaterials()`——
	 *    后者是资产自带的槽，关卡/蓝图里覆盖成别的材质时，资产自带那个可能引着不相干的贴图。
	 * 2) 贴图口径：`GetReferencedTexturesAndOverrides()` 是"引用到的所有贴图 ∪ 覆盖"的**并集**，
	 *    会把没走到的分支、父材质里被实例覆盖掉的默认贴图也算进来（于是 8K 假象）；
	 *    `GetUsedTextures()` 走编译后的 uniform 贴图表达式、按材质实例求值 = 真正会采样的贴图。
	 *    它返回空（材质没编译出资源）时才退回并集版，并在 UI 上标 ⚠。
	 *
	 * 注意：这个调用不便宜，所以只对明细表里真正显示的那几行做。
	 */
	void GatherMeshTextureInfo(FMeshUsageStat& Stat)
	{
		Stat.bTextureScanned = true;

		TMap<UTexture*, FMeshTextureEntry> Entries;

		for (const FMeshMaterialRef& MaterialRef : Stat.Materials)
		{
			UMaterialInterface* Material = MaterialRef.Material.Get();
			if (!Material)
			{
				continue;
			}

			// 注意：bAllFeatureLevels = true 时，传进去的 FeatureLevel 参数会被忽略
			// （引擎内部只在 !bAllFeatureLevels 时才读它，那时才会用到 GMaxRHIFeatureLevel）；
			// 这里传 ERHIFeatureLevel::Num 是为了不引用 GMaxRHIFeatureLevel——那个符号在 RHI 模块里，
			// 本插件不依赖 RHI，直接引用会链接失败（LNK2019）。
			TArray<UTexture*> UsedTextures;
			Material->GetUsedTextures(UsedTextures, EMaterialQualityLevel::Num, /*bAllQualityLevels=*/true,
				ERHIFeatureLevel::Num, /*bAllFeatureLevels=*/true);

			if (UsedTextures.Num() == 0)
			{
				// 兜底：材质没有可用的编译资源
				TSet<const UTexture*> Referenced;
				Material->GetReferencedTexturesAndOverrides(Referenced);
				for (const UTexture* ReferencedTexture : Referenced)
				{
					if (ReferencedTexture)
					{
						UsedTextures.Add(const_cast<UTexture*>(ReferencedTexture));
					}
				}

				if (UsedTextures.Num() > 0)
				{
					Stat.bTextureFallback = true;
				}
			}

			const FString OwnerName = FString::Printf(TEXT("%s[槽%d%s]"),
				*Material->GetName(),
				MaterialRef.SlotIndex,
				MaterialRef.bOverridden ? TEXT("·组件覆盖") : TEXT(""));

			// 贴图 → 参数名（"color"/"AO"/"chucao"…）：
			// 一个贴图可能被多个参数引用，拼起来；有参数名才能一眼看出"这张 8K 是挂在哪个槽上的"
			TMap<UTexture*, FString> ParameterNames;
			{
				TArray<FMaterialParameterInfo> ParameterInfos;
				TArray<FGuid> ParameterIds;
				Material->GetAllTextureParameterInfo(ParameterInfos, ParameterIds);

				for (const FMaterialParameterInfo& ParameterInfo : ParameterInfos)
				{
					UTexture* ParameterTexture = nullptr;
					if (Material->GetTextureParameterValue(FHashedMaterialParameterInfo(ParameterInfo), ParameterTexture)
						&& ParameterTexture)
					{
						FString& Names = ParameterNames.FindOrAdd(ParameterTexture);
						if (!Names.IsEmpty())
						{
							Names += TEXT("/");
						}
						Names += ParameterInfo.Name.ToString();
					}
				}
			}

			for (UTexture* Texture : UsedTextures)
			{
				if (!Texture)
				{
					continue;
				}

				FMeshTextureEntry& Entry = Entries.FindOrAdd(Texture);
				Entry.Name = Texture->GetName();
				if (Entry.Owner.IsEmpty())
				{
					Entry.Owner = OwnerName;
				}
				if (const FString* ParamName = ParameterNames.Find(Texture))
				{
					Entry.Param = *ParamName;
				}
			}
		}

		Stat.TextureCount = Entries.Num();

		TArray<FMeshTextureEntry> Sorted;
		Sorted.Reserve(Entries.Num());

		for (const TPair<UTexture*, FMeshTextureEntry>& Pair : Entries)
		{
			FMeshTextureEntry Entry = Pair.Value;
			Entry.Width = FMath::RoundToInt(Pair.Key->GetSurfaceWidth());
			Entry.Height = FMath::RoundToInt(Pair.Key->GetSurfaceHeight());
			Entry.KB = (double)Pair.Key->GetResourceSizeBytes(EResourceSizeMode::EstimatedTotal) / 1024.0;
			Entry.Folder = FPackageName::GetLongPackagePath(Pair.Key->GetOutermost()->GetName());

			// 「自己的贴图」= 和网格资产同一个目录（含子目录）
			Entry.bForeign = !(Entry.Folder == Stat.MeshFolder
				|| Entry.Folder.StartsWith(Stat.MeshFolder + TEXT("/")));

			if (Entry.bForeign)
			{
				++Stat.TextureForeignCount;
			}
			else if (Entry.Area() > (int64)Stat.TextureLocalW * Stat.TextureLocalH)
			{
				Stat.TextureLocalW = Entry.Width;
				Stat.TextureLocalH = Entry.Height;
				Stat.TextureLocalName = Entry.Name;
			}

			Stat.TextureKB += Entry.KB;
			Sorted.Add(MoveTemp(Entry));
		}

		// 大的排前面（提示里按这个顺序列）
		Sorted.Sort([](const FMeshTextureEntry& A, const FMeshTextureEntry& B)
		{
			return A.Area() > B.Area();
		});

		if (Sorted.Num() > 0)
		{
			Stat.TextureW = Sorted[0].Width;
			Stat.TextureH = Sorted[0].Height;
			Stat.TextureMaxName = Sorted[0].Name;
			Stat.TextureMaxOwner = Sorted[0].Owner;
		}

		// 全部列出（按尺寸降序）：提示里只显示前几条，写日志诊断时全都要
		for (const FMeshTextureEntry& Entry : Sorted)
		{
			const FString ParamText = Entry.Param.IsEmpty()
				? FString(TEXT("材质里固定的贴图"))
				: FString::Printf(TEXT("参数 %s"), *Entry.Param);

			const FString ForeignMark = Entry.bForeign
				? FString::Printf(TEXT("  ⚠不在本资产目录：%s"), *Entry.Folder)
				: FString(TEXT(""));

			Stat.TextureTopList.Add(FString::Printf(TEXT("%d×%d  %s  ← %s ｜ %s%s  （%.0f KB）"),
				Entry.Width, Entry.Height, *Entry.Name, *ParamText, *Entry.Owner, *ForeignMark, Entry.KB));
		}

		// 材质一览（诊断用）
		for (const FMeshMaterialRef& MaterialRef : Stat.Materials)
		{
			const UMaterialInterface* Material = MaterialRef.Material.Get();
			if (!Material)
			{
				continue;
			}

			FString ParentText;
			if (const UMaterialInstance* MaterialInstance = Cast<UMaterialInstance>(Material))
			{
				ParentText = MaterialInstance->Parent
					? FString::Printf(TEXT("，父材质 %s"), *MaterialInstance->Parent->GetName())
					: FString(TEXT("，无父材质"));
			}

			Stat.MaterialInfoList.Add(FString::Printf(TEXT("%s[槽%d]（%s·%s%s）位于 %s"),
				*Material->GetName(),
				MaterialRef.SlotIndex,
				*Material->GetClass()->GetName(),
				MaterialRef.bOverridden ? TEXT("组件覆盖") : TEXT("网格自带"),
				*ParentText,
				*Material->GetPathName()));
		}
	}
}

void FHistogramSelectionStats::Compute(FSelectionStats& Out, int32 MaxTopMeshes)
{
	Out = FSelectionStats();

	if (!GEditor)
	{
		return;
	}

	// 收集组件：选中 Actor 的静态网格组件 + 直接选中的组件（TSet 去重）
	TSet<UStaticMeshComponent*> Components;

	if (USelection* SelectedActors = GEditor->GetSelectedActors())
	{
		for (int32 Index = 0; Index < SelectedActors->Num(); ++Index)
		{
			AActor* Actor = Cast<AActor>(SelectedActors->GetSelectedObject(Index));
			if (!Actor)
			{
				continue;
			}

			++Out.ActorCount;

			if (Out.ActorCount == 1)
			{
				FString ClassName = Actor->GetClass()->GetName();
				ClassName.RemoveFromEnd(TEXT("_C"));
				Out.Label = FString::Printf(TEXT("%s（%s）"), *Actor->GetActorLabel(), *ClassName);
			}

			TArray<UStaticMeshComponent*> ActorComponents;
			Actor->GetComponents<UStaticMeshComponent>(ActorComponents);
			for (UStaticMeshComponent* Component : ActorComponents)
			{
				if (Component)
				{
					Components.Add(Component);
				}
			}
		}
	}

	if (USelection* SelectedComponents = GEditor->GetSelectedComponents())
	{
		for (int32 Index = 0; Index < SelectedComponents->Num(); ++Index)
		{
			if (UStaticMeshComponent* Component = Cast<UStaticMeshComponent>(SelectedComponents->GetSelectedObject(Index)))
			{
				Components.Add(Component);
			}
		}
	}

	if (Out.ActorCount > 1)
	{
		Out.Label = FString::Printf(TEXT("%d 个 Actor"), Out.ActorCount);
	}

	if (Components.Num() == 0)
	{
		Out.bHasSelection = false;
		return;
	}

	TMap<UStaticMesh*, FMeshUsageStat> ByMesh;

	// LOD0 用到的材质槽（按网格缓存，几百个组件共用同一个网格时不重复算）
	TMap<UStaticMesh*, TArray<int32>> MeshSlotCache;
	auto GetLOD0MaterialSlots = [&MeshSlotCache](UStaticMesh* InMesh) -> TArray<int32>
	{
		if (const TArray<int32>* Cached = MeshSlotCache.Find(InMesh))
		{
			return *Cached;
		}

		TArray<int32> Slots;
		if (const FStaticMeshRenderData* RenderData = InMesh->GetRenderData())
		{
			if (RenderData->LODResources.Num() > 0)
			{
				for (const FStaticMeshSection& Section : RenderData->LODResources[0].Sections)
				{
					Slots.AddUnique(Section.MaterialIndex);
				}
			}
		}

		if (Slots.Num() == 0)
		{
			// 拿不到渲染数据（网格没加载完等）→ 退回全部材质槽
			for (int32 Index = 0; Index < InMesh->GetStaticMaterials().Num(); ++Index)
			{
				Slots.Add(Index);
			}
		}

		MeshSlotCache.Add(InMesh, Slots);
		return Slots;
	};

	for (UStaticMeshComponent* Component : Components)
	{
		UStaticMesh* Mesh = Component->GetStaticMesh();
		if (!Mesh)
		{
			continue;
		}

		// 硬件实例数（ISM 才有意义）
		int32 NumHWInstances = 1;
		if (UInstancedStaticMeshComponent* ISM = Cast<UInstancedStaticMeshComponent>(Component))
		{
			NumHWInstances = ISM->GetNumRenderInstances();
		}

		const bool bUnlit = Component->UsesOnlyUnlitMaterials();

		// ---- 光照相关（口径与引擎 PrimitiveStats 一致）----
		int32 LightsLMCount = 0;
		if (!bUnlit && Component->LODData.Num() > 0)
		{
			const FStaticMeshComponentLODInfo& LODInfo = Component->LODData[0];
			const FMeshMapBuildData* BuildData = Component->GetMeshMapBuildData(LODInfo);
			if (BuildData && BuildData->LightMap)
			{
				LightsLMCount = BuildData->LightMap->LightGuids.Num();
			}
		}

		int32 LightsOtherCount = 0;
		if (UWorld* World = Component->GetWorld())
		{
			if (World->Scene)
			{
				TArray<const ULightComponent*> RelevantLights;
				World->Scene->GetRelevantLights(Component, &RelevantLights);
				LightsOtherCount = (!bUnlit && RelevantLights.Num() >= LightsLMCount)
					? RelevantLights.Num() - LightsLMCount
					: 0;
			}
		}

		int32 LightMapWidth = 0;
		int32 LightMapHeight = 0;
		Component->GetLightMapResolution(LightMapWidth, LightMapHeight);
		const int32 LMSMResolution = FMath::TruncToInt32(FMath::Sqrt((float)(LightMapHeight * LightMapWidth)));

		int32 LightMapData = 0;
		int32 LegacyShadowMapData = 0;
		Component->GetLightAndShadowMapMemoryUsage(LightMapData, LegacyShadowMapData);

		// 实例顶点色内存
		int32 InstVertexColorMem = 0;
		for (const FStaticMeshComponentLODInfo& LODInfo : Component->LODData)
		{
			if (LODInfo.OverrideVertexColors)
			{
				InstVertexColorMem += LODInfo.OverrideVertexColors->GetAllocatedSize();
			}
		}

		const double Radius = Component->Bounds.SphereRadius;
		AActor* ActorOuter = Cast<AActor>(Component->GetOuter());

		FMeshUsageStat& Stat = ByMesh.FindOrAdd(Mesh);

		// 单件信息只算一次
		if (Stat.MeshName.IsEmpty())
		{
			Stat.MeshName = Mesh->GetName();
			Stat.MeshAssetPath = FSoftObjectPath(Mesh);
			Stat.MeshFolder = FPackageName::GetLongPackagePath(Mesh->GetOutermost()->GetName());

			if (const FStaticMeshRenderData* RenderData = Mesh->GetRenderData())
			{
				if (RenderData->LODResources.Num() > 0)
				{
					const FStaticMeshLODResources& LOD0 = RenderData->LODResources[0];
					for (const FStaticMeshSection& Section : LOD0.Sections)
					{
						Stat.Triangles += Section.NumTriangles;
						++Stat.Sections;
					}

					for (int32 LODIndex = 0; LODIndex < RenderData->LODResources.Num(); ++LODIndex)
					{
						Stat.VertexColorKB += (double)RenderData->LODResources[LODIndex].VertexBuffers.ColorVertexBuffer.GetAllocatedSize() / 1024.0;
					}
				}
			}

			Stat.ResourceKB = (double)(FArchiveCountMem(Mesh).GetNum() + (int64)Mesh->GetResourceSizeBytes(EResourceSizeMode::EstimatedTotal)) / 1024.0;
			Stat.RadiusMin = Radius;
			Stat.RadiusMax = Radius;
		}

		// 这个组件**实际使用**的材质（组件/Actor 覆盖优先；没有覆盖才落到网格自带材质槽）
		{
			const TArray<FStaticMaterial>& StaticMaterials = Mesh->GetStaticMaterials();

			for (int32 SlotIndex : GetLOD0MaterialSlots(Mesh))
			{
				UMaterialInterface* ComponentMaterial = Component->GetMaterial(SlotIndex);
				if (!ComponentMaterial)
				{
					continue;
				}

				const bool bOverridden = StaticMaterials.IsValidIndex(SlotIndex)
					&& StaticMaterials[SlotIndex].MaterialInterface != ComponentMaterial;

				bool bAlreadyAdded = false;
				for (const FMeshMaterialRef& Existing : Stat.Materials)
				{
					if (Existing.Material.Get() == ComponentMaterial)
					{
						bAlreadyAdded = true;
						break;
					}
				}

				if (!bAlreadyAdded)
				{
					FMeshMaterialRef MaterialRef;
					MaterialRef.Material = ComponentMaterial;
					MaterialRef.SlotIndex = SlotIndex;
					MaterialRef.bOverridden = bOverridden;
					Stat.Materials.Add(MaterialRef);
				}
			}
		}

		// ---- 按实例累加 ----
		++Stat.Count;
		Stat.HWInstances += NumHWInstances;
		Stat.InstTriangles += (int64)Stat.Triangles * NumHWInstances;
		Stat.InstSections += (int64)Stat.Sections * NumHWInstances;
		Stat.InstVertexColorKB += (double)InstVertexColorMem / 1024.0;
		Stat.LightsLM += LightsLMCount;
		Stat.LightsOther += LightsOtherCount;
		Stat.LightMapKB += (double)LightMapData / 1024.0;
		Stat.LMSMResolution += (double)LMSMResolution;

		// 光照图真实尺寸：留面积最大的那一张（实例之间可能不一样）
		if ((int64)LightMapWidth * LightMapHeight > (int64)Stat.LightMapW * Stat.LightMapH)
		{
			Stat.LightMapW = LightMapWidth;
			Stat.LightMapH = LightMapHeight;
		}
		Stat.RadiusMin = FMath::Min(Stat.RadiusMin, Radius);
		Stat.RadiusMax = FMath::Max(Stat.RadiusMax, Radius);
		Stat.RadiusSum += Radius;

		if (ActorOuter)
		{
			Stat.Actors.AddUnique(ActorOuter);

			// 蓝图资产：Actor 是蓝图生成的（ClassGeneratedBy = 蓝图资产）
			if (UBlueprintGeneratedClass* BlueprintClass = Cast<UBlueprintGeneratedClass>(ActorOuter->GetClass()))
			{
				if (UObject* BlueprintAsset = BlueprintClass->ClassGeneratedBy)
				{
					Stat.BlueprintAssetPaths.AddUnique(FSoftObjectPath(BlueprintAsset));
				}
			}
		}
	}

	// ---- 汇总（合计 / 平均）----
	Out.UniqueMeshCount = ByMesh.Num();

	double RadiusSumAll = 0.0;
	bool bFirstMesh = true;

	for (const TPair<UStaticMesh*, FMeshUsageStat>& Pair : ByMesh)
	{
		const FMeshUsageStat& Stat = Pair.Value;

		Out.ComponentCount += (int32)Stat.Count;
		Out.TotalTriangles += Stat.Triangles;
		Out.TotalInstTriangles += Stat.InstTriangles;
		Out.TotalSections += Stat.Sections;
		Out.TotalInstSections += Stat.InstSections;
		Out.TotalResourceKB += Stat.ResourceKB;
		Out.TotalLightMapKB += Stat.LightMapKB;
		Out.TotalVertexColorKB += Stat.VertexColorKB;
		Out.TotalInstVertexColorKB += Stat.InstVertexColorKB;

		Out.AvgLM += Stat.LightsLM;
		Out.AvgOL += Stat.LightsOther;
		Out.AvgCost += Stat.GetCost();
		Out.AvgRes += Stat.LMSMResolution;

		if ((int64)Stat.LightMapW * Stat.LightMapH > (int64)Out.LightMapMaxW * Out.LightMapMaxH)
		{
			Out.LightMapMaxW = Stat.LightMapW;
			Out.LightMapMaxH = Stat.LightMapH;
		}

		RadiusSumAll += Stat.RadiusSum;

		if (bFirstMesh)
		{
			Out.RadiusMin = Stat.RadiusMin;
			Out.RadiusMax = Stat.RadiusMax;
			bFirstMesh = false;
		}
		else
		{
			Out.RadiusMin = FMath::Min(Out.RadiusMin, Stat.RadiusMin);
			Out.RadiusMax = FMath::Max(Out.RadiusMax, Stat.RadiusMax);
		}
	}

	if (Out.ComponentCount > 0)
	{
		const double Inv = 1.0 / (double)Out.ComponentCount;
		Out.AvgLM *= Inv;
		Out.AvgOL *= Inv;
		Out.AvgRes *= Inv;
		Out.RadiusAvg = RadiusSumAll * Inv;
	}

	// ---- 明细：按合计三角面降序 ----
	ByMesh.ValueSort([](const FMeshUsageStat& A, const FMeshUsageStat& B)
	{
		return A.InstTriangles > B.InstTriangles;
	});

	const int32 WantCount = FMath::Clamp(MaxTopMeshes, 0, 16);
	Out.TopMeshes.Reserve(WantCount);

	int32 Taken = 0;
	for (const TPair<UStaticMesh*, FMeshUsageStat>& Pair : ByMesh)
	{
		if (Taken >= WantCount)
		{
			break;
		}

		FMeshUsageStat StatCopy = Pair.Value;

		// 材质贴图统计只给要显示的行做（解析材质不便宜，几百个网格会拖慢刷新）
		GatherMeshTextureInfo(StatCopy);

		if ((int64)StatCopy.TextureW * StatCopy.TextureH > (int64)Out.TextureMaxW * Out.TextureMaxH)
		{
			Out.TextureMaxW = StatCopy.TextureW;
			Out.TextureMaxH = StatCopy.TextureH;
		}
		Out.TextureKB += StatCopy.TextureKB;

		Out.TopMeshes.Add(MoveTemp(StatCopy));
		++Taken;
	}

	Out.bHasSelection = true;
}
