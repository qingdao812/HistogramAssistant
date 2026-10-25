// HistogramAssistant - 帧对比（A/B）窗口
//
// 独立的 nomad tab（可拖到副屏，编辑器布局会记住位置）。
// 自带抓图与自动刷新，**不依赖主面板**：主面板关掉后它照样能用。
//
// 三种呈现：
//   并排：左=参考帧、右=当前帧（两张等大图）
//   擦除：一张图里按百分比左右拼接（左参考 / 右当前）
//   差异：|当前 − 参考| 按亮度放大后走连续色图（蓝=几乎没变 → 红=变化很大）
//
// 注意：窗口里的"当前帧"是**快照**（默认 0.5 秒刷新），不是实时画面 ——
// 实时只有"贴在 3D 视口上的覆盖层"才能做到。副屏对比的取舍就在这里。

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Engine/Texture2D.h"
#include "UObject/StrongObjectPtr.h"

class SFalseColorView;
class SCheckBox;
class SSlider;
class STextBlock;

enum class EFrameCompareMode : uint8
{
	SideBySide,	// 并排
	Wipe,		// 擦除
	Diff		// 差异
};

class SFrameComparePanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SFrameComparePanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	// ---- 逻辑 ----
	void DoCapture();
	void StoreReference();
	void ClearReference();
	void RebuildSingle();
	static void UploadTexture(TStrongObjectPtr<UTexture2D>& Holder, const TArray<FColor>& Pixels, int32 Width, int32 Height);

	// ---- UI 回调 ----
	FReply OnStoreClicked();
	FReply OnClearClicked();
	FReply OnCycleModeClicked();
	FText GetModeLabel() const;
	ECheckBoxState GetAutoRefreshState() const;
	void OnAutoRefreshChanged(ECheckBoxState NewState);
	float GetWipePercent() const;
	void OnWipePercentChanged(float NewValue);
	float GetDiffGain() const;
	void OnDiffGainChanged(float NewValue);
	EVisibility GetSideBySideVisibility() const;
	EVisibility GetSingleVisibility() const;
	FText GetStatusText() const;
	EActiveTimerReturnType OnTick(double InCurrentTime, float InDeltaTime);

	// ---- 视图 ----
	TSharedPtr<SFalseColorView> RefView;
	TSharedPtr<SFalseColorView> CurView;
	TSharedPtr<SFalseColorView> SingleView;
	TSharedPtr<STextBlock> StatusText;

	// ---- 数据 ----
	TArray<FColor> RefPixels;
	int32 RefW = 0;
	int32 RefH = 0;

	TArray<FColor> CurPixels;
	int32 CurW = 0;
	int32 CurH = 0;

	TStrongObjectPtr<UTexture2D> RefTexture;
	TStrongObjectPtr<UTexture2D> CurTexture;
	TStrongObjectPtr<UTexture2D> SingleTexture;

	EFrameCompareMode Mode = EFrameCompareMode::SideBySide;
	float WipePercent = 50.f;	// 0~100
	float DiffGainValue = 4.f;	// 1~16
	bool bAutoRefresh = true;
	FString StatusMessage;
};
