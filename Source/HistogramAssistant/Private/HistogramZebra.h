// HistogramAssistant - 斑马纹（Zebra）曝光预警
//
// 相机/监视器上的经典功能：超过上限（削波）或低于下限（死黑）的像素画斜纹。
// 本实现的特点：**只画命中阈值的像素，其余 alpha=0（全透明）** ——
// 所以把它叠到视口上时，实时画面照常显示，斜纹像"贴"在上面；
// 也因为它大部分透明，可以和伪彩色覆盖层同时开（Z 序更高即可）。

#pragma once

#include "CoreMinimal.h"

struct FZebraSettings
{
	/** 削波预警上限（IRE，≥ 该值画红斜纹） */
	float HighIRE = 95.f;

	/** 死黑预警下限（IRE，≤ 该值画蓝斜纹） */
	float LowIRE = 5.f;

	/** 是否启用死黑预警 */
	bool bUseLow = true;

	/** 斜纹宽度（像素） */
	int32 StripeWidth = 10;

	/** 阈值判定用的块平均大小（1 = 逐像素；>1 更稳定、不闪） */
	int32 SampleBlockSize = 8;
};

class FHistogramZebra
{
public:
	/** 生成斑马纹覆盖层（BGRA + alpha，未命中的像素 alpha=0） */
	static void MakeOverlay(const TArray<FColor>& SrcPixels, int32 Width, int32 Height, const FZebraSettings& Settings, TArray<FColor>& OutRgba);

	/** FColor(显示域 sRGB 8bit) → IRE% */
	static float IREOf(const FColor& C);
};
