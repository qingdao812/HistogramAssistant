// HistogramAssistant - 选中对象资源统计（对齐引擎 Primitive Stats 的字段口径）
//
// 解决的缺口：UE 自带工具里 Primitive Stats 是"逐组件"的、Asset Audit 是"逐资产"的，
// **"一个用几百个小模型拼出来的蓝图，整体多重"没有现成视图**。
//
// 字段来源全部照抄引擎实现（Editor/StatsViewer/Private/StatsPages/PrimitiveStatsPage.cpp），
// 这样面板里的数字能和 Primitive Stats 对上：
//   Tris/Sections : StaticMesh->GetRenderData()->LODResources[0].Sections[]（sum NumTriangles / 计数）
//   Size(KB)      : (FArchiveCountMem(Resource).GetNum() + GetResourceSizeBytes(EstimatedTotal)) / 1024
//   InstTris/InstSections : Triangles × HWInstances 累加
//   LM            : GetMeshMapBuildData(LODData[0])->LightMap->LightGuids.Num()
//   OL            : World->Scene->GetRelevantLights() 数量 − LM
//   LM(KB) / Res  : GetLightAndShadowMapMemoryUsage() / GetLightMapResolution() → sqrt(W*H)
//   Min/Max/Avg R : Component->Bounds.SphereRadius
//   VC / Inst VC  : 颜色顶点缓冲 / OverrideVertexColors 的分配大小

#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPath.h"

class AActor;
class UMaterialInterface;

/**
 * 组件**实际使用**的材质。
 * 关键：不能用网格资产自带材质槽（`Mesh->GetStaticMaterials()`）算贴图——
 * 关卡里/蓝图里经常把材质覆盖成另一套（`UPrimitiveComponent::OverrideMaterials`），
 * 这时资产自带的那个材质可能引着完全不相干的贴图（例如父材质的 8K 默认 AO），
 * 于是出现"明明是 1K 的模型，面板报 8K"。
 */
struct FMeshMaterialRef
{
	TWeakObjectPtr<UMaterialInterface> Material;
	int32 SlotIndex = 0;
	/** true = 来自组件/Actor 覆盖，不是网格资产自带的材质槽 */
	bool bOverridden = false;
};

/** 按唯一网格聚合的使用统计 */
struct FMeshUsageStat
{
	FString MeshName;

	/** 用到这个网格的组件实际使用的材质（统计贴图用；含覆盖） */
	TArray<FMeshMaterialRef> Materials;

	/** 静态网格资产路径：点击明细行时「浏览至资产」用 */
	FSoftObjectPath MeshAssetPath;

	/** 用到这个网格的 Actor 所属蓝图资产（实例化蓝图才有） */
	TArray<FSoftObjectPath> BlueprintAssetPaths;

	/** 用到它的 Actor（Ctrl+点击明细行时选中并聚焦） */
	TArray<TWeakObjectPtr<AActor>> Actors;

	int64 Count = 0;			// 组件数（= Primitive Stats 的 Count）
	int64 HWInstances = 0;		// 硬件实例数（ISM 才有意义，否则 = Count）
	int32 Triangles = 0;		// 单件三角面
	int64 InstTriangles = 0;	// 合计三角面（Sum Tris，含实例）
	int32 Sections = 0;			// 单件材质槽
	int64 InstSections = 0;		// 合计材质槽（含实例）
	double ResourceKB = 0.0;	// 单件资源大小 KB
	double VertexColorKB = 0.0;
	double InstVertexColorKB = 0.0;

	// 光照相关：按实例累加，显示时取平均/求和
	double LightsLM = 0.0;
	double LightsOther = 0.0;
	double LightMapKB = 0.0;
	double LMSMResolution = 0.0;

	/**
	 * 光照图（Lightmap）**真实尺寸**（像素）：取该网格所有实例里面积最大的那一张。
	 * 引擎 Primitive Stats 的 Res 列只给 sqrt(W×H) 一个数（见 LMSMResolution），
	 * 光看一个 12 根本不知道贴图多大，所以这里额外记 W×H 原样显示。
	 */
	int32 LightMapW = 0;
	int32 LightMapH = 0;

	// 材质贴图（只对明细表里真正显示的那几行做统计：解析材质不便宜）
	int32 TextureW = 0;			// 材质里用到的最大的贴图宽
	int32 TextureH = 0;			// 材质里用到的最大的贴图高
	int32 TextureCount = 0;		// 材质引用到的贴图张数（去重）
	double TextureKB = 0.0;		// 这些贴图的资源占用合计（去重，KB）
	/** 最大贴图的资产名 + 它来自哪个材质槽（放列里/提示里，方便核对是不是真的这张） */
	FString TextureMaxName;
	FString TextureMaxOwner;
	/** 全部用到的贴图明细（按尺寸降序，"8192×8192  tj4mcccla_8K_AO  ← 参数 AO ｜ M_tizi_Inst[槽0]"） */
	TArray<FString> TextureTopList;
	/** 用到的材质一览（"M_tizi_Inst（MaterialInstanceConstant·组件覆盖）→ 父材质 M_SHIZHUAN"），写日志诊断用 */
	TArray<FString> MaterialInfoList;

	/** 网格资产所在目录（用来判断贴图是不是"这个资产自己的"） */
	FString MeshFolder;
	/** 只算"跟网格同目录"的贴图，取最大的一张 */
	int32 TextureLocalW = 0;
	int32 TextureLocalH = 0;
	FString TextureLocalName;
	/** 有几张贴图来自其他目录 / 引擎默认（多半是共用母材质留下的，比如 AO） */
	int32 TextureForeignCount = 0;
	/** true = 材质没编译出资源，拿不到"实际采样"的贴图，退回"图里引用的"，结果可能偏大 */
	bool bTextureFallback = false;
	/** 是否做过材质贴图统计（没做过就显示 "-"，别把 0 当成"没有贴图"） */
	bool bTextureScanned = false;

	// 包围球（按实例）
	double RadiusMin = 0.0;
	double RadiusMax = 0.0;
	double RadiusSum = 0.0;

	double GetAvgLM() const { return Count > 0 ? LightsLM / (double)Count : 0.0; }
	double GetAvgOL() const { return Count > 0 ? LightsOther / (double)Count : 0.0; }
	double GetCost() const { return GetAvgOL() * (double)Sections; }	// 与引擎一致：Avg OL × Sections
	double GetAvgRes() const { return Count > 0 ? LMSMResolution / (double)Count : 0.0; }
	double GetAvgRadius() const { return Count > 0 ? RadiusSum / (double)Count : 0.0; }
};

/** 当前选中对象的汇总 */
struct FSelectionStats
{
	bool bHasSelection = false;
	FString Label;
	TArray<TWeakObjectPtr<AActor>> Actors;

	int32 ActorCount = 0;
	int32 ComponentCount = 0;
	int32 UniqueMeshCount = 0;

	int64 TotalTriangles = 0;		// 单件三角面之和（按唯一网格）
	int64 TotalInstTriangles = 0;	// 合计三角面（含实例）
	int64 TotalSections = 0;
	int64 TotalInstSections = 0;
	double TotalResourceKB = 0.0;
	double TotalLightMapKB = 0.0;
	double TotalVertexColorKB = 0.0;
	double TotalInstVertexColorKB = 0.0;

	double AvgLM = 0.0;
	double AvgOL = 0.0;
	double AvgCost = 0.0;
	double AvgRes = 0.0;
	double RadiusMin = 0.0;
	double RadiusMax = 0.0;
	double RadiusAvg = 0.0;

	/** 光照图最大尺寸（像素，跨所有网格取面积最大的一张） */
	int32 LightMapMaxW = 0;
	int32 LightMapMaxH = 0;
	/** 材质贴图最大尺寸（像素，只统计明细表里显示的那几行） */
	int32 TextureMaxW = 0;
	int32 TextureMaxH = 0;
	double TextureKB = 0.0;

	TArray<FMeshUsageStat> TopMeshes;	// 按合计三角面降序
};

class FHistogramSelectionStats
{
public:
	/** 统计当前编辑器选中内容（选中 Actor 的静态网格组件 + 直接选中的静态网格组件） */
	static void Compute(FSelectionStats& Out, int32 MaxTopMeshes = 5);
};
