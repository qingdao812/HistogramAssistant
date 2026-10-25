#include "HistogramPanel.h"

#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "HistogramExport.h"
#include "HistogramZebra.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Application/SlateApplication.h"
#include "Engine/Selection.h"
#include "Editor.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Images/SImage.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Rendering/DrawElements.h"
#include "RHI.h"
#include "IAssetViewport.h"
#include "LevelEditor.h"
#include "Modules/ModuleManager.h"

#define LOCTEXT_NAMESPACE "HistogramAssistant"

namespace
{
	FString FormatBytesShort(int64 Bytes)
	{
		if (Bytes >= 1024ll * 1024ll)
		{
			return FString::Printf(TEXT("%.1f MB"), (double)Bytes / 1024.0 / 1024.0);
		}
		if (Bytes >= 1024ll)
		{
			return FString::Printf(TEXT("%.1f KB"), (double)Bytes / 1024.0);
		}
		return FString::Printf(TEXT("%lld B"), Bytes);
	}

	// -----------------------------------------------------------------------
	// 「选中对象资源统计」明细表：列定义（表头全中文 + 鼠标提示）
	// 列顺序必须与 BuildMeshCells() 里填的单元格顺序一致。
	// 宽度单位 = 像素（8pt 等宽字体下的经验值）；单元格自己定宽，
	// 所以中文字头不会把数字挤歪（用空格对齐做不到这点）。
	// -----------------------------------------------------------------------
	struct FMeshTableColumn
	{
		const TCHAR* Label;
		const TCHAR* Tip;
		float Width;
		bool bLeftAlign;
	};

	const FMeshTableColumn GMeshTableColumns[] =
	{
		{ TEXT("网格"),			TEXT("静态网格资产名（同一个网格用多少次都只算一行；点这一行＝在内容浏览器里浏览至这个资产，Ctrl+点击＝在视口里选中并聚焦用到它的 Actor）"),	170.f, true },
		{ TEXT("数量"),			TEXT("用到这个网格的组件数量"),													44.f, false },
		{ TEXT("硬件实例"),		TEXT("硬件实例数合计（普通静态网格组件算 1，实例化网格组件算它的实例数）"),			64.f, false },
		{ TEXT("三角面"),		TEXT("单个网格的三角面数（取 LOD0 各材质段的三角面之和）"),							64.f, false },
		{ TEXT("合计三角面"),	TEXT("三角面 × 数量（就是自带 Primitive Stats 里的 Sum Tris，含实例）"),				88.f, false },
		{ TEXT("材质槽"),		TEXT("单个网格的材质槽数量（≈ 需要多少次 draw call）"),								56.f, false },
		{ TEXT("合计槽位"),		TEXT("材质槽 × 数量"),																72.f, false },
		{ TEXT("贴图最大"),		TEXT("这个网格的材质【实际采样到】的最大贴图尺寸（宽×高，单位像素）。\n材质取自【组件实际使用的材质】（关卡/蓝图里覆盖过的优先），不是网格资产自带材质槽里的那个。\n贴图口径 = 材质编译后的 uniform 贴图（按材质实例解析过覆盖），不是「材质图里引用到的所有贴图」；\n⚠ 这个数可能来自**别的目录**（共用母材质/导入器留下的贴图，比如某张 8K 的 AO），\n想知道它是不是这个资产自己的，看右边「本资产贴图」列或鼠标悬停"),	84.f, false },
		{ TEXT("本资产贴图"),	TEXT("只统计【和这个网格资产在同一个目录（含子目录）】的贴图，取最大的一张。\n用它来区分「这个资产自己的贴图」和「共用母材质 / 导入器带进来的外部贴图」。\n两者差很多时（比如这里 1K、左边 8K），就是外部贴图在占内存"),	84.f, false },
		{ TEXT("最大贴图名"),	TEXT("左边「贴图最大」那张贴图的资产名 —— 拿这个名字去内容浏览器搜一下，就能核对面板有没有算错。\n鼠标停在整行上会列出全部贴图（尺寸/参数名/来自哪个材质/是否本资产目录），\n点「贴图诊断→日志」可把全量明细写进日志"),	170.f, true },
		{ TEXT("贴图 KB"),		TEXT("这个网格的材质实际采样到的贴图占用的内存 KB（去重后合计）。\n⚠ 两个注意：\n① 贴图常被很多网格共用，加起来会比真实总显存大——按贴图统计要看 Window → Statistics → Texture Stats；\n② 贴图如果还没被流送加载进来，这里只有几十 KB 甚至 0（不代表它真的小，8K 贴图加载后是几十 MB）"),	64.f, false },
		{ TEXT("网格 KB"),		TEXT("网格几何本身占的内存 KB（不含材质贴图；按唯一网格只算一次，重复使用不重复计）"),		64.f, false },
		{ TEXT("顶点色 KB"),	TEXT("网格顶点色缓冲大小 KB（单件）"),												72.f, false },
		{ TEXT("实例顶点色 KB"),TEXT("组件上覆盖的顶点色合计大小 KB（按实例累加）"),									96.f, false },
		{ TEXT("光照图尺寸"),	TEXT("光照贴图（Lightmap）的真实像素尺寸 宽×高。\n取这个网格所有实例里面积最大的一张（实例之间可能不一样）。\n这就是回答“光照图分辨率那列写着 12，到底是多大”的地方"),	96.f, false },
		{ TEXT("光照图 KB"),	TEXT("光照贴图占用的内存 KB"),														72.f, false },
		{ TEXT("均·光照图分辨率"),TEXT("光照贴图分辨率的平均值，单位不是宽×高，而是 √(宽×高)（面积开平方）——\n这是引擎自带 Primitive Stats 里 Res 列的口径，为了让矩形贴图只用一个数字表示；\n比如 128×128 → 128，64×256 → 也是 128。所以看到 12 就是 12×12 那种很小的贴图，真实尺寸看左边「光照图尺寸」列"),	110.f, false },
		{ TEXT("光照图光源"),	TEXT("光照贴图里影响这个网格的光源数（所有实例合计）"),								80.f, false },
		{ TEXT("动态光源"),		TEXT("影响这个网格的其他（动态）光源数（所有实例合计）"),							72.f, false },
		{ TEXT("均·光照图光源"),TEXT("平均每个组件被几个光照贴图光源照到（Primitive Stats 里的 Avg LM）"),				88.f, false },
		{ TEXT("均·动态光源"),	TEXT("平均每个组件被几个动态光源照到（Primitive Stats 里的 Avg OL）"),					80.f, false },
		{ TEXT("成本"),			TEXT("渲染成本估算 = 平均动态光源数 × 材质槽数（越大越贵，用来挑最该优化的网格）"),		52.f, false },
		{ TEXT("最小半径"),		TEXT("包围球半径的最小值，单位厘米（cm）。\n包围球 = 把这个组件的整个包围盒刚刚好裹住的那个球，\n半径 = 球心到最远角的距离；它衡量的是“这个物体在场景里有多大”。\n看最小/最大能知道选中的这批东西尺寸差多少"),	72.f, false },
		{ TEXT("最大半径"),		TEXT("包围球半径的最大值，单位厘米（cm）。\n半径突然很大 = 这个网格实例被缩放放大了，或者它是个大件（墙/地面/整体模型）"),	72.f, false },
		{ TEXT("平均半径"),		TEXT("包围球半径的平均值，单位厘米（cm）。\n100 cm = 1 米；用它大致判断物体尺度（人 ≈ 90，小道具 ≈ 10~30）"),	72.f, false },
	};

	const int32 GMeshTableColumnCount = UE_ARRAY_COUNT(GMeshTableColumns);

	/** 宽×高 文本；尺寸拿不到时给个占位符，别显示 0×0 */
	FString FormatSizeWH(int32 Width, int32 Height, const TCHAR* EmptyText)
	{
		if (Width <= 0 || Height <= 0)
		{
			return FString(EmptyText);
		}
		return FString::Printf(TEXT("%d×%d"), Width, Height);
	}

	/** 按列顺序把一条网格统计填成单元格文本（列顺序必须与 GMeshTableColumns 一致） */
	void BuildMeshCells(const FMeshUsageStat& Mesh, TArray<FString>& Out)
	{
		Out.Reset();
		Out.Reserve(GMeshTableColumnCount);

		Out.Add(Mesh.MeshName);														// 网格
		Out.Add(FString::Printf(TEXT("%lld"), Mesh.Count));							// 数量
		Out.Add(FString::Printf(TEXT("%lld"), Mesh.HWInstances));					// 硬件实例
		Out.Add(FString::Printf(TEXT("%d"), Mesh.Triangles));						// 三角面
		Out.Add(FString::Printf(TEXT("%lld"), Mesh.InstTriangles));					// 合计三角面
		Out.Add(FString::Printf(TEXT("%d"), Mesh.Sections));						// 材质槽
		Out.Add(FString::Printf(TEXT("%lld"), Mesh.InstSections));					// 合计槽位
		Out.Add(Mesh.bTextureScanned												// 贴图最大
			? FormatSizeWH(Mesh.TextureW, Mesh.TextureH, TEXT("无贴图"))
			: FString(TEXT("-")));
		Out.Add(Mesh.bTextureScanned												// 本资产贴图
			? FormatSizeWH(Mesh.TextureLocalW, Mesh.TextureLocalH, TEXT("无"))
			: FString(TEXT("-")));
		Out.Add(Mesh.bTextureScanned && !Mesh.TextureMaxName.IsEmpty()				// 最大贴图名
			? Mesh.TextureMaxName
			: FString(Mesh.bTextureScanned ? TEXT("无贴图") : TEXT("-")));
		Out.Add(Mesh.bTextureScanned												// 贴图 KB
			? FString::Printf(TEXT("%.0f"), Mesh.TextureKB)
			: FString(TEXT("-")));
		Out.Add(FString::Printf(TEXT("%.0f"), Mesh.ResourceKB));					// 网格 KB
		Out.Add(FString::Printf(TEXT("%.0f"), Mesh.VertexColorKB));					// 顶点色 KB
		Out.Add(FString::Printf(TEXT("%.0f"), Mesh.InstVertexColorKB));				// 实例顶点色 KB
		Out.Add(FormatSizeWH(Mesh.LightMapW, Mesh.LightMapH, TEXT("无")));			// 光照图尺寸
		Out.Add(FString::Printf(TEXT("%.0f"), Mesh.LightMapKB));					// 光照图 KB
		Out.Add(FString::Printf(TEXT("%.0f"), Mesh.LMSMResolution));				// 均·光照图分辨率
		Out.Add(FString::Printf(TEXT("%.1f"), Mesh.LightsLM));						// 光照图光源
		Out.Add(FString::Printf(TEXT("%.1f"), Mesh.LightsOther));					// 动态光源
		Out.Add(FString::Printf(TEXT("%.1f"), Mesh.GetAvgLM()));					// 均·光照图光源
		Out.Add(FString::Printf(TEXT("%.1f"), Mesh.GetAvgOL()));					// 均·动态光源
		Out.Add(FString::Printf(TEXT("%.1f"), Mesh.GetCost()));						// 成本
		Out.Add(FString::Printf(TEXT("%.1f"), Mesh.RadiusMin));						// 最小半径
		Out.Add(FString::Printf(TEXT("%.1f"), Mesh.RadiusMax));						// 最大半径
		Out.Add(FString::Printf(TEXT("%.1f"), Mesh.GetAvgRadius()));				// 平均半径
	}
}

// ---------------------------------------------------------------------------
// SHistogramGraph
// ---------------------------------------------------------------------------

void SHistogramGraph::Construct(const FArguments& InArgs)
{
}

void SHistogramGraph::SetData(const FHistogramData& InData)
{
	Data = InData;
	bHasData = true;
	Invalidate(EInvalidateWidgetReason::Paint);
}

void SHistogramGraph::SetViewMode(EHistogramViewMode InMode)
{
	if (ViewMode != InMode)
	{
		ViewMode = InMode;
		Invalidate(EInvalidateWidgetReason::Paint);
	}
}

FVector2D SHistogramGraph::ComputeDesiredSize(float LayoutScaleMultiplier) const
{
	return FVector2D(380.f, 240.f);
}

int32 SHistogramGraph::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const FVector2D LocalSize = AllottedGeometry.GetLocalSize();
	const float Pad = 8.f;
	const float GraphW = FMath::Max(10.f, LocalSize.X - Pad * 2.f);
	const float GraphH = FMath::Max(10.f, LocalSize.Y - Pad * 2.f);

	// 背景
	FSlateDrawElement::MakeBox(
		OutDrawElements,
		LayerId,
		AllottedGeometry.ToPaintGeometry(),
		FAppStyle::GetBrush("WhiteBrush"),
		ESlateDrawEffect::None,
		FLinearColor(0.015f, 0.015f, 0.02f, 1.f));
	++LayerId;

	// 网格
	TArray<FVector2D> Line;
	for (int32 g = 0; g <= 4; ++g)
	{
		const float Y = Pad + GraphH * g / 4.f;
		Line.Reset();
		Line.Add(FVector2D(Pad, Y));
		Line.Add(FVector2D(Pad + GraphW, Y));
		FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(), Line, ESlateDrawEffect::None, FLinearColor(1.f, 1.f, 1.f, 0.08f), false, 1.f);
	}

	if (!bHasData)
	{
		++LayerId;
		return LayerId;
	}

	// 单独通道的最大值取对数归一化，避免 RGB 曲线互相压扁
	auto MaxLogOf = [](const TArray<int32>& Hist)
	{
		int32 MaxCount = 1;
		for (int32 v : Hist)
		{
			MaxCount = FMath::Max(MaxCount, v);
		}
		return FMath::Max(1.0, FMath::Loge(1.0 + (double)MaxCount));
	};

	auto BuildCurveInRect = [&](const TArray<int32>& Hist, const FLinearColor& Color, float Thickness, float Top, float Bottom, double InMaxLog)
	{
		TArray<FVector2D> Points;
		const int32 N = FMath::Min(Hist.Num(), 256);
		Points.Reserve(N);
		for (int32 i = 0; i < N; ++i)
		{
			const double V = FMath::Loge(1.0 + (double)Hist[i]) / InMaxLog;
			const float X = Pad + GraphW * i / 255.f;
			const float Y = Top + (Bottom - Top) * (1.f - (float)V);
			Points.Add(FVector2D(X, Y));
		}
		FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(), Points, ESlateDrawEffect::None, Color, true, Thickness);
	};

	const FLinearColor RColor(1.f, 0.25f, 0.25f);
	const FLinearColor GColor(0.3f, 1.f, 0.3f);
	const FLinearColor BColor(0.35f, 0.55f, 1.f);
	const FLinearColor LumaColor(1.f, 1.f, 1.f);

	if (ViewMode == EHistogramViewMode::LumaOnly)
	{
		// 亮度：只看明暗
		const double MaxLog = MaxLogOf(Data.LumaHist);
		BuildCurveInRect(Data.LumaHist, LumaColor, 1.8f, Pad, Pad + GraphH, MaxLog);
		FSlateDrawElement::MakeText(
			OutDrawElements,
			LayerId,
			AllottedGeometry.ToPaintGeometry(FVector2f(Pad, Pad), FSlateLayoutTransform(FVector2f(90.f, 14.f))),
			LOCTEXT("LumaOnlyLabel", "亮度 Luma"),
			FCoreStyle::GetDefaultFontStyle("Bold", 9),
			ESlateDrawEffect::None,
			FLinearColor(1.f, 1.f, 1.f, 0.7f));
	}
	else if (ViewMode == EHistogramViewMode::SplitRGB)
	{
		// 独立 R / G / B：三行，各自归一化
		const float Gap = 6.f;
		const float BandH = (GraphH - Gap * 2.f) / 3.f;
		const TArray<int32>* Hists[3] = { &Data.RHist, &Data.GHist, &Data.BHist };
		const FLinearColor Colors[3] = { RColor, GColor, BColor };
		const FText Labels[3] = { LOCTEXT("ChannelR", "R"), LOCTEXT("ChannelG", "G"), LOCTEXT("ChannelB", "B") };

		for (int32 c = 0; c < 3; ++c)
		{
			const float Top = Pad + c * (BandH + Gap);
			const float Bottom = Top + BandH;
			BuildCurveInRect(*Hists[c], Colors[c], 1.4f, Top, Bottom, MaxLogOf(*Hists[c]));
			FSlateDrawElement::MakeText(
				OutDrawElements,
				LayerId,
				AllottedGeometry.ToPaintGeometry(FVector2f(Pad, Top), FSlateLayoutTransform(FVector2f(20.f, 14.f))),
				Labels[c],
				FCoreStyle::GetDefaultFontStyle("Bold", 10),
				ESlateDrawEffect::None,
				Colors[c]);
		}
	}
	else
	{
		// 综合：R / G / B / 亮度叠加
		const double MaxLog = FMath::Max(1.0, FMath::Loge(1.0 + (double)Data.MaxHistCount));
		BuildCurveInRect(Data.RHist, RColor, 1.2f, Pad, Pad + GraphH, MaxLog);
		BuildCurveInRect(Data.GHist, GColor, 1.2f, Pad, Pad + GraphH, MaxLog);
		BuildCurveInRect(Data.BHist, BColor, 1.2f, Pad, Pad + GraphH, MaxLog);
		BuildCurveInRect(Data.LumaHist, LumaColor, 1.8f, Pad, Pad + GraphH, MaxLog);
	}

	// 平均亮度参考线（独立 RGB 模式没有亮度概念，不画）
	if (ViewMode != EHistogramViewMode::SplitRGB)
	{
		const float AvgX = Pad + GraphW * FMath::Clamp(Data.AvgLuma / 255.f, 0.f, 1.f);
		Line.Reset();
		Line.Add(FVector2D(AvgX, Pad));
		Line.Add(FVector2D(AvgX, Pad + GraphH));
		FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(), Line, ESlateDrawEffect::None, FLinearColor(1.f, 0.9f, 0.2f, 0.7f), false, 1.5f);
	}

	// 横轴两端标注：左=暗，右=亮
	{
		const FSlateFontInfo AxisFont = FCoreStyle::GetDefaultFontStyle("Regular", 9);
		const float LabelY = Pad + GraphH + 2.f;

		FSlateDrawElement::MakeText(
			OutDrawElements,
			LayerId,
			AllottedGeometry.ToPaintGeometry(FVector2f(40.f, 14.f), FSlateLayoutTransform(FVector2f(Pad, LabelY))),
			LOCTEXT("AxisDark", "暗"),
			AxisFont,
			ESlateDrawEffect::None,
			FLinearColor(1.f, 1.f, 1.f, 0.55f));

		FSlateDrawElement::MakeText(
			OutDrawElements,
			LayerId,
			AllottedGeometry.ToPaintGeometry(FVector2f(40.f, 14.f), FSlateLayoutTransform(FVector2f(Pad + GraphW - 40.f, LabelY))),
			LOCTEXT("AxisBright", "亮"),
			AxisFont,
			ESlateDrawEffect::None,
			FLinearColor(1.f, 1.f, 1.f, 0.55f));
	}

	++LayerId;
	return LayerId;
}

// ---------------------------------------------------------------------------
// SFalseColorView
// ---------------------------------------------------------------------------

void SFalseColorView::Construct(const FArguments& InArgs)
{
	Brush.DrawAs = ESlateBrushDrawType::Image;
}

void SFalseColorView::SetTexture(UTexture2D* InTexture)
{
	Texture.Reset(InTexture);
	Brush.SetResourceObject(InTexture);
	Invalidate(EInvalidateWidgetReason::Paint);
}

FVector2D SFalseColorView::ComputeDesiredSize(float LayoutScaleMultiplier) const
{
	// 只是"没有可用空间时"的兜底尺寸；实际高度由 FillHeight 槽位给多少用多少
	return FVector2D(320.f, 180.f);
}

int32 SFalseColorView::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const FVector2D LocalSize = AllottedGeometry.GetLocalSize();
	if (LocalSize.X <= 1.f || LocalSize.Y <= 1.f)
	{
		return LayerId;
	}

	// 不画背景：等比缩放留下的留边直接露出面板主题背景。
	// （原先画了一块近黑底 (0.015,0.015,0.02)，面积能占一半以上，容易被误读成"图像内容是黑的"）
	UTexture2D* Tex = Texture.Get();
	if (!Tex)
	{
		return LayerId;
	}

	const float TexW = (float)Tex->GetSizeX();
	const float TexH = (float)Tex->GetSizeY();
	if (TexW <= 0.f || TexH <= 0.f)
	{
		return LayerId;
	}

	// 等比铺满（contain）并居中：面板拉多宽就用多大，且画面不变形
	const float FitScale = FMath::Min(LocalSize.X / TexW, LocalSize.Y / TexH);
	const FVector2D DrawSize(TexW * FitScale, TexH * FitScale);
	const FVector2D Offset((LocalSize.X - DrawSize.X) * 0.5f, (LocalSize.Y - DrawSize.Y) * 0.5f);

	// 注意 ToPaintGeometry 的签名：尺寸在前、偏移在后（UE 5.5）
	FSlateDrawElement::MakeBox(
		OutDrawElements,
		LayerId,
		AllottedGeometry.ToPaintGeometry(
			FVector2f((float)DrawSize.X, (float)DrawSize.Y),
			FSlateLayoutTransform(FVector2f((float)Offset.X, (float)Offset.Y))),
		&Brush,
		ESlateDrawEffect::None,
		FLinearColor::White);

	return LayerId + 1;
}

// ---------------------------------------------------------------------------
// SHistogramPanel
// ---------------------------------------------------------------------------

void SHistogramPanel::Construct(const FArguments& InArgs)
{
	FalseColorProfile = FFalseColorProfile::MakeDefault();

	GraphWidget = SNew(SHistogramGraph)
		.Visibility(this, &SHistogramPanel::GetHistogramVisibility);
	FalseColorView = SNew(SFalseColorView)
		.Visibility(this, &SHistogramPanel::GetFalseColorVisibility);
	StatsText = SNew(STextBlock)
		.Text(LOCTEXT("StatsInitial", "尚未捕获。点击「捕获视口」，或勾选自动刷新并保持一个激活的 3D 视口。"))
		.AutoWrapText(true);
	DiagnosisText = SNew(STextBlock)
		.Text(LOCTEXT("DiagInitial", "说明：捕获后将根据直方图给出当前画面的问题与调色建议。"))
		.AutoWrapText(true);
	StatusText = SNew(STextBlock).Text(FText::GetEmpty());

	GrayscaleCheckBox = SNew(SCheckBox)
		.IsChecked(ECheckBoxState::Unchecked)
		.OnCheckStateChanged(this, &SHistogramPanel::OnGrayscaleChanged)
		[
			SNew(STextBlock).Text(LOCTEXT("GrayscaleToggle", "黑白预览"))
		];

	FalseColorCheckBox = SNew(SCheckBox)
		.IsChecked(ECheckBoxState::Unchecked)
		.ToolTipText(LOCTEXT("FalseColorTooltip", "伪彩色曝光：把画面按 IRE 档位上色。绿=18%中灰基准，粉=肤色，黄/橙=高光，红=削波，蓝/紫=死黑。纯 CPU 计算，不改渲染、不影响直方图。"))
		.OnCheckStateChanged(this, &SHistogramPanel::OnFalseColorChanged)
		[
			SNew(STextBlock).Text(LOCTEXT("FalseColorToggle", "伪彩色预览（曝光）"))
		];

	OverlayCheckBox = SNew(SCheckBox)
		.IsChecked(ECheckBoxState::Unchecked)
		.ToolTipText(LOCTEXT("OverlayTooltip", "把伪彩图铺满整个 3D 视口（默认关闭，面板小图为主）。注意：大图是「抓下来的那一帧」，不跟随视角，建议配合自动刷新。"))
		.OnCheckStateChanged(this, &SHistogramPanel::OnOverlayChanged)
		[
			SNew(STextBlock).Text(LOCTEXT("OverlayToggle", "叠加到视口（大图）"))
		];

	SmoothCheckBox = SNew(SCheckBox)
		.IsChecked(bSmoothBands ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
		.ToolTipText(LOCTEXT("SmoothTooltip", "分档前先按块取平均亮度：抹掉逐像素跳档产生的彩色噪点，结果是大块干净的色域。关掉则是逐像素精确分档（真机语义）。"))
		.OnCheckStateChanged(this, &SHistogramPanel::OnSmoothChanged)
		[
			SNew(STextBlock).Text(LOCTEXT("SmoothToggle", "平滑去噪点（大块色域）"))
		];

	ZebraCheckBox = SNew(SCheckBox)
		.IsChecked(ECheckBoxState::Unchecked)
		.ToolTipText(LOCTEXT("ZebraTooltip", "斑马纹曝光预警：≥上限的像素画红斜纹（削波）、≤下限的画蓝斜纹（死黑）。只有超阈值的像素不透明，实时画面照常显示，所以可以和伪彩同时开。"))
		.OnCheckStateChanged(this, &SHistogramPanel::OnZebraChanged)
		[
			SNew(STextBlock).Text(LOCTEXT("ZebraToggle", "斑马纹"))
		];

	ZebraHighSlider = SNew(SSlider)
		.MinValue(50.f)
		.MaxValue(100.f)
		.Value(this, &SHistogramPanel::GetZebraHighIRE)
		.OnValueChanged(this, &SHistogramPanel::OnZebraHighChanged);

	ZebraLowSlider = SNew(SSlider)
		.MinValue(0.f)
		.MaxValue(50.f)
		.Value(this, &SHistogramPanel::GetZebraLowIRE)
		.OnValueChanged(this, &SHistogramPanel::OnZebraLowChanged);

	SAssignNew(SelectionStatsText, STextBlock)
		.Text(this, &SHistogramPanel::GetSelectionStatsText)
		.AutoWrapText(true);

	// 明细表头：全中文列名（每个单元格定宽，见 GMeshTableColumns）
	SelectionTableHeader = BuildMeshTableHeader();

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(4.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 8.f, 0.f)
			[
				SNew(SButton)
					.Text(this, &SHistogramPanel::GetViewModeLabel)
					.ToolTipText(LOCTEXT("ViewModeTooltip", "点击在「综合（R/G/B/亮度叠加）→ RGB 独立 → 亮度」之间切换"))
					.OnClicked(this, &SHistogramPanel::OnCycleViewModeClicked)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 8.f, 0.f)
			[
				SNew(SButton)
					.Text(LOCTEXT("CaptureButton", "捕获视口"))
					.OnClicked(this, &SHistogramPanel::OnCaptureClicked)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 8.f, 0.f)
			[
				SNew(SButton)
					.Text(LOCTEXT("ExportButton", "导出"))
					.ToolTipText(LOCTEXT("ExportTooltip", "一键导出到 Saved/HistogramAssistant：原始视口截图 + 黑白图 + 伪彩图 + 直方图（四张 PNG），外加一份含统计/诊断/各档占比的文本报告"))
					.OnClicked(this, &SHistogramPanel::OnExportClicked)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 8.f, 0.f)
			[
				SNew(SButton)
					.Text(LOCTEXT("OpenCompareButton", "A/B 对比"))
					.ToolTipText(LOCTEXT("OpenCompareTooltip", "打开/聚焦「帧对比（A/B）」窗口：参考帧 vs 当前帧，支持并排 / 擦除 / 差异，可直接拖到副屏（等价于 Tools 菜单里的同名项）"))
					.OnClicked(this, &SHistogramPanel::OnOpenCompareClicked)
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
			[
				GrayscaleCheckBox.ToSharedRef()
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
			[
				FalseColorCheckBox.ToSharedRef()
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(4.f, 0.f, 4.f, 4.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
			[
				SNew(SCheckBox)
					.IsChecked(bAutoRefresh ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
					.OnCheckStateChanged(this, &SHistogramPanel::OnAutoRefreshChanged)
					[
						SNew(STextBlock).Text(LOCTEXT("AutoRefresh", "自动刷新（0.5 秒）"))
					]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
			[
				OverlayCheckBox.ToSharedRef()
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
			[
				SmoothCheckBox.ToSharedRef()
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				StatusText.ToSharedRef()
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(4.f, 0.f, 4.f, 4.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
			[
				ZebraCheckBox.ToSharedRef()
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 4.f, 0.f)
			[
				SNew(STextBlock).Text(LOCTEXT("ZebraHighLabel", "削波 ≥"))
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(0.f, 0.f, 12.f, 0.f)
			[
				ZebraHighSlider.ToSharedRef()
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 4.f, 0.f)
			[
				SNew(STextBlock).Text(LOCTEXT("ZebraLowLabel", "死黑 ≤"))
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				ZebraLowSlider.ToSharedRef()
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(4.f, 0.f, 4.f, 4.f)
		[
			StatsText.ToSharedRef()
		]
		+ SVerticalBox::Slot().FillHeight(1.f).Padding(4.f)
		[
			GraphWidget.ToSharedRef()
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(4.f, 2.f, 4.f, 0.f)
		[
			SNew(SHorizontalBox)
				.Visibility(this, &SHistogramPanel::GetFalseColorVisibility)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0.f, 0.f, 8.f, 0.f)
			[
				SNew(SButton)
					.Text(this, &SHistogramPanel::GetFalseColorModeLabel)
					.ToolTipText(LOCTEXT("FcModeTooltip", "点击切换：连续色图（整幅画面按亮度着彩虹色，看结构）↔ 档位（只标关键档、其余灰度，判曝光）"))
					.OnClicked(this, &SHistogramPanel::OnCycleFalseColorMode)
			]
			+ SHorizontalBox::Slot().FillWidth(1.f)
			[
				SNew(STextBlock)
					.Text(this, &SHistogramPanel::GetFalseColorLegend)
					.AutoWrapText(true)
			]
		]
		+ SVerticalBox::Slot().FillHeight(1.f).Padding(4.f, 2.f, 4.f, 4.f)
		[
			FalseColorView.ToSharedRef()
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(4.f)
		[
			SNew(STextBlock)
				.Text(LOCTEXT("DiagTitle", "直方图诊断"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(4.f, 0.f, 4.f, 4.f)
		[
			DiagnosisText.ToSharedRef()
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(4.f, 6.f, 4.f, 0.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock)
					.Text(LOCTEXT("SelectionStatsTitle", "选中对象资源统计"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
			[
				SNew(SCheckBox)
					.IsChecked(this, &SHistogramPanel::GetShowMeshDetailsState)
					.OnCheckStateChanged(this, &SHistogramPanel::OnShowMeshDetailsChanged)
					.ToolTipText(LOCTEXT("ShowMeshDetailsTip", "展开后按唯一网格聚合，列出最重的 12 个网格的全部 25 列（口径与自带 Window → Statistics → Primitive Stats 一致，数字可对照；贴图几列是插件额外补的）；鼠标停在中文列名上会显示这一列的含义；点某一行＝在内容浏览器里「浏览至资产」，Ctrl+点击＝在视口里选中并聚焦用到它的 Actor"))
					[
						SNew(STextBlock).Text(LOCTEXT("ShowMeshDetails", "展开网格明细"))
					]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
			[
				SNew(SButton)
					.ContentPadding(FMargin(4.f, 1.f))
					.ToolTipText(LOCTEXT("TextureDiagTip", "把当前选中对象的「材质 → 贴图」完整明细（尺寸/参数名/来自哪个材质/内存/父材质）写进日志。\n觉得贴图数字不对时点一下，然后去 Saved\\Logs\\xiaozhen.log 搜 HistogramAssistant 就能看到全部数据"))
					.OnClicked(FOnClicked::CreateLambda([this]()
					{
						return OnDumpTextureDiagnostics();
					}))
					[
						SNew(STextBlock).Text(LOCTEXT("TextureDiag", "贴图诊断→日志"))
					]
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(4.f, 0.f, 4.f, 0.f)
		[
			SelectionStatsText.ToSharedRef()
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(4.f, 2.f, 4.f, 4.f)
		[
			SNew(SVerticalBox)
				.Visibility(this, &SHistogramPanel::GetMeshDetailsVisibility)
			+ SVerticalBox::Slot().AutoHeight()
			[
				// 列很多（对齐 Primitive Stats 全字段）→ 横向可滚动，不撑破面板
				SNew(SScrollBox)
					.Orientation(Orient_Horizontal)
				+ SScrollBox::Slot()
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SelectionTableHeader.ToSharedRef()
					]
					+ SVerticalBox::Slot().AutoHeight()[ BuildMeshRowWidget(0) ]
					+ SVerticalBox::Slot().AutoHeight()[ BuildMeshRowWidget(1) ]
					+ SVerticalBox::Slot().AutoHeight()[ BuildMeshRowWidget(2) ]
					+ SVerticalBox::Slot().AutoHeight()[ BuildMeshRowWidget(3) ]
					+ SVerticalBox::Slot().AutoHeight()[ BuildMeshRowWidget(4) ]
					+ SVerticalBox::Slot().AutoHeight()[ BuildMeshRowWidget(5) ]
					+ SVerticalBox::Slot().AutoHeight()[ BuildMeshRowWidget(6) ]
					+ SVerticalBox::Slot().AutoHeight()[ BuildMeshRowWidget(7) ]
					+ SVerticalBox::Slot().AutoHeight()[ BuildMeshRowWidget(8) ]
					+ SVerticalBox::Slot().AutoHeight()[ BuildMeshRowWidget(9) ]
					+ SVerticalBox::Slot().AutoHeight()[ BuildMeshRowWidget(10) ]
					+ SVerticalBox::Slot().AutoHeight()[ BuildMeshRowWidget(11) ]
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f, 0.f, 0.f)
			[
				SNew(STextBlock)
					.Text(LOCTEXT("MeshRowHint", "点某一行 = 在内容浏览器里浏览至该网格资产（蓝图行会一起选中）；Ctrl + 点击 = 在视口里选中并聚焦用到它的 Actor\n尺寸单位：贴图/光照图是像素（px），半径是厘米（cm，100 cm = 1 米）；鼠标停在列名或某一行上有详细解释\n贴图列 = 【组件实际使用的材质】里【真正采样到】的贴图；「贴图最大」含外部带进来的贴图，「本资产贴图」只算同目录的 —— 两者差很多就是外部贴图在占内存"))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
					.ColorAndOpacity(FLinearColor(0.65f, 0.65f, 0.65f))
			]
		]
	];

	RegisterActiveTimer(0.5f, FWidgetActiveTimerDelegate::CreateSP(this, &SHistogramPanel::OnAutoRefreshTick));

	// 选中对象资源统计：跟随编辑器选择变化（只标脏，实际重算在 0.5 秒节拍里做）
	USelection::SelectionChangedEvent.AddRaw(this, &SHistogramPanel::OnEditorSelectionChanged);
	RefreshSelectionStats();
}

SHistogramPanel::~SHistogramPanel()
{
	// 关闭面板时顺带把黑白预览还原，避免把临时 Volume 留在关卡里
	if (bGrayscalePreview)
	{
		FHistogramCapture::SetGrayscalePreview(false);
		bGrayscalePreview = false;
	}

	// 伪彩是纯 CPU 显示，没有场景副作用；但要记得把视口上的两层覆盖层都摘掉
	RemoveViewportOverlay();
	RemoveZebraOverlay();
	ClearFalseColorImage();

	// 解绑选择变化回调（delegate 存的是 raw 指针，必须摘掉）
	USelection::SelectionChangedEvent.RemoveAll(this);
}

FReply SHistogramPanel::OnCaptureClicked()
{
	DoCapture();
	return FReply::Handled();
}

FReply SHistogramPanel::OnOpenCompareClicked()
{
	// 打开（或聚焦）帧对比窗口；这里的名字必须与模块里注册的 CompareTabName 完全一致
	FGlobalTabmanager::Get()->TryInvokeTab(FTabId(FName(TEXT("HistogramAssistantCompareTab"))));
	return FReply::Handled();
}

FReply SHistogramPanel::OnExportClicked()
{
	// 1) 先弹目录选择框（先选后抓，取消就不浪费一次抓图）
	FString ChosenDir = LastExportDirectory;
	if (ChosenDir.IsEmpty())
	{
		ChosenDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("HistogramAssistant"));
	}

	if (IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get())
	{
		const void* ParentWindowHandle = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
		FString PickedDir = ChosenDir;
		const bool bPicked = DesktopPlatform->OpenDirectoryDialog(
			ParentWindowHandle,
			LOCTEXT("ExportDirTitle", "选择导出目录").ToString(),
			ChosenDir,
			PickedDir);

		if (!bPicked)
		{
			StatusText->SetText(LOCTEXT("ExportCancelled", "已取消导出"));
			StatusText->SetColorAndOpacity(FLinearColor(0.8f, 0.8f, 0.8f));
			return FReply::Handled();
		}

		ChosenDir = PickedDir;
	}
	// 拿不到平台接口时（理论上不会）不阻断，直接用默认目录
	LastExportDirectory = ChosenDir;

	// 2) 抓一帧（导出必须拿到像素，与伪彩是否开启无关）
	FHistogramData Data;
	TArray<FColor> Pixels;
	if (!FHistogramCapture::CaptureActiveViewport(Data, &Pixels))
	{
		StatusText->SetText(LOCTEXT("ExportNoCapture", "导出失败：请确认编辑器里有一个激活的 3D 视口"));
		StatusText->SetColorAndOpacity(FLinearColor(1.f, 0.45f, 0.45f));
		return FReply::Handled();
	}

	// 与屏幕显示保持一致的档位表（含当前的平滑设置与伪彩模式）
	FFalseColorProfile EffectiveProfile = FalseColorProfile;
	EffectiveProfile.SampleBlockSize = bSmoothBands
		? FMath::Clamp(FMath::RoundToInt((float)FMath::Min(Data.Width, Data.Height) / 160.f), 2, 16)
		: 1;

	const FHistogramExportResult Result = FHistogramExport::ExportMerged(
		ChosenDir,
		Pixels,
		Data.Width,
		Data.Height,
		Data,
		EffectiveProfile);

	if (Result.bSuccess)
	{
		StatusText->SetText(FText::FromString(FString::Printf(TEXT("已导出四合一图 → %s"), *Result.FilePath)));
		StatusText->SetColorAndOpacity(FLinearColor(0.45f, 1.f, 0.45f));
	}
	else
	{
		StatusText->SetText(FText::FromString(FString::Printf(TEXT("导出失败：%s"), *Result.Error)));
		StatusText->SetColorAndOpacity(FLinearColor(1.f, 0.45f, 0.45f));
	}

	return FReply::Handled();
}

FString SHistogramPanel::BuildBandReport(const FHistogramData& Data) const
{
	FString Report;
	int32 BinStart = 0;
	const double Total = (double)Data.Width * (double)Data.Height;

	for (const FFalseColorBand& Band : FalseColorProfile.Bands)
	{
		const int32 BinEnd = FMath::Clamp(FMath::RoundToInt(Band.MaxIRE * 2.55f), 0, 256);

		int64 Count = 0;
		for (int32 Bin = BinStart; Bin < BinEnd; ++Bin)
		{
			Count += Data.LumaHist[Bin];
		}

		const double Pct = Total > 0.0 ? 100.0 * (double)Count / Total : 0.0;
		Report += FString::Printf(TEXT("[%.1f-%.1f]=%.1f%% "), BinStart / 2.55f, Band.MaxIRE, Pct);
		BinStart = BinEnd;
	}

	return Report;
}

FReply SHistogramPanel::OnCycleViewModeClicked()
{
	ViewMode = static_cast<EHistogramViewMode>((static_cast<int32>(ViewMode) + 1) % 3);
	GraphWidget->SetViewMode(ViewMode);
	return FReply::Handled();
}

FText SHistogramPanel::GetViewModeLabel() const
{
	switch (ViewMode)
	{
	case EHistogramViewMode::SplitRGB:
		return LOCTEXT("ModeSplit", "显示：RGB 独立");
	case EHistogramViewMode::LumaOnly:
		return LOCTEXT("ModeLuma", "显示：亮度");
	case EHistogramViewMode::Combined:
	default:
		return LOCTEXT("ModeCombined", "显示：综合");
	}
}

void SHistogramPanel::OnGrayscaleChanged(ECheckBoxState NewState)
{
	bGrayscalePreview = (NewState == ECheckBoxState::Checked);

	// 与伪彩色预览互斥：两个显示效果叠在一起没有意义
	if (bGrayscalePreview && bFalseColorPreview)
	{
		bFalseColorPreview = false;
		if (FalseColorCheckBox.IsValid())
		{
			FalseColorCheckBox->SetIsChecked(ECheckBoxState::Unchecked);
		}
		ClearFalseColorImage();
	}

	if (FHistogramCapture::SetGrayscalePreview(bGrayscalePreview))
	{
		StatusText->SetText(bGrayscalePreview
			? LOCTEXT("GrayscaleOn", "黑白预览已开启（饱和度=0），再次点击可还原")
			: LOCTEXT("GrayscaleOff", "黑白预览已关闭"));
		StatusText->SetColorAndOpacity(FLinearColor(0.45f, 1.f, 0.45f));
		if (bGrayscalePreview)
		{
			DoCapture(); // 重新抓一帧，让直方图反映黑白后的明暗
		}
	}
	else
	{
		bGrayscalePreview = false;
		if (GrayscaleCheckBox.IsValid())
		{
			GrayscaleCheckBox->SetIsChecked(ECheckBoxState::Unchecked);
		}
		StatusText->SetText(LOCTEXT("GrayscaleFailed", "黑白预览失败：请确认有激活的 3D 视口"));
		StatusText->SetColorAndOpacity(FLinearColor(1.f, 0.45f, 0.45f));
	}
}

void SHistogramPanel::OnFalseColorChanged(ECheckBoxState NewState)
{
	bFalseColorPreview = (NewState == ECheckBoxState::Checked);

	if (!bFalseColorPreview)
	{
		RemoveViewportOverlay();
		ClearFalseColorImage();
		StatusText->SetText(LOCTEXT("FalseColorOff", "伪彩色预览已关闭"));
		StatusText->SetColorAndOpacity(FLinearColor(0.45f, 1.f, 0.45f));
		return;
	}

	// 与黑白预览互斥：黑白会破坏"用颜色判断曝光"的前提
	if (bGrayscalePreview)
	{
		if (GrayscaleCheckBox.IsValid())
		{
			GrayscaleCheckBox->SetIsChecked(ECheckBoxState::Unchecked);
		}
		FHistogramCapture::SetGrayscalePreview(false);
		bGrayscalePreview = false;
	}

	// 立刻抓一帧生成伪彩图
	DoCapture();

	// 只有用户显式勾了「叠加到视口（大图）」才铺满视口；默认只显示面板小图
	if (bViewportOverlay)
	{
		AddViewportOverlay();
	}
}

void SHistogramPanel::OnOverlayChanged(ECheckBoxState NewState)
{
	bViewportOverlay = (NewState == ECheckBoxState::Checked);

	if (bViewportOverlay && bFalseColorPreview)
	{
		AddViewportOverlay();
	}
	else
	{
		RemoveViewportOverlay();
	}
}

void SHistogramPanel::OnSmoothChanged(ECheckBoxState NewState)
{
	bSmoothBands = (NewState == ECheckBoxState::Checked);
	if (bFalseColorPreview)
	{
		DoCapture();   // 立刻按新设置重算一帧，方便开关对比
	}
}

void SHistogramPanel::AddViewportOverlay()
{
	if (OverlayView.IsValid())
	{
		return;   // 已经叠加过了
	}

	FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
	TSharedPtr<IAssetViewport> AssetViewport = LevelEditorModule.GetFirstActiveViewport();
	if (!AssetViewport.IsValid())
	{
		if (StatusText.IsValid())
		{
			StatusText->SetText(LOCTEXT("OverlayNoViewport", "未找到活动 3D 视口，伪彩只显示在面板里"));
			StatusText->SetColorAndOpacity(FLinearColor(1.f, 0.75f, 0.3f));
		}
		return;
	}

	// HitTestInvisible 很关键：可见但不吃鼠标事件，叠加后视口仍能正常旋转/缩放/框选
	OverlayView = SNew(SFalseColorView)
		.Visibility(EVisibility::HitTestInvisible);
	OverlayView->SetTexture(FalseColorView.IsValid() ? FalseColorView->GetTexture() : nullptr);

	AssetViewport->AddOverlayWidget(OverlayView.ToSharedRef(), /*ZOrder=*/100);
	OverlayViewport = AssetViewport;

	if (StatusText.IsValid())
	{
		StatusText->SetText(LOCTEXT("OverlayOn", "伪彩已铺满 3D 视口。建议同时勾选「自动刷新」，否则大图不跟随视角。"));
		StatusText->SetColorAndOpacity(FLinearColor(0.45f, 1.f, 0.45f));
	}
}

void SHistogramPanel::RemoveViewportOverlay()
{
	if (TSharedPtr<IAssetViewport> Viewport = OverlayViewport.Pin())
	{
		if (OverlayView.IsValid())
		{
			Viewport->RemoveOverlayWidget(OverlayView.ToSharedRef());
		}
	}
	OverlayViewport.Reset();
	OverlayView.Reset();
}

// ---------------------------------------------------------------------------
// 斑马纹（曝光预警）
// ---------------------------------------------------------------------------

void SHistogramPanel::OnZebraChanged(ECheckBoxState NewState)
{
	bZebraEnabled = (NewState == ECheckBoxState::Checked);

	if (bZebraEnabled)
	{
		AddZebraOverlay();
		DoCapture();   // 立刻抓一帧生成斑马纹纹理
	}
	else
	{
		RemoveZebraOverlay();
		if (StatusText.IsValid())
		{
			StatusText->SetText(LOCTEXT("ZebraOff", "斑马纹已关闭"));
			StatusText->SetColorAndOpacity(FLinearColor(0.45f, 1.f, 0.45f));
		}
	}
}

void SHistogramPanel::AddZebraOverlay()
{
	if (ZebraView.IsValid())
	{
		return;   // 已经叠加过了
	}

	FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
	TSharedPtr<IAssetViewport> AssetViewport = LevelEditorModule.GetFirstActiveViewport();
	if (!AssetViewport.IsValid())
	{
		if (StatusText.IsValid())
		{
			StatusText->SetText(LOCTEXT("ZebraNoViewport", "未找到活动 3D 视口，斑马纹无法显示"));
			StatusText->SetColorAndOpacity(FLinearColor(1.f, 0.75f, 0.3f));
		}
		return;
	}

	// HitTestInvisible：不吃鼠标事件，视口照常旋转缩放
	ZebraView = SNew(SFalseColorView)
		.Visibility(EVisibility::HitTestInvisible);
	ZebraView->SetTexture(nullptr);

	// ZOrder 110 > 伪彩覆盖层的 100 → 斑马纹画在伪彩之上
	AssetViewport->AddOverlayWidget(ZebraView.ToSharedRef(), /*ZOrder=*/110);
	ZebraViewport = AssetViewport;

	if (StatusText.IsValid())
	{
		StatusText->SetText(LOCTEXT("ZebraOn", "斑马纹已开启：红=削波、蓝=死黑（只有超阈值的像素不透明，实时画面照常可见）"));
		StatusText->SetColorAndOpacity(FLinearColor(0.45f, 1.f, 0.45f));
	}
}

void SHistogramPanel::RemoveZebraOverlay()
{
	if (TSharedPtr<IAssetViewport> Viewport = ZebraViewport.Pin())
	{
		if (ZebraView.IsValid())
		{
			Viewport->RemoveOverlayWidget(ZebraView.ToSharedRef());
		}
	}
	ZebraViewport.Reset();
	ZebraView.Reset();
	ZebraTexW = 0;
	ZebraTexH = 0;
}

void SHistogramPanel::UpdateZebraTexture(const TArray<FColor>& Pixels, int32 Width, int32 Height)
{
	if (!ZebraView.IsValid() || Width <= 0 || Height <= 0 || Pixels.Num() != Width * Height)
	{
		return;
	}

	// 阈值判定沿用"平滑"设置：块平均更稳定，不会在边界处闪碎纹
	FZebraSettings Effective = ZebraSettings;
	Effective.SampleBlockSize = bSmoothBands
		? FMath::Clamp(FMath::RoundToInt((float)FMath::Min(Width, Height) / 160.f), 2, 16)
		: 1;

	// 斜纹宽度也随分辨率缩放，保证不同视口大小下观感一致
	Effective.StripeWidth = FMath::Max(4, FMath::RoundToInt((float)FMath::Min(Width, Height) / 130.f));

	TArray<FColor> Overlay;
	FHistogramZebra::MakeOverlay(Pixels, Width, Height, Effective, Overlay);
	if (Overlay.Num() != Width * Height)
	{
		return;
	}

	UTexture2D* Target = ZebraView->GetTexture();
	if (!Target || ZebraTexW != Width || ZebraTexH != Height)
	{
		UTexture2D* NewTexture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
		if (!NewTexture)
		{
			return;
		}

		NewTexture->SRGB = true;          // 斜纹颜色是显示域值
		NewTexture->Filter = TF_Nearest;  // 硬边斜纹，不要插值
		NewTexture->AddressX = TA_Clamp;
		NewTexture->AddressY = TA_Clamp;
		NewTexture->UpdateResource();

		ZebraView->SetTexture(NewTexture);
		ZebraTexW = Width;
		ZebraTexH = Height;
		Target = NewTexture;
	}

	const int32 SrcPitch = Width * (int32)sizeof(FColor);
	const int32 NumBytes = Overlay.Num() * (int32)sizeof(FColor);

	// 同伪彩：Regions 指针必须在渲染线程仍有效 → 堆上分配，在清理回调里释放
	FUpdateTextureRegion2D* RegionPtr = new FUpdateTextureRegion2D(0, 0, 0, 0, (uint32)Width, (uint32)Height);
	uint8* SrcData = (uint8*)FMemory::Malloc(NumBytes);
	FMemory::Memcpy(SrcData, Overlay.GetData(), NumBytes);

	Target->UpdateTextureRegions(
		0,
		1,
		RegionPtr,
		(uint32)SrcPitch,
		(uint32)sizeof(FColor),
		SrcData,
		[RegionPtr](uint8* InData, const FUpdateTextureRegion2D*)
		{
			FMemory::Free(InData);
			delete RegionPtr;
		});
}

float SHistogramPanel::GetZebraHighIRE() const
{
	return ZebraSettings.HighIRE;
}

float SHistogramPanel::GetZebraLowIRE() const
{
	return ZebraSettings.LowIRE;
}

void SHistogramPanel::OnZebraHighChanged(float NewValue)
{
	ZebraSettings.HighIRE = FMath::Clamp(NewValue, 0.f, 100.f);
	if (bZebraEnabled)
	{
		DoCapture();   // 立刻按新阈值重算
	}
}

void SHistogramPanel::OnZebraLowChanged(float NewValue)
{
	ZebraSettings.LowIRE = FMath::Clamp(NewValue, 0.f, 100.f);
	if (bZebraEnabled)
	{
		DoCapture();
	}
}

EVisibility SHistogramPanel::GetFalseColorVisibility() const
{
	return bFalseColorPreview ? EVisibility::Visible : EVisibility::Collapsed;
}

FReply SHistogramPanel::OnCycleFalseColorMode()
{
	FalseColorProfile.Mode = (FalseColorProfile.Mode == EFalseColorMode::ContinuousRamp)
		? EFalseColorMode::ExposureBands
		: EFalseColorMode::ContinuousRamp;

	if (bFalseColorPreview)
	{
		DoCapture();   // 立刻按新模式重算一帧，方便对比
	}
	return FReply::Handled();
}

FText SHistogramPanel::GetFalseColorModeLabel() const
{
	return FalseColorProfile.Mode == EFalseColorMode::ContinuousRamp
		? LOCTEXT("FcModeRamp", "伪彩：连续色图")
		: LOCTEXT("FcModeBands", "伪彩：档位（曝光）");
}

FText SHistogramPanel::GetFalseColorLegend() const
{
	if (FalseColorProfile.Mode == EFalseColorMode::ContinuousRamp)
	{
		return LOCTEXT("LegendRamp", "连续色图：整幅画面按亮度着色 —— 蓝=最暗 → 青 → 绿 → 黄 → 红=最亮。用来看结构/层次，不表示绝对曝光档位。");
	}

	// 档位模式的术语按真机含义写清楚：粉档说的是"亮度位置"而不是颜色，且原文限定的是浅肤色
	return LOCTEXT("LegendBands", "档位（曝光）：只标关键档、其余显示灰度 —— 紫/蓝=死黑 · 青=深阴影 · 绿=18% 中灰基准（灰卡）· 粉=浅肤色常见亮度（≈61–70 IRE）· 麦秆/黄=强高光 · 橙/红=接近削波/削波。「平均亮度」×100 = 画面平均 IRE。");
}

EVisibility SHistogramPanel::GetHistogramVisibility() const
{
	// 伪彩开启时隐藏直方图：伪彩本身就是曝光工具，同屏两份信息只会互相挤；
	// 隐藏后腾出的空间交给伪彩图（FillHeight），图就能跟着面板变大。
	return bFalseColorPreview ? EVisibility::Collapsed : EVisibility::Visible;
}

void SHistogramPanel::ClearFalseColorImage()
{
	if (FalseColorView.IsValid())
	{
		FalseColorView->SetTexture(nullptr);
	}
	if (OverlayView.IsValid())
	{
		OverlayView->SetTexture(nullptr);
	}
	FalseColorTexW = 0;
	FalseColorTexH = 0;
}

void SHistogramPanel::UpdateFalseColorImage(const TArray<FColor>& Pixels, int32 Width, int32 Height)
{
	if (Width <= 0 || Height <= 0 || Pixels.Num() != Width * Height)
	{
		return;
	}

	// 只读原始像素，上色结果放到显示副本里 —— 直方图统计仍基于原始像素，互不影响
	// 平滑档位：块大小随视口自适应（约每 160 像素一块），把逐像素跳档的彩色噪点抹平
	FFalseColorProfile EffectiveProfile = FalseColorProfile;
	EffectiveProfile.SampleBlockSize = bSmoothBands
		? FMath::Clamp(FMath::RoundToInt((float)FMath::Min(Width, Height) / 160.f), 2, 16)
		: 1;

	TArray<FColor> Colored;
	FHistogramFalseColor::Apply(Pixels, Width, Height, EffectiveProfile, Colored);

	if (Colored.Num() != Width * Height)
	{
		return;
	}

	if (!FalseColorView.IsValid())
	{
		return;
	}

	// 尺寸变化（换视口/改分辨率）时重建纹理
	UTexture2D* Target = FalseColorView->GetTexture();
	if (!Target || FalseColorTexW != Width || FalseColorTexH != Height)
	{
		UTexture2D* NewTexture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
		if (!NewTexture)
		{
			return;
		}

		NewTexture->SRGB = true;          // 档位色本身就是 8bit 显示域值
		NewTexture->Filter = TF_Nearest;  // 伪彩是硬边色块，不要插值
		NewTexture->AddressX = TA_Clamp;
		NewTexture->AddressY = TA_Clamp;
		NewTexture->UpdateResource();

		FalseColorView->SetTexture(NewTexture);
		FalseColorTexW = Width;
		FalseColorTexH = Height;
		Target = NewTexture;
	}

	// 视口覆盖层显示同一张纹理（若已叠加）
	if (OverlayView.IsValid())
	{
		OverlayView->SetTexture(Target);
	}

	const int32 SrcPitch = Width * (int32)sizeof(FColor);
	const int32 NumBytes = Colored.Num() * (int32)sizeof(FColor);

	// ★ 关键：UTexture2D::UpdateTextureRegions 只是把 Regions 指针存下来，直到渲染线程
	//   （Texture2D.cpp:1421）才解引用。所以区域对象必须放在堆上并活到清理回调，
	//   绝不能在栈上构造 FUpdateTextureRegion2D 再传地址 —— 那会读到失效的栈内存，
	//   表现为 D3D12 拷贝命令列表 Close() 失败 / E_INVALIDARG 崩溃（2026-09-18 踩过）。
	FUpdateTextureRegion2D* RegionPtr = new FUpdateTextureRegion2D(0, 0, 0, 0, (uint32)Width, (uint32)Height);
	uint8* SrcData = (uint8*)FMemory::Malloc(NumBytes);
	FMemory::Memcpy(SrcData, Colored.GetData(), NumBytes);

	Target->UpdateTextureRegions(
		0,
		1,
		RegionPtr,
		(uint32)SrcPitch,
		(uint32)sizeof(FColor),
		SrcData,
		[RegionPtr](uint8* InData, const FUpdateTextureRegion2D*)
		{
			// 这段在 RHI 线程执行：像素缓冲与区域对象一起释放
			FMemory::Free(InData);
			delete RegionPtr;
		});
}

void SHistogramPanel::OnAutoRefreshChanged(ECheckBoxState NewState)
{
	bAutoRefresh = (NewState == ECheckBoxState::Checked);
	if (bAutoRefresh)
	{
		DoCapture();
	}
}

EActiveTimerReturnType SHistogramPanel::OnAutoRefreshTick(double InCurrentTime, float InDeltaTime)
{
	// 选择变化时只标脏，统一在这里重算（避免拖框选时疯狂重算）
	if (bSelectionStatsDirty)
	{
		RefreshSelectionStats();
	}

	if (bAutoRefresh)
	{
		DoCapture();
	}
	return EActiveTimerReturnType::Continue;
}

void SHistogramPanel::DoCapture()
{
	FHistogramData Data;
	TArray<FColor> Pixels;

	// 只有开了伪彩色才额外取像素，平时零开销
	const bool bWantPixels = bFalseColorPreview || bZebraEnabled;

	if (FHistogramCapture::CaptureActiveViewport(Data, bWantPixels ? &Pixels : nullptr))
	{
		GraphWidget->SetData(Data);
		StatsText->SetText(BuildStatsText(Data));
		DiagnosisText->SetText(BuildDiagnosisText(Data));
		StatusText->SetText(FText::FromString(FString::Printf(TEXT("已捕获 %d x %d  %s"), Data.Width, Data.Height, *FDateTime::Now().ToString())));
		StatusText->SetColorAndOpacity(FLinearColor(0.45f, 1.f, 0.45f));

		if (bWantPixels)
		{
			if (bFalseColorPreview)
			{
				UpdateFalseColorImage(Pixels, Data.Width, Data.Height);
			}
			if (bZebraEnabled)
			{
				UpdateZebraTexture(Pixels, Data.Width, Data.Height);
			}

			// 调试日志：把各档在画面里的占比打到 Output Log（搜 [HistogramAssistant]），
			// 用来核对"画面上看到的颜色"和"统计数字"是否一致（无需再靠截图推断）。
			UE_LOG(LogTemp, Log, TEXT("[HistogramAssistant] FalseColor %dx%d mode=%s meanIRE=%.1f  %s"),
				Data.Width, Data.Height,
				FalseColorProfile.Mode == EFalseColorMode::ContinuousRamp ? TEXT("ramp") : TEXT("bands"),
				Data.AvgLuma / 2.55f, *BuildBandReport(Data));
		}
	}
	else
	{
		StatusText->SetText(LOCTEXT("CaptureFailed", "捕获失败：请确认编辑器里有一个激活的 3D 视口"));
		StatusText->SetColorAndOpacity(FLinearColor(1.f, 0.45f, 0.45f));
	}
}

FText SHistogramPanel::BuildStatsText(const FHistogramData& D) const
{
	return FText::FromString(FString::Printf(
		TEXT("视口: %d x %d\n平均亮度: %.3f    最暗: %.3f    最亮: %.3f\n阴影: %.1f%%    中间调: %.1f%%    高光: %.1f%%\nR 平均: %.1f    G 平均: %.1f    B 平均: %.1f"),
		D.Width,
		D.Height,
		D.AvgLuma / 255.f,
		D.MinLuma / 255.f,
		D.MaxLuma / 255.f,
		D.ShadowRatio * 100.f,
		D.MidtoneRatio * 100.f,
		D.HighlightRatio * 100.f,
		D.AvgR,
		D.AvgG,
		D.AvgB));
}

FText SHistogramPanel::BuildDiagnosisText(const FHistogramData& D) const
{
	const float Luma = D.AvgLuma;

	// 1. 曝光判断
	FString Exposure;
	if (Luma < 64.f)
	{
		Exposure = TEXT("曝光不足（偏暗）——提高曝光/亮度，或提亮阴影");
	}
	else if (Luma < 118.f)
	{
		Exposure = TEXT("曝光偏暗——可适当提亮");
	}
	else if (Luma <= 165.f)
	{
		Exposure = TEXT("曝光正常，明暗均衡");
	}
	else if (Luma <= 216.f)
	{
		Exposure = TEXT("曝光偏亮——可适当压暗");
	}
	else
	{
		Exposure = TEXT("曝光过度（偏亮）——降低曝光，或压高光");
	}

	// 2. 影调比例判断
	FString Tonal;
	if (D.ShadowRatio > 0.48f && D.HighlightRatio < 0.10f)
	{
		Tonal = TEXT("画面以暗部为主：若就是要低调氛围则正常，否则提亮阴影");
	}
	else if (D.HighlightRatio > 0.48f && D.ShadowRatio < 0.10f)
	{
		Tonal = TEXT("画面以高光为主：若就是要高调氛围则正常，否则压高光");
	}
	else if (D.MidtoneRatio < 0.22f)
	{
		Tonal = TEXT("中间调偏少、明暗对比强，画面可能显生硬，可降低对比");
	}
	else
	{
		Tonal = TEXT("影调分布均衡");
	}

	// 3. 对比度判断（用最暗/最亮范围）
	FString Contrast;
	const float Range = D.MaxLuma - D.MinLuma;
	if (Range < 70.f)
	{
		Contrast = TEXT("对比度偏低，画面发灰——增加对比度");
	}
	else if (Range > 200.f)
	{
		Contrast = TEXT("对比度很高——如需柔和画面可降低对比");
	}
	else
	{
		Contrast = TEXT("对比度合适");
	}

	// 4. 色调偏移判断（用 RGB 均值差）
	FString Tint;
	const float Rm = D.AvgR, Gm = D.AvgG, Bm = D.AvgB;
	const float RG = Rm - Gm;
	const float RB = Rm - Bm;
	const float GB = Gm - Bm;

	if (FMath::Abs(RG) < 8.f && FMath::Abs(GB) < 8.f)
	{
		Tint = TEXT("RGB 均衡，无明显偏色");
	}
	else
	{
		TArray<FString> Parts;
		if (RB > 10.f)
		{
			Parts.Add(TEXT("偏红/暖"));
		}
		else if (RB < -10.f)
		{
			Parts.Add(TEXT("偏蓝/冷"));
		}
		if (RG > 8.f && GB > 8.f)
		{
			Parts.Add(TEXT("偏黄"));
		}
		else if (RG < -8.f && GB < -8.f)
		{
			Parts.Add(TEXT("偏品红/紫"));
		}
		else if (GB > 8.f && RG < 0.f)
		{
			Parts.Add(TEXT("偏青/绿"));
		}

		if (Parts.Num() == 0)
		{
			Tint = TEXT("色调基本正常");
		}
		else
		{
			Tint = FString::Printf(TEXT("色调偏移：%s——调整白平衡或色温"), *FString::Join(Parts, TEXT("、")));
		}
	}

	return FText::FromString(FString::Printf(
		TEXT("曝光：%s\n%s\n对比度：%s\n色调：%s"),
		*Exposure,
		*Tonal,
		*Contrast,
		*Tint));
}

void SHistogramPanel::OnEditorSelectionChanged(UObject* NewSelection)
{
	bSelectionStatsDirty = true;
}

void SHistogramPanel::RefreshSelectionStats()
{
	FHistogramSelectionStats::Compute(CachedSelectionStats, 12);
	SelectionMeshRows.Reset();

	if (!CachedSelectionStats.bHasSelection || CachedSelectionStats.ComponentCount == 0)
	{
		SelectionStatsString = TEXT("（在关卡里选中一个 Actor / 蓝图实例 —— 这里显示组件数、合计三角面、资源占用、材质槽与光照统计）");
	}
	else
	{
		const FSelectionStats& S = CachedSelectionStats;

		FString Text = FString::Printf(TEXT("选中：%s\n"), *S.Label);
		Text += FString::Printf(TEXT("组件数 %d ｜ 唯一网格 %d ｜ 单件三角面 %d ｜ 合计三角面 %s\n"),
			S.ComponentCount,
			S.UniqueMeshCount,
			S.TotalTriangles,
			*FText::AsNumber(S.TotalInstTriangles).ToString());
		Text += FString::Printf(TEXT("单件材质槽 %d ｜ 合计槽位 %d ｜ 网格占用 %s ｜ 顶点色 %s\n"),
			S.TotalSections,
			(int32)S.TotalInstSections,
			*FormatBytesShort((int64)(S.TotalResourceKB * 1024.0)),
			*FormatBytesShort((int64)(S.TotalVertexColorKB * 1024.0)));
		Text += FString::Printf(TEXT("实例顶点色 %s ｜ 光照贴图 %s ｜ 均·光照图分辨率 %.0f（√宽×高）\n"),
			*FormatBytesShort((int64)(S.TotalInstVertexColorKB * 1024.0)),
			*FormatBytesShort((int64)(S.TotalLightMapKB * 1024.0)),
			S.AvgRes);

		// 尺寸单独一行：光照图看全选中里最大的一张，材质贴图只统计明细表里显示的那几行
		const FString LightMapSizeText = (S.LightMapMaxW > 0 && S.LightMapMaxH > 0)
			? FString::Printf(TEXT("%d×%d"), S.LightMapMaxW, S.LightMapMaxH)
			: FString(TEXT("无（没烘光照）"));
		const FString TextureSizeText = (S.TextureMaxW > 0 && S.TextureMaxH > 0)
			? FString::Printf(TEXT("%d×%d"), S.TextureMaxW, S.TextureMaxH)
			: FString(TEXT("无贴图"));
		Text += FString::Printf(TEXT("光照图最大尺寸 %s px ｜ 材质贴图最大尺寸 %s px ｜ 贴图占用 %s（只统计明细表里的 Top12）\n"),
			*LightMapSizeText,
			*TextureSizeText,
			*FormatBytesShort((int64)(S.TextureKB * 1024.0)));

		Text += FString::Printf(TEXT("均·光照图光源 %.1f ｜ 均·动态光源 %.1f ｜ 成本 %.1f ｜ 包围球半径 %.1f~%.1f cm（平均 %.1f）"),
			S.AvgLM,
			S.AvgOL,
			S.AvgCost,
			S.RadiusMin,
			S.RadiusMax,
			S.RadiusAvg);

		SelectionStatsString = Text;

		// 明细行：每行按列拆成单元格（列定义见 GMeshTableColumns）
		for (const FMeshUsageStat& Mesh : S.TopMeshes)
		{
			TArray<FString> Cells;
			BuildMeshCells(Mesh, Cells);
			SelectionMeshRows.Add(MoveTemp(Cells));
		}
	}

	bSelectionStatsDirty = false;
}

FText SHistogramPanel::GetSelectionStatsText() const
{
	return FText::FromString(SelectionStatsString);
}

FText SHistogramPanel::GetSelectionMeshCell(int32 RowIndex, int32 ColIndex) const
{
	if (!SelectionMeshRows.IsValidIndex(RowIndex))
	{
		return FText::GetEmpty();
	}

	const TArray<FString>& Cells = SelectionMeshRows[RowIndex];
	return Cells.IsValidIndex(ColIndex) ? FText::FromString(Cells[ColIndex]) : FText::GetEmpty();
}

TSharedRef<SWidget> SHistogramPanel::BuildMeshTableHeader()
{
	TSharedRef<SHorizontalBox> HeaderRow = SNew(SHorizontalBox);

	for (int32 Col = 0; Col < GMeshTableColumnCount; ++Col)
	{
		const FMeshTableColumn& Column = GMeshTableColumns[Col];

		HeaderRow->AddSlot().AutoWidth()
		[
			SNew(SBox)
				.WidthOverride(Column.Width)
				.Padding(FMargin(0.f, 0.f, 6.f, 0.f))
			[
				SNew(STextBlock)
					.Text(FText::FromString(Column.Label))
					.Font(FCoreStyle::GetDefaultFontStyle("Mono", 8))
					.Justification(Column.bLeftAlign ? ETextJustify::Left : ETextJustify::Right)
					.ToolTipText(FText::FromString(Column.Tip))
			]
		];
	}

	return HeaderRow;
}

TSharedRef<SWidget> SHistogramPanel::BuildMeshRowWidget(int32 RowIndex)
{
	const TAttribute<EVisibility> RowVisibility = TAttribute<EVisibility>::CreateLambda([this, RowIndex]()
	{
		return SelectionMeshRows.IsValidIndex(RowIndex) ? EVisibility::Visible : EVisibility::Collapsed;
	});

	TSharedRef<SHorizontalBox> RowCells = SNew(SHorizontalBox);

	for (int32 Col = 0; Col < GMeshTableColumnCount; ++Col)
	{
		const FMeshTableColumn& Column = GMeshTableColumns[Col];

		RowCells->AddSlot().AutoWidth()
		[
			SNew(SBox)
				.WidthOverride(Column.Width)
				.Padding(FMargin(0.f, 0.f, 6.f, 0.f))
			[
				SNew(STextBlock)
					.Text(TAttribute<FText>::CreateLambda([this, RowIndex, Col]()
					{
						return GetSelectionMeshCell(RowIndex, Col);
					}))
					.Font(FCoreStyle::GetDefaultFontStyle("Mono", 8))
					.Justification(Column.bLeftAlign ? ETextJustify::Left : ETextJustify::Right)
			]
		];
	}

	return SNew(SButton)
		.ButtonStyle(FCoreStyle::Get(), "NoBorder")
		.ContentPadding(FMargin(0.f))
		.Visibility(RowVisibility)
		.ToolTipText(TAttribute<FText>::CreateLambda([this, RowIndex]()
		{
			return GetSelectionMeshRowTooltip(RowIndex);
		}))
		.OnClicked(FOnClicked::CreateLambda([this, RowIndex]()
		{
			return OnSelectionMeshRowClicked(RowIndex);
		}))
		[
			RowCells
		];
}

FText SHistogramPanel::GetSelectionMeshRowTooltip(int32 RowIndex) const
{
	if (!CachedSelectionStats.TopMeshes.IsValidIndex(RowIndex))
	{
		return FText::GetEmpty();
	}

	const FMeshUsageStat& Stat = CachedSelectionStats.TopMeshes[RowIndex];

	FString Text = FString::Printf(TEXT("网格：%s\n"), *Stat.MeshName);

	Text += (Stat.LightMapW > 0 && Stat.LightMapH > 0)
		? FString::Printf(TEXT("光照图尺寸：%d×%d px ｜ 光照图占用 %.0f KB\n"), Stat.LightMapW, Stat.LightMapH, Stat.LightMapKB)
		: FString(TEXT("光照图尺寸：无（没烘光照 / 未烘焙）\n"));

	if (Stat.bTextureScanned)
	{
		if (Stat.TextureW > 0 && Stat.TextureH > 0)
		{
			Text += FString::Printf(TEXT("材质贴图最大：%d×%d px ｜ 共 %d 张 ｜ 占用 %.0f KB\n"),
				Stat.TextureW, Stat.TextureH, Stat.TextureCount, Stat.TextureKB);
			Text += FString::Printf(TEXT("          来自：%s\n"), *Stat.TextureMaxOwner);
			Text += TEXT("          （占用几十 KB = 贴图还没被流送加载进来，不代表它真的小）\n");

			if (Stat.TextureTopList.Num() > 0)
			{
				Text += TEXT("材质用到的贴图（大的在前）：\n");
				const int32 ShowCount = FMath::Min(Stat.TextureTopList.Num(), 6);
				for (int32 Index = 0; Index < ShowCount; ++Index)
				{
					Text += FString::Printf(TEXT("  %s\n"), *Stat.TextureTopList[Index]);
				}
				if (Stat.TextureTopList.Num() > ShowCount)
				{
					Text += FString::Printf(TEXT("  …还有 %d 张，点「贴图诊断→日志」看全部\n"), Stat.TextureTopList.Num() - ShowCount);
				}
			}

			if (Stat.MaterialInfoList.Num() > 0)
			{
				Text += TEXT("使用的材质：\n");
				for (const FString& Line : Stat.MaterialInfoList)
				{
					Text += FString::Printf(TEXT("  %s\n"), *Line);
				}
			}

			// 「这个资产自己的贴图」vs「外部带进来的」
			const FString LocalSizeText = (Stat.TextureLocalW > 0 && Stat.TextureLocalH > 0)
				? FString::Printf(TEXT("%d×%d（%s）"), Stat.TextureLocalW, Stat.TextureLocalH, *Stat.TextureLocalName)
				: FString(TEXT("无（这个资产目录里没有它的贴图）"));
			Text += FString::Printf(TEXT("本资产（同目录 %s）贴图最大：%s\n"), *Stat.MeshFolder, *LocalSizeText);

			if (Stat.TextureForeignCount > 0)
			{
				Text += FString::Printf(TEXT("⚠ 其中 %d 张来自其他目录/引擎默认 —— 多半是共用母材质或导入器留下的，\n   不是这个资产自己的贴图（上面标了「不在本资产目录」的那几张）\n"),
					Stat.TextureForeignCount);
			}
		}
		else
		{
			Text += TEXT("材质贴图：无（这个网格的材质不用贴图，纯色/程序化）\n");
		}

		if (Stat.bTextureFallback)
		{
			Text += TEXT("⚠ 该材质还没编译出资源，这行是按「材质图里引用到的贴图」统计的，可能偏大\n");
		}
	}

	Text += FString::Printf(TEXT("包围球半径：%.1f~%.1f cm（平均 %.1f）—— 物体在场景里的实际尺寸\n"),
		Stat.RadiusMin,
		Stat.RadiusMax,
		Stat.GetAvgRadius());

	Text += FString::Printf(TEXT("资产：%s\n"), Stat.MeshAssetPath.IsValid() ? *Stat.MeshAssetPath.ToString() : TEXT("（未知）"));

	for (const FSoftObjectPath& BlueprintPath : Stat.BlueprintAssetPaths)
	{
		Text += FString::Printf(TEXT("蓝图：%s\n"), *BlueprintPath.ToString());
	}

	Text += TEXT("点击：在内容浏览器里浏览至该资产（等于右键「浏览至资产」）\n");
	Text += FString::Printf(TEXT("Ctrl+点击：在视口里选中并聚焦用到它的 %d 个 Actor"), Stat.Actors.Num());

	return FText::FromString(Text);
}

FReply SHistogramPanel::OnSelectionMeshRowClicked(int32 RowIndex)
{
	if (!CachedSelectionStats.TopMeshes.IsValidIndex(RowIndex) || !GEditor)
	{
		return FReply::Handled();
	}

	const FMeshUsageStat& Stat = CachedSelectionStats.TopMeshes[RowIndex];
	const FModifierKeysState Modifiers = FSlateApplication::Get().GetModifierKeys();

	// Ctrl（Mac 上 Cmd）+ 点击 = 旧行为：在视口里选中并聚焦用到它的 Actor
	if (Modifiers.IsControlDown() || Modifiers.IsCommandDown())
	{
		TArray<AActor*> Actors;
		for (const TWeakObjectPtr<AActor>& WeakActor : Stat.Actors)
		{
			if (WeakActor.IsValid())
			{
				Actors.Add(WeakActor.Get());
			}
		}

		if (Actors.Num() == 0)
		{
			if (StatusText.IsValid())
			{
				StatusText->SetText(LOCTEXT("MeshRowNoActor", "这个网格已经不在场景里了（Actor 可能被删掉）"));
				StatusText->SetColorAndOpacity(FLinearColor(1.f, 0.75f, 0.3f));
			}
			return FReply::Handled();
		}

		GEditor->SelectNone(false, true, false);
		for (AActor* Actor : Actors)
		{
			GEditor->SelectActor(Actor, true, true);
		}
		GEditor->MoveViewportCamerasToActor(Actors, /*bActiveViewportOnly=*/false);

		return FReply::Handled();
	}

	// 默认：浏览至资产（静态网格资产 + 用到它的蓝图资产）
	TArray<UObject*> AssetsToSync;

	auto AddAsset = [&AssetsToSync](const FSoftObjectPath& Path)
	{
		if (!Path.IsValid())
		{
			return;
		}

		UObject* Asset = Path.ResolveObject();
		if (!Asset)
		{
			Asset = Path.TryLoad();
		}

		if (Asset)
		{
			AssetsToSync.AddUnique(Asset);
		}
	};

	AddAsset(Stat.MeshAssetPath);
	for (const FSoftObjectPath& BlueprintPath : Stat.BlueprintAssetPaths)
	{
		AddAsset(BlueprintPath);
	}

	if (AssetsToSync.Num() == 0)
	{
		if (StatusText.IsValid())
		{
			StatusText->SetText(FText::FromString(FString::Printf(TEXT("找不到资产（可能已被删除或未加载）：%s"), *Stat.MeshName)));
			StatusText->SetColorAndOpacity(FLinearColor(1.f, 0.45f, 0.45f));
		}
		return FReply::Handled();
	}

	GEditor->SyncBrowserToObjects(AssetsToSync, /*bFocusContentBrowser=*/true);

	if (StatusText.IsValid())
	{
		FString Names;
		for (UObject* Asset : AssetsToSync)
		{
			Names += Names.IsEmpty() ? Asset->GetName() : FString::Printf(TEXT(" / %s"), *Asset->GetName());
		}

		StatusText->SetText(FText::FromString(FString::Printf(TEXT("已在内容浏览器中浏览至资产：%s"), *Names)));
		StatusText->SetColorAndOpacity(FLinearColor(0.45f, 1.f, 0.45f));
	}

	return FReply::Handled();
}

ECheckBoxState SHistogramPanel::GetShowMeshDetailsState() const
{
	return bShowMeshDetails ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void SHistogramPanel::OnShowMeshDetailsChanged(ECheckBoxState NewState)
{
	bShowMeshDetails = (NewState == ECheckBoxState::Checked);
}

EVisibility SHistogramPanel::GetMeshDetailsVisibility() const
{
	return bShowMeshDetails ? EVisibility::Visible : EVisibility::Collapsed;
}

FReply SHistogramPanel::OnDumpTextureDiagnostics()
{
	const FSelectionStats& S = CachedSelectionStats;

	UE_LOG(LogTemp, Display, TEXT("========== [HistogramAssistant] 贴图诊断开始 =========="));
	UE_LOG(LogTemp, Display, TEXT("选中：%s ｜ Actor %d ｜ 组件 %d ｜ 唯一网格 %d"),
		*S.Label, S.ActorCount, S.ComponentCount, S.UniqueMeshCount);
	UE_LOG(LogTemp, Display, TEXT("光照图最大尺寸 %d×%d px ｜ 材质贴图最大尺寸 %d×%d px ｜ 贴图占用 %.0f KB（Top12 内）"),
		S.LightMapMaxW, S.LightMapMaxH, S.TextureMaxW, S.TextureMaxH, S.TextureKB);

	for (int32 RowIndex = 0; RowIndex < S.TopMeshes.Num(); ++RowIndex)
	{
		const FMeshUsageStat& Stat = S.TopMeshes[RowIndex];

		UE_LOG(LogTemp, Display, TEXT("[%d] 网格 %s（%s）"),
			RowIndex, *Stat.MeshName, Stat.MeshAssetPath.IsValid() ? *Stat.MeshAssetPath.ToString() : TEXT("?"));
		UE_LOG(LogTemp, Display, TEXT("    贴图最大 %d×%d（%s）｜ 本资产目录 %s 内的最大贴图 %d×%d（%s）｜ 共 %d 张（外部目录 %d 张）｜ %.0f KB%s"),
			Stat.TextureW, Stat.TextureH, *Stat.TextureMaxName,
			*Stat.MeshFolder, Stat.TextureLocalW, Stat.TextureLocalH, *Stat.TextureLocalName,
			Stat.TextureCount, Stat.TextureForeignCount, Stat.TextureKB,
			Stat.bTextureFallback ? TEXT("  ⚠ 材质没编译出资源，用的是“引用的贴图”兜底口径") : TEXT(""));

		UE_LOG(LogTemp, Display, TEXT("    材质（组件实际使用）："));
		for (const FString& Line : Stat.MaterialInfoList)
		{
			UE_LOG(LogTemp, Display, TEXT("      %s"), *Line);
		}

		UE_LOG(LogTemp, Display, TEXT("    贴图（材质实际采样，大的在前）："));
		for (const FString& Line : Stat.TextureTopList)
		{
			UE_LOG(LogTemp, Display, TEXT("      %s"), *Line);
		}
	}

	UE_LOG(LogTemp, Display, TEXT("========== [HistogramAssistant] 贴图诊断结束（共 %d 行网格）=========="),
		S.TopMeshes.Num());

	if (StatusText.IsValid())
	{
		StatusText->SetText(LOCTEXT("TextureDiagDone", "贴图诊断已写入日志：Saved\\Logs\\xiaozhen.log（搜 HistogramAssistant）"));
		StatusText->SetColorAndOpacity(FLinearColor(0.45f, 1.f, 0.45f));
	}

	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
