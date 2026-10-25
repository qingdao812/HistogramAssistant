#include "HistogramFalseColor.h"

#include "Async/ParallelFor.h"

FFalseColorProfile FFalseColorProfile::MakeDefault()
{
	// 对照 RED 官方 False Color Video Mode（IRE）。
	// 关键：真机是「只在关键档着色，其余亮度显示**去饱和灰度图**」——RED 原文：
	//   "at all other values, the desaturated image represents the luminance value"
	//
	// 2026-09-18 修订：原先把 IRE 10–38 这 28 个 IRE 单位整段涂成扁平青色，
	// 于是画面里大量中暗面（实测占视口 39.6%）被涂成同一块暗青，看着像「亮面发黑」。
	// 现在青只保留 10–12.5 这段真正的深阴影标记，其余交给灰度 —— 和真机一致。
	FFalseColorProfile Profile;

	auto AddBand = [&Profile](float MaxIRE, const FColor& Color, bool bKeepOriginal, const TCHAR* Meaning)
	{
		FFalseColorBand Band;
		Band.MaxIRE = MaxIRE;
		Band.Color = Color;
		Band.bKeepOriginal = bKeepOriginal;
		Band.Meaning = Meaning;
		Profile.Bands.Add(Band);
	};

	AddBand(  4.0f, FColor(150,  60, 200), false, TEXT("死黑"));      // 紫
	AddBand( 10.0f, FColor(  0,  90, 255), false, TEXT("即将死黑"));  // 蓝
	AddBand( 12.5f, FColor(  0, 165, 165), false, TEXT("深阴影"));    // 青
	AddBand( 41.0f, FColor::White,          true,  TEXT("暗部"));      // 灰度
	AddBand( 48.0f, FColor(  0, 220,  60), false, TEXT("18%中灰"));   // 绿
	AddBand( 61.0f, FColor::White,          true,  TEXT("中间调"));    // 灰度
	AddBand( 70.0f, FColor(255, 105, 180), false, TEXT("浅肤色亮度"));      // 粉
	AddBand( 92.0f, FColor::White,          true,  TEXT("高光前"));    // 灰度
	AddBand( 93.5f, FColor(240, 230, 140), false, TEXT("强高光"));    // 麦秆
	AddBand( 95.5f, FColor(255, 240,   0), false, TEXT("高光"));      // 黄
	AddBand( 98.5f, FColor(255, 130,   0), false, TEXT("接近削波"));  // 橙
	AddBand(100.1f, FColor(255,   0,   0), false, TEXT("削波"));      // 红

	Profile.bDesaturateOthers = true;   // 非关键档显示去饱和灰度（真机做法）

	// ---- 连续色图（CLUT）：整幅画面按亮度着色，蓝 → 青 → 绿 → 黄 → 红 ----
	// 与"档位"模式互补：色图看结构和明暗层次，档位判曝光绝对位置。
	{
		struct FRampStop { float T; FColor C; };
		const FRampStop Stops[] =
		{
			{ 0.00f, FColor(  0,   0, 255) },
			{ 0.25f, FColor(  0, 190, 255) },
			{ 0.50f, FColor(  0, 255,  90) },
			{ 0.75f, FColor(255, 255,   0) },
			{ 1.00f, FColor(255,   0,   0) },
		};
		const int32 NumStops = UE_ARRAY_COUNT(Stops);

		Profile.Ramp.SetNum(256);
		for (int32 i = 0; i < 256; ++i)
		{
			const float T = (float)i / 255.f;

			int32 s = 0;
			while (s + 2 < NumStops && T > Stops[s + 1].T)
			{
				++s;
			}

			const float Span = FMath::Max(KINDA_SMALL_NUMBER, Stops[s + 1].T - Stops[s].T);
			const float A = FMath::Clamp((T - Stops[s].T) / Span, 0.f, 1.f);

			Profile.Ramp[i] = FColor(
				(uint8)FMath::Clamp(FMath::RoundToInt(FMath::Lerp((float)Stops[s].C.R, (float)Stops[s + 1].C.R, A)), 0, 255),
				(uint8)FMath::Clamp(FMath::RoundToInt(FMath::Lerp((float)Stops[s].C.G, (float)Stops[s + 1].C.G, A)), 0, 255),
				(uint8)FMath::Clamp(FMath::RoundToInt(FMath::Lerp((float)Stops[s].C.B, (float)Stops[s + 1].C.B, A)), 0, 255),
				255);
		}
	}

	return Profile;
}

float FHistogramFalseColor::IREOf(const FColor& C)
{
	// ReadPixels 得到的 FColor 是显示域(sRGB 编码)的 8bit 值 —— 正是 IRE 的定义域，
	// 因此这里的亮度不需要做任何 gamma 变换即可与 IRE 直接比较。
	const float Luma = (0.2126f * C.R + 0.7152f * C.G + 0.0722f * C.B) / 255.f;
	return Luma * 100.f;
}

int32 FHistogramFalseColor::BandIndexForIRE(float IRE, const FFalseColorProfile& Profile)
{
	const int32 Num = Profile.Bands.Num();
	for (int32 i = 0; i < Num; ++i)
	{
		if (IRE < Profile.Bands[i].MaxIRE)
		{
			return i;
		}
	}
	return Num > 0 ? Num - 1 : INDEX_NONE;
}

void FHistogramFalseColor::Apply(const TArray<FColor>& SrcPixels, int32 Width, int32 Height, const FFalseColorProfile& Profile, TArray<FColor>& OutPixels)
{
	const int32 Num = SrcPixels.Num();
	OutPixels.SetNumUninitialized(Num);

	if (Num == 0 || Profile.Bands.Num() == 0 || Width <= 0 || Height <= 0 || Num != Width * Height)
	{
		return;
	}

	const FColor* Src = SrcPixels.GetData();
	FColor* Dst = OutPixels.GetData();
	const bool bDesaturate = Profile.bDesaturateOthers;
	const int32 Block = FMath::Clamp(Profile.SampleBlockSize, 1, 64);

	// 可选：先按 Block×Block 求「块平均 IRE」，再用块平均去分档。
	// 逐像素硬边界会让相邻像素在档位之间来回跳 → 彩色噪点（带高频纹理的表面最明显）。
	// 用块平均后变成大块干净的色域，更像监视器上那种一眼可读的伪彩。
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
				BlockIRE[By * BlockW + (X / Block)] += FHistogramFalseColor::IREOf(Src[Y * Width + X]);
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

	// 2K 视口 ~2M 像素，ParallelFor 后 1~3ms 量级
	ParallelFor(Num, [Src, Dst, &Profile, bDesaturate, Block, BlockW, Width, &BlockIRE](int32 Index)
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
			IRE = FHistogramFalseColor::IREOf(C);
		}

		const int32 BandIndex = FHistogramFalseColor::BandIndexForIRE(IRE, Profile);

		// 连续色图模式（CLUT 查表）：整幅画面都有颜色，蓝=最暗 → 青 → 绿 → 黄 → 白=最亮
		if (Profile.Mode == EFalseColorMode::ContinuousRamp && Profile.Ramp.Num() >= 256)
		{
			Dst[Index] = Profile.Ramp[FMath::Clamp(FMath::RoundToInt(IRE * 2.55f), 0, 255)];
			return;
		}

		// ★ 一律强制 A=255：ReadPixels 抓回来的像素 alpha=0（编辑器视口的 alpha 无意义），
		//   如果沿用原 alpha，非关键档的像素会变成"完全透明"，Slate 直接把底下真实场景透出来 ——
		//   表现为"只有关键档有色块、模型表面没被覆盖"（2026-09-18 踩过，2026-09-18 修）。
		if (BandIndex == INDEX_NONE)
		{
			Dst[Index] = FColor(C.R, C.G, C.B, 255);
			return;
		}

		const FFalseColorBand& Band = Profile.Bands[BandIndex];
		if (Band.bKeepOriginal)
		{
			if (bDesaturate)
			{
				const uint8 Gray = (uint8)FMath::Clamp(FMath::RoundToInt(0.2126f * C.R + 0.7152f * C.G + 0.0722f * C.B), 0, 255);
				Dst[Index] = FColor(Gray, Gray, Gray, 255);
			}
			else
			{
				Dst[Index] = FColor(C.R, C.G, C.B, 255);
			}
		}
		else
		{
			Dst[Index] = Band.Color;   // FColor(r,g,b) 默认 A=255，本来就不透明
		}
	});
}
