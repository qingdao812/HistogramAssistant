#pragma once

#include "CoreMinimal.h"

class APostProcessVolume;

struct FHistogramData
{
	int32 Width = 0;
	int32 Height = 0;

	float AvgLuma = 0.f;
	float MinLuma = 0.f;
	float MaxLuma = 0.f;

	float AvgR = 0.f;
	float AvgG = 0.f;
	float AvgB = 0.f;

	float ShadowRatio = 0.f;
	float MidtoneRatio = 0.f;
	float HighlightRatio = 0.f;

	TArray<int32> LumaHist;
	TArray<int32> RHist;
	TArray<int32> GHist;
	TArray<int32> BHist;

	int32 MaxHistCount = 1;
};

class FHistogramCapture
{
public:
	// 抓取活动视口。
	// OutPixels 非空时，额外输出原始显示域像素（sRGB 8bit，与直方图统计同源）供伪彩色/导出使用；
	// 传 nullptr（默认）时零额外开销。
	static bool CaptureActiveViewport(FHistogramData& OutData, TArray<FColor>* OutPixels = nullptr);

	// 在活动视口对应的世界里生成/删除一个临时 PostProcessVolume，把饱和度压到 0（黑白预览）
	static bool SetGrayscalePreview(bool bEnabled);
	static bool IsGrayscalePreviewEnabled();
};
