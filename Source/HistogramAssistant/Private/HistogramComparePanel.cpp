#include "HistogramComparePanel.h"

#include "HistogramPanel.h"          // 复用 SFalseColorView（等比铺满的纹理视图）
#include "HistogramCapture.h"
#include "HistogramFalseColor.h"
#include "Misc/DateTime.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Text/STextBlock.h"
#include "RHI.h"
#include "Async/ParallelFor.h"

#define LOCTEXT_NAMESPACE "HistogramAssistantCompare"

void SFrameComparePanel::Construct(const FArguments& InArgs)
{
	RefView = SNew(SFalseColorView);
	CurView = SNew(SFalseColorView);
	SingleView = SNew(SFalseColorView);
	StatusText = SNew(STextBlock)
		.Text(this, &SFrameComparePanel::GetStatusText)
		.AutoWrapText(true);

	ChildSlot
	[
		SNew(SVerticalBox)
		// 工具条
		+ SVerticalBox::Slot().AutoHeight().Padding(4.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 8.f, 0.f)
			[
				SNew(SButton)
					.Text(LOCTEXT("StoreRef", "存参考帧"))
					.ToolTipText(LOCTEXT("StoreRefTip", "把当前画面存为参考帧（改灯光/后处理之前先点一下）"))
					.OnClicked(this, &SFrameComparePanel::OnStoreClicked)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 8.f, 0.f)
			[
				SNew(SButton)
					.Text(LOCTEXT("ClearRef", "清除参考"))
					.OnClicked(this, &SFrameComparePanel::OnClearClicked)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 8.f, 0.f)
			[
				SNew(SButton)
					.Text(this, &SFrameComparePanel::GetModeLabel)
					.ToolTipText(LOCTEXT("ModeTip", "点击切换：并排 → 擦除 → 差异"))
					.OnClicked(this, &SFrameComparePanel::OnCycleModeClicked)
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 4.f, 0.f)
			[
				SNew(STextBlock).Text(LOCTEXT("WipeLabel", "擦除位置"))
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(0.f, 0.f, 12.f, 0.f)
			[
				SNew(SSlider)
					.MinValue(0.f)
					.MaxValue(100.f)
					.Value(this, &SFrameComparePanel::GetWipePercent)
					.OnValueChanged(this, &SFrameComparePanel::OnWipePercentChanged)
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 4.f, 0.f)
			[
				SNew(STextBlock).Text(LOCTEXT("GainLabel", "差异放大"))
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(0.f, 0.f, 12.f, 0.f)
			[
				SNew(SSlider)
					.MinValue(1.f)
					.MaxValue(16.f)
					.Value(this, &SFrameComparePanel::GetDiffGain)
					.OnValueChanged(this, &SFrameComparePanel::OnDiffGainChanged)
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SCheckBox)
					.IsChecked(this, &SFrameComparePanel::GetAutoRefreshState)
					.OnCheckStateChanged(this, &SFrameComparePanel::OnAutoRefreshChanged)
					[
						SNew(STextBlock).Text(LOCTEXT("CompareAutoRefresh", "自动刷新（0.5 秒）"))
					]
			]
		]
		// 状态
		+ SVerticalBox::Slot().AutoHeight().Padding(4.f, 0.f, 4.f, 4.f)
		[
			StatusText.ToSharedRef()
		]
		// 并排：左参考 / 右当前
		+ SVerticalBox::Slot().FillHeight(1.f).Padding(4.f)
		[
			SNew(SHorizontalBox)
				.Visibility(this, &SFrameComparePanel::GetSideBySideVisibility)
			+ SHorizontalBox::Slot().FillWidth(1.f).Padding(2.f)
			[
				RefView.ToSharedRef()
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).Padding(2.f)
			[
				CurView.ToSharedRef()
			]
		]
		// 单图：擦除合成 / 差异热力图
		+ SVerticalBox::Slot().FillHeight(1.f).Padding(4.f)
		[
			SNew(SVerticalBox)
				.Visibility(this, &SFrameComparePanel::GetSingleVisibility)
			+ SVerticalBox::Slot().FillHeight(1.f)
			[
				SingleView.ToSharedRef()
			]
		]
	];

	DoCapture();   // 打开就有画面

	// 自己的刷新节拍
	RegisterActiveTimer(0.5f, FWidgetActiveTimerDelegate::CreateSP(this, &SFrameComparePanel::OnTick));
}

void SFrameComparePanel::UploadTexture(TStrongObjectPtr<UTexture2D>& Holder, const TArray<FColor>& Pixels, int32 Width, int32 Height)
{
	if (Width <= 0 || Height <= 0 || Pixels.Num() != Width * Height)
	{
		return;
	}

	UTexture2D* Target = Holder.Get();
	if (!Target || Target->GetSizeX() != Width || Target->GetSizeY() != Height)
	{
		UTexture2D* NewTexture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
		if (!NewTexture)
		{
			return;
		}

		NewTexture->SRGB = true;
		NewTexture->Filter = TF_Nearest;
		NewTexture->AddressX = TA_Clamp;
		NewTexture->AddressY = TA_Clamp;
		NewTexture->UpdateResource();

		Holder.Reset(NewTexture);
		Target = NewTexture;
	}

	const int32 SrcPitch = Width * (int32)sizeof(FColor);
	const int32 NumBytes = Pixels.Num() * (int32)sizeof(FColor);

	// 与伪彩/斑马纹同样的注意点：Regions 指针要活到渲染线程消费完
	FUpdateTextureRegion2D* RegionPtr = new FUpdateTextureRegion2D(0, 0, 0, 0, (uint32)Width, (uint32)Height);
	uint8* SrcData = (uint8*)FMemory::Malloc(NumBytes);
	FMemory::Memcpy(SrcData, Pixels.GetData(), NumBytes);

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

void SFrameComparePanel::DoCapture()
{
	FHistogramData Data;
	TArray<FColor> Pixels;
	if (!FHistogramCapture::CaptureActiveViewport(Data, &Pixels))
	{
		StatusMessage = TEXT("捕获失败：请确认编辑器里有一个激活的 3D 视口");
		return;
	}

	CurPixels = MoveTemp(Pixels);
	CurW = Data.Width;
	CurH = Data.Height;

	UploadTexture(CurTexture, CurPixels, CurW, CurH);
	if (CurView.IsValid())
	{
		CurView->SetTexture(CurTexture.Get());
	}

	RebuildSingle();

	StatusMessage = FString::Printf(TEXT("%dx%d   抓取 %s   %s"),
		CurW, CurH,
		*FDateTime::Now().ToString(TEXT("%H:%M:%S")),
		RefW > 0 ? TEXT("[已有参考帧]") : TEXT("[还没存参考帧 —— 改灯光前先点「存参考帧」]"));
}

void SFrameComparePanel::StoreReference()
{
	if (CurW <= 0 || CurH <= 0 || CurPixels.Num() != CurW * CurH)
	{
		DoCapture();   // 还没有帧就先抓一帧
	}

	if (CurW <= 0 || CurPixels.Num() != CurW * CurH)
	{
		StatusMessage = TEXT("存参考帧失败：没有可用画面");
		return;
	}

	RefPixels = CurPixels;
	RefW = CurW;
	RefH = CurH;

	UploadTexture(RefTexture, RefPixels, RefW, RefH);
	if (RefView.IsValid())
	{
		RefView->SetTexture(RefTexture.Get());
	}

	RebuildSingle();
	StatusMessage = FString::Printf(TEXT("已存参考帧 %dx%d    %s"), RefW, RefH, *FDateTime::Now().ToString(TEXT("%H:%M:%S")));
}

void SFrameComparePanel::ClearReference()
{
	RefPixels.Reset();
	RefW = 0;
	RefH = 0;
	RefTexture.Reset();
	if (RefView.IsValid())
	{
		RefView->SetTexture(nullptr);
	}
	RebuildSingle();
	StatusMessage = TEXT("参考帧已清除");
}

void SFrameComparePanel::RebuildSingle()
{
	if (!SingleView.IsValid())
	{
		return;
	}

	if (Mode == EFrameCompareMode::SideBySide || CurW <= 0 || RefW != CurW || RefH != CurH)
	{
		SingleView->SetTexture(nullptr);
		return;
	}

	TArray<FColor> Out;
	Out.SetNumUninitialized(CurPixels.Num());

	if (Mode == EFrameCompareMode::Wipe)
	{
		const int32 SplitX = FMath::Clamp(FMath::RoundToInt((float)CurW * WipePercent * 0.01f), 0, CurW);
		ParallelFor(CurH, [&](int32 Y)
		{
			const FColor* RefRow = RefPixels.GetData() + (int64)Y * CurW;
			const FColor* CurRow = CurPixels.GetData() + (int64)Y * CurW;
			FColor* DstRow = Out.GetData() + (int64)Y * CurW;

			for (int32 X = 0; X < SplitX; ++X)
			{
				DstRow[X] = RefRow[X];
			}
			for (int32 X = SplitX; X < CurW; ++X)
			{
				DstRow[X] = CurRow[X];
			}
		});
	}
	else // Diff
	{
		const float Gain = FMath::Clamp(DiffGainValue, 1.f, 32.f);

		TArray<FColor> Grey;
		Grey.SetNumUninitialized(CurPixels.Num());
		ParallelFor(CurPixels.Num(), [&](int32 Index)
		{
			const FColor& A = RefPixels[Index];
			const FColor& B = CurPixels[Index];
			const float LA = 0.2126f * A.R + 0.7152f * A.G + 0.0722f * A.B;
			const float LB = 0.2126f * B.R + 0.7152f * B.G + 0.0722f * B.B;
			const uint8 V = (uint8)FMath::Clamp(FMath::RoundToInt(FMath::Abs(LB - LA) * Gain), 0, 255);
			Grey[Index] = FColor(V, V, V, 255);
		});

		// 差值是标量 → 直接借连续色图（蓝=几乎没变 → 青 → 绿 → 黄 → 红=变化很大）
		FFalseColorProfile RampProfile = FFalseColorProfile::MakeDefault();
		RampProfile.Mode = EFalseColorMode::ContinuousRamp;
		FHistogramFalseColor::Apply(Grey, CurW, CurH, RampProfile, Out);
	}

	UploadTexture(SingleTexture, Out, CurW, CurH);
	SingleView->SetTexture(SingleTexture.Get());
}

FReply SFrameComparePanel::OnStoreClicked()
{
	StoreReference();
	return FReply::Handled();
}

FReply SFrameComparePanel::OnClearClicked()
{
	ClearReference();
	return FReply::Handled();
}

FReply SFrameComparePanel::OnCycleModeClicked()
{
	Mode = static_cast<EFrameCompareMode>((static_cast<int32>(Mode) + 1) % 3);
	RebuildSingle();
	return FReply::Handled();
}

FText SFrameComparePanel::GetModeLabel() const
{
	switch (Mode)
	{
	case EFrameCompareMode::Wipe:
		return LOCTEXT("ModeWipe", "模式：擦除");
	case EFrameCompareMode::Diff:
		return LOCTEXT("ModeDiff", "模式：差异");
	case EFrameCompareMode::SideBySide:
	default:
		return LOCTEXT("ModeSideBySide", "模式：并排");
	}
}

ECheckBoxState SFrameComparePanel::GetAutoRefreshState() const
{
	return bAutoRefresh ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void SFrameComparePanel::OnAutoRefreshChanged(ECheckBoxState NewState)
{
	bAutoRefresh = (NewState == ECheckBoxState::Checked);
	if (bAutoRefresh)
	{
		DoCapture();
	}
}

float SFrameComparePanel::GetWipePercent() const
{
	return WipePercent;
}

void SFrameComparePanel::OnWipePercentChanged(float NewValue)
{
	WipePercent = FMath::Clamp(NewValue, 0.f, 100.f);
	if (Mode == EFrameCompareMode::Wipe)
	{
		RebuildSingle();
	}
}

float SFrameComparePanel::GetDiffGain() const
{
	return DiffGainValue;
}

void SFrameComparePanel::OnDiffGainChanged(float NewValue)
{
	DiffGainValue = FMath::Clamp(NewValue, 1.f, 16.f);
	if (Mode == EFrameCompareMode::Diff)
	{
		RebuildSingle();
	}
}

EVisibility SFrameComparePanel::GetSideBySideVisibility() const
{
	return (Mode == EFrameCompareMode::SideBySide) ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SFrameComparePanel::GetSingleVisibility() const
{
	return (Mode == EFrameCompareMode::SideBySide) ? EVisibility::Collapsed : EVisibility::Visible;
}

FText SFrameComparePanel::GetStatusText() const
{
	return FText::FromString(StatusMessage);
}

EActiveTimerReturnType SFrameComparePanel::OnTick(double InCurrentTime, float InDeltaTime)
{
	if (bAutoRefresh)
	{
		DoCapture();
	}
	return EActiveTimerReturnType::Continue;
}

#undef LOCTEXT_NAMESPACE
