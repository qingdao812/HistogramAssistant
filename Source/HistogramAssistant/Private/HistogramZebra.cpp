#include "HistogramZebra.h"

#include "Async/ParallelFor.h"

float FHistogramZebra::IREOf(const FColor& C)
{
	// 与伪彩/面板统计同一口径：显示域(sRGB 8bit) → IRE%
	return (0.2126f * C.R + 0.7152f * C.G + 0.0722f * C.B) / 255.f * 100.f;
}

void FHistogramZebra::MakeOverlay(const TArray<FColor>& SrcPixels, int32 Width, int32 Height, const FZebraSettings& Settings, TArray<FColor>& OutRgba)
{
	const int32 Num = SrcPixels.Num();
	OutRgba.SetNumUninitialized(Num);

	if (Num == 0 || Width <= 0 || Height <= 0 || Num != Width * Height)
	{
		return;
	}

	// 先全部清成透明（alpha=0）：只有命中阈值的像素才不透明，
	// 这样叠到视口上时实时画面照常显示，斜纹像贴在上面。
	FMemory::Memset(OutRgba.GetData(), 0, (SIZE_T)Num * sizeof(FColor));

	const FColor* Src = SrcPixels.GetData();
	FColor* Dst = OutRgba.GetData();

	const float HighIRE = FMath::Clamp(Settings.HighIRE, 0.f, 100.f);
	const float LowIRE = FMath::Clamp(Settings.LowIRE, 0.f, 100.f);
	const bool bUseLow = Settings.bUseLow && (LowIRE > 0.f);
	const int32 Stripe = FMath::Max(2, Settings.StripeWidth);
	const int32 Block = FMath::Clamp(Settings.SampleBlockSize, 1, 64);

	// 阈值判定也用块平均亮度：和伪彩一样，避免边界处逐像素闪烁的碎纹
	TArray<float> BlockIRE;
	int32 BlockW = 0;
	int32 BlockH = 0;
	if (Block > 1)
	{
		BlockW = (Width + Block - 1) / Block;
		BlockH = (Height + Block - 1) / Block;
		BlockIRE.SetNumZeroed(BlockW * BlockH);

		for (int32 Y = 0; Y < Height; ++Y)
		{
			const int32 By = Y / Block;
			for (int32 X = 0; X < Width; ++X)
			{
				BlockIRE[By * BlockW + (X / Block)] += FHistogramZebra::IREOf(Src[Y * Width + X]);
			}
		}

		for (int32 By = 0; By < BlockH; ++By)
		{
			for (int32 Bx = 0; Bx < BlockW; ++Bx)
			{
				const int32 X0 = Bx * Block;
				const int32 Y0 = By * Block;
				const int32 W = FMath::Max(1, FMath::Min(X0 + Block, Width) - X0);
				const int32 H = FMath::Max(1, FMath::Min(Y0 + Block, Height) - Y0);
				BlockIRE[By * BlockW + Bx] /= (float)(W * H);
			}
		}
	}

	ParallelFor(Num, [Src, Dst, Width, Block, BlockW, HighIRE, LowIRE, bUseLow, Stripe, &BlockIRE](int32 Index)
	{
		const FColor& C = Src[Index];

		float IRE;
		if (Block > 1)
		{
			const int32 Bx = (Index % Width) / Block;
			const int32 By = (Index / Width) / Block;
			IRE = BlockIRE[By * BlockW + Bx];
		}
		else
		{
			IRE = FHistogramZebra::IREOf(C);
		}

		const bool bOver = (IRE >= HighIRE);
		const bool bUnder = bUseLow && (IRE <= LowIRE);
		if (!bOver && !bUnder)
		{
			return;   // 保持透明
		}

		// 45° 斜纹：亮/暗两块交替
		const int32 X = Index % Width;
		const int32 Y = Index / Width;
		const bool bBright = (((X + Y) / Stripe) % 2) == 0;

		Dst[Index] = bOver
			? (bBright ? FColor(255, 40, 40, 255) : FColor(96, 0, 0, 255))     // 削波：红斜纹
			: (bBright ? FColor(70, 140, 255, 255) : FColor(0, 24, 96, 255));  // 死黑：蓝斜纹
	});
}
