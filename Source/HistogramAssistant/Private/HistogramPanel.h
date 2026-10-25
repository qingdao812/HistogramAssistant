#pragma once

#include "CoreMinimal.h"
#include "Input/Reply.h"
#include "Framework/Application/SlateApplication.h"
#include "Styling/SlateTypes.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SLeafWidget.h"
#include "Layout/Visibility.h"
#include "Engine/Texture2D.h"
#include "UObject/StrongObjectPtr.h"
#include "HistogramCapture.h"
#include "HistogramFalseColor.h"
#include "HistogramZebra.h"
#include "HistogramSelectionStats.h"

class STextBlock;
class SCheckBox;
class SSlider;
class IAssetViewport;

enum class EHistogramViewMode : uint8
{
	Combined,	// 综合：R / G / B / 亮度四条曲线叠加
	SplitRGB,	// 独立：R、G、B 三个通道分三行显示
	LumaOnly	// 亮度：只看明暗
};

class SHistogramGraph : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SHistogramGraph) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	void SetData(const FHistogramData& InData);
	void SetViewMode(EHistogramViewMode InMode);

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override;

private:
	FHistogramData Data;
	EHistogramViewMode ViewMode = EHistogramViewMode::Combined;
	bool bHasData = false;
};

/**
 * 伪彩色显示区。
 * 不用 SImage 的原因：SImage 会把画刷拉伸填满整个槽位 → 画面变形；
 * 而且它必须在构造时用 ImageSize 定死尺寸，没法跟着面板宽度走。
 * 这里自己按"等比铺满（contain）+ 居中"绘制，面板拉多宽它就能用多大。
 */
class SFalseColorView : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SFalseColorView) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** 设置（或传 nullptr 清空）要显示的纹理；内部持强引用，避免被 GC */
	void SetTexture(UTexture2D* InTexture);
	UTexture2D* GetTexture() const { return Texture.Get(); }

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override;

private:
	FSlateBrush Brush;
	TStrongObjectPtr<UTexture2D> Texture;
};

class SHistogramPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHistogramPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SHistogramPanel() override;

private:
	FReply OnCaptureClicked();
	FReply OnExportClicked();
	FReply OnOpenCompareClicked();
	void OnAutoRefreshChanged(ECheckBoxState NewState);
	FReply OnCycleViewModeClicked();
	FReply OnCycleFalseColorMode();
	FText GetFalseColorModeLabel() const;
	FText GetFalseColorLegend() const;
	FText GetViewModeLabel() const;
	void OnGrayscaleChanged(ECheckBoxState NewState);
	void OnFalseColorChanged(ECheckBoxState NewState);
	void OnOverlayChanged(ECheckBoxState NewState);
	void OnSmoothChanged(ECheckBoxState NewState);
	void OnZebraChanged(ECheckBoxState NewState);
	void AddZebraOverlay();
	void RemoveZebraOverlay();
	void UpdateZebraTexture(const TArray<FColor>& Pixels, int32 Width, int32 Height);
	float GetZebraHighIRE() const;
	float GetZebraLowIRE() const;
	void OnZebraHighChanged(float NewValue);
	void OnZebraLowChanged(float NewValue);
	void OnEditorSelectionChanged(UObject* NewSelection);
	void RefreshSelectionStats();
	FText GetSelectionStatsText() const;
	/** 明细表：单元格文本（列定义在 .cpp 的 GMeshTableColumns，中文表头 + 提示） */
	FText GetSelectionMeshCell(int32 RowIndex, int32 ColIndex) const;
	/** 明细行提示：显示这一行对应的资产路径与点击行为 */
	FText GetSelectionMeshRowTooltip(int32 RowIndex) const;
	TSharedRef<SWidget> BuildMeshTableHeader();
	TSharedRef<SWidget> BuildMeshRowWidget(int32 RowIndex);
	FReply OnSelectionMeshRowClicked(int32 RowIndex);
	ECheckBoxState GetShowMeshDetailsState() const;
	void OnShowMeshDetailsChanged(ECheckBoxState NewState);
	EVisibility GetMeshDetailsVisibility() const;
	/** 把「材质 → 贴图」明细写进日志（Saved/Logs/xiaozhen.log），排查贴图数字用 */
	FReply OnDumpTextureDiagnostics();
	EActiveTimerReturnType OnAutoRefreshTick(double InCurrentTime, float InDeltaTime);
	void DoCapture();
	void UpdateFalseColorImage(const TArray<FColor>& Pixels, int32 Width, int32 Height);
	void ClearFalseColorImage();
	void AddViewportOverlay();
	void RemoveViewportOverlay();
	EVisibility GetFalseColorVisibility() const;
	EVisibility GetHistogramVisibility() const;
	FString BuildBandReport(const FHistogramData& Data) const;
	FText BuildStatsText(const FHistogramData& Data) const;
	FText BuildDiagnosisText(const FHistogramData& Data) const;

	TSharedPtr<SHistogramGraph> GraphWidget;
	TSharedPtr<STextBlock> StatsText;
	TSharedPtr<STextBlock> DiagnosisText;
	TSharedPtr<STextBlock> StatusText;
	TSharedPtr<SCheckBox> GrayscaleCheckBox;
	TSharedPtr<SCheckBox> FalseColorCheckBox;
	TSharedPtr<SCheckBox> OverlayCheckBox;
	TSharedPtr<SCheckBox> SmoothCheckBox;
	/** 上次选择的导出目录（本次编辑器会话内记住） */
	FString LastExportDirectory;
	bool bAutoRefresh = false;
	bool bGrayscalePreview = false;
	EHistogramViewMode ViewMode = EHistogramViewMode::Combined;

	// ---- 伪彩色曝光（纯 CPU：抓图 → 上色 → 面板显示，不参与渲染）----
	bool bFalseColorPreview = false;
	FFalseColorProfile FalseColorProfile;
	TSharedPtr<SFalseColorView> FalseColorView;
	int32 FalseColorTexW = 0;
	int32 FalseColorTexH = 0;

	// ---- 伪彩大图：叠加到 3D 视口（IAssetViewport::AddOverlayWidget）----
	// 默认不叠加：面板小图为主，用户显式勾选才铺满视口
	bool bViewportOverlay = false;
	/** true = 分档前按块取平均亮度（大块色域、无彩色噪点）；默认开，用户可关 */
	bool bSmoothBands = true;
	TSharedPtr<SFalseColorView> OverlayView;
	TWeakPtr<IAssetViewport> OverlayViewport;

	// ---- 斑马纹（曝光预警）：独立的视口覆盖层，只有超阈值的像素不透明 ----
	TSharedPtr<SCheckBox> ZebraCheckBox;
	TSharedPtr<SSlider> ZebraHighSlider;
	TSharedPtr<SSlider> ZebraLowSlider;
	bool bZebraEnabled = false;
	FZebraSettings ZebraSettings;
	TSharedPtr<SFalseColorView> ZebraView;
	TWeakPtr<IAssetViewport> ZebraViewport;
	int32 ZebraTexW = 0;
	int32 ZebraTexH = 0;

	// ---- 选中对象资源统计（跟随编辑器选择）----
	TSharedPtr<STextBlock> SelectionStatsText;
	TSharedPtr<SWidget> SelectionTableHeader;
	FSelectionStats CachedSelectionStats;
	/** 明细行 = 每行的各列文本（长度与 GMeshTableColumns 一致） */
	TArray<TArray<FString>> SelectionMeshRows;
	FString SelectionStatsString;
	bool bSelectionStatsDirty = true;
	bool bShowMeshDetails = false;
};
