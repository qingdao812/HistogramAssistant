#include "HistogramExport.h"

#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Async/ParallelFor.h"

namespace
{
	void PlotPixel(TArray<FColor>& Img, int32 W, int32 H, int32 X, int32 Y, const FColor& C)
	{
		if (X < 0 || Y < 0 || X >= W || Y >= H)
		{
			return;
		}
		Img[Y * W + X] = C;
	}

	void FillRect(TArray<FColor>& Img, int32 W, int32 H, int32 X0, int32 Y0, int32 X1, int32 Y1, const FColor& C)
	{
		for (int32 Y = FMath::Max(0, Y0); Y < FMath::Min(H, Y1); ++Y)
		{
			for (int32 X = FMath::Max(0, X0); X < FMath::Min(W, X1); ++X)
			{
				Img[Y * W + X] = C;
			}
		}
	}

	void DrawLine(TArray<FColor>& Img, int32 W, int32 H, float X0, float Y0, float X1, float Y1, const FColor& C, int32 Thickness = 1)
	{
		const int32 Steps = FMath::Max(1, FMath::CeilToInt(FMath::Max(FMath::Abs(X1 - X0), FMath::Abs(Y1 - Y0))));
		for (int32 i = 0; i <= Steps; ++i)
		{
			const float T = (float)i / (float)Steps;
			const int32 X = FMath::RoundToInt(FMath::Lerp(X0, X1, T));
			const int32 Y = FMath::RoundToInt(FMath::Lerp(Y0, Y1, T));
			for (int32 dy = 0; dy < Thickness; ++dy)
			{
				for (int32 dx = 0; dx < Thickness; ++dx)
				{
					PlotPixel(Img, W, H, X + dx, Y + dy, C);
				}
			}
		}
	}

	/** 画直方图（与面板一致：对数归一化 + 平均亮度黄线）。尺寸由调用方给，直接铺满它的格子 */
	void MakeHistogramImage(const FHistogramData& D, int32 OutW, int32 OutH, TArray<FColor>& Out)
	{
		OutW = FMath::Max(64, OutW);
		OutH = FMath::Max(64, OutH);
		Out.Init(FColor(6, 6, 9), OutW * OutH);

		const int32 Pad = FMath::Max(8, OutW / 48);
		const int32 GW = OutW - Pad * 2;
		const int32 GH = OutH - Pad * 2 - FMath::Max(16, OutH / 24);

		for (int32 g = 0; g <= 4; ++g)
		{
			const int32 Y = Pad + GH * g / 4;
			DrawLine(Out, OutW, OutH, (float)Pad, (float)Y, (float)(Pad + GW), (float)Y, FColor(38, 38, 46));
		}

		const double MaxLog = FMath::Max(1.0, FMath::Loge(1.0 + (double)FMath::Max(1, D.MaxHistCount)));

		struct FCurve
		{
			const TArray<int32>* Hist;
			FColor Color;
			int32 Thickness;
		};

		// 线宽跟着画布尺寸走，避免大画布上曲线细得看不见
		const int32 Thin = FMath::Max(2, OutW / 340);
		const int32 Thick = FMath::Max(3, OutW / 240);

		const FCurve Curves[] =
		{
			{ &D.RHist,    FColor(255,  70,  70), Thin },
			{ &D.GHist,    FColor( 80, 255,  80), Thin },
			{ &D.BHist,    FColor( 95, 145, 255), Thin },
			{ &D.LumaHist, FColor(255, 255, 255), Thick },
		};

		for (const FCurve& Curve : Curves)
		{
			const int32 N = FMath::Min(Curve.Hist->Num(), 256);
			for (int32 i = 0; i < N - 1; ++i)
			{
				const double V0 = FMath::Loge(1.0 + (double)(*Curve.Hist)[i]) / MaxLog;
				const double V1 = FMath::Loge(1.0 + (double)(*Curve.Hist)[i + 1]) / MaxLog;
				const float X0 = (float)Pad + (float)GW * (float)i / 255.f;
				const float X1 = (float)Pad + (float)GW * (float)(i + 1) / 255.f;
				const float Y0 = (float)Pad + (float)GH * (1.f - (float)V0);
				const float Y1 = (float)Pad + (float)GH * (1.f - (float)V1);
				DrawLine(Out, OutW, OutH, X0, Y0, X1, Y1, Curve.Color, Curve.Thickness);
			}
		}

		// 平均亮度参考线
		const float AvgX = (float)Pad + (float)GW * FMath::Clamp(D.AvgLuma / 255.f, 0.f, 1.f);
		DrawLine(Out, OutW, OutH, AvgX, (float)Pad, AvgX, (float)(Pad + GH), FColor(255, 230, 60), 3);

		// 横轴两端刻度块（左=暗、右=亮；图上不打字，避免任何文字）
		FillRect(Out, OutW, OutH, Pad, Pad + GH + 6, Pad + 28, Pad + GH + 12, FColor(150, 150, 150));
		FillRect(Out, OutW, OutH, Pad + GW - 28, Pad + GH + 6, Pad + GW, Pad + GH + 12, FColor(150, 150, 150));
	}

	/** 双线性采样（U/V 取 0~1） */
	FColor SampleBilinear(const TArray<FColor>& Src, int32 SW, int32 SH, float U, float V)
	{
		const float FX = FMath::Clamp(U * (float)SW - 0.5f, 0.f, (float)(SW - 1));
		const float FY = FMath::Clamp(V * (float)SH - 0.5f, 0.f, (float)(SH - 1));
		const int32 X0 = FMath::FloorToInt(FX);
		const int32 Y0 = FMath::FloorToInt(FY);
		const int32 X1 = FMath::Min(X0 + 1, SW - 1);
		const int32 Y1 = FMath::Min(Y0 + 1, SH - 1);
		const float TX = FX - (float)X0;
		const float TY = FY - (float)Y0;

		const FColor& C00 = Src[Y0 * SW + X0];
		const FColor& C10 = Src[Y0 * SW + X1];
		const FColor& C01 = Src[Y1 * SW + X0];
		const FColor& C11 = Src[Y1 * SW + X1];

		auto LerpChannel = [TX, TY](uint8 A, uint8 B, uint8 C, uint8 D) -> uint8
		{
			const float Top = FMath::Lerp((float)A, (float)B, TX);
			const float Bot = FMath::Lerp((float)C, (float)D, TX);
			return (uint8)FMath::Clamp(FMath::RoundToInt(FMath::Lerp(Top, Bot, TY)), 0, 255);
		};

		return FColor(
			LerpChannel(C00.R, C10.R, C01.R, C11.R),
			LerpChannel(C00.G, C10.G, C01.G, C11.G),
			LerpChannel(C00.B, C10.B, C01.B, C11.B),
			255);
	}

	/** 把 Src 等比缩放后画进 Dst 的 (X0,Y0,CW,CH) 格子里（居中留边） */
	void BlitFit(const TArray<FColor>& Src, int32 SW, int32 SH, TArray<FColor>& Dst, int32 DW, int32 DH, int32 X0, int32 Y0, int32 CW, int32 CH)
	{
		if (SW <= 0 || SH <= 0 || CW <= 0 || CH <= 0 || Src.Num() != SW * SH)
		{
			return;
		}

		const float FitScale = FMath::Min((float)CW / (float)SW, (float)CH / (float)SH);
		const int32 DrawW = FMath::Max(1, FMath::RoundToInt((float)SW * FitScale));
		const int32 DrawH = FMath::Max(1, FMath::RoundToInt((float)SH * FitScale));
		const int32 OX = X0 + (CW - DrawW) / 2;
		const int32 OY = Y0 + (CH - DrawH) / 2;

		// 1:1 快路径：不缩放时按行写入（仍然强制 A=255，防止源数据带 alpha=0 时整块变透明）
		if (DrawW == SW && DrawH == SH && OX >= 0 && OY >= 0)
		{
			for (int32 Row = 0; Row < DrawH; ++Row)
			{
				const int32 PY = OY + Row;
				if (PY >= DH)
				{
					break;
				}

				const FColor* SrcRow = Src.GetData() + (int64)Row * SW;
				FColor* DstRow = Dst.GetData() + (int64)PY * DW + OX;
				for (int32 Col = 0; Col < SW; ++Col)
				{
					const FColor& C = SrcRow[Col];
					DstRow[Col] = FColor(C.R, C.G, C.B, 255);
				}
			}
			return;
		}

		ParallelFor(DrawH, [&Src, &Dst, SW, SH, DW, DH, OX, OY, DrawW, DrawH](int32 Row)
		{
			const float V = ((float)Row + 0.5f) / (float)DrawH;
			const int32 PY = OY + Row;
			if (PY < 0 || PY >= DH)
			{
				return;
			}

			for (int32 Col = 0; Col < DrawW; ++Col)
			{
				const int32 PX = OX + Col;
				if (PX < 0 || PX >= DW)
				{
					continue;
				}

				const float U = ((float)Col + 0.5f) / (float)DrawW;
				Dst[PY * DW + PX] = SampleBilinear(Src, SW, SH, U, V);
			}
		});
	}

	/** 2×2 拼版：左上 原图 / 右上 黑白 / 左下 伪彩 / 右下 直方图 */
	void MakeMergedImage(
		const TArray<FColor>& Viewport, int32 VW, int32 VH,
		const TArray<FColor>& Gray,
		const TArray<FColor>& FalseColor,
		const TArray<FColor>& Histogram, int32 HW, int32 HH,
		int32 CellW, int32 CellH,
		TArray<FColor>& Out, int32& OutW, int32& OutH)
	{
		const int32 Gap = 6;
		const int32 Pad = 6;

		OutW = Pad * 2 + CellW * 2 + Gap;
		OutH = Pad * 2 + CellH * 2 + Gap;
		Out.Init(FColor(18, 18, 22), OutW * OutH);

		const int32 Left = Pad;
		const int32 Right = Pad + CellW + Gap;
		const int32 Top = Pad;
		const int32 Bottom = Pad + CellH + Gap;

		BlitFit(Viewport,   VW, VH, Out, OutW, OutH, Left,  Top,    CellW, CellH);
		BlitFit(Gray,       VW, VH, Out, OutW, OutH, Right, Top,    CellW, CellH);
		BlitFit(FalseColor, VW, VH, Out, OutW, OutH, Left,  Bottom, CellW, CellH);
		BlitFit(Histogram,  HW, HH, Out, OutW, OutH, Right, Bottom, CellW, CellH);
	}
}

bool FHistogramExport::SavePng(const TArray<FColor>& Pixels, int32 Width, int32 Height, const FString& FilePath)
{
	if (Width <= 0 || Height <= 0 || Pixels.Num() != Width * Height)
	{
		return false;
	}

	TArray64<uint8> Compressed;
	FImageUtils::PNGCompressImageArray(Width, Height, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Compressed);
	if (Compressed.Num() == 0)
	{
		return false;
	}

	return FFileHelper::SaveArrayToFile(Compressed, *FilePath);
}

FHistogramExportResult FHistogramExport::ExportMerged(
	const FString& PreferredDirectory,
	const TArray<FColor>& Pixels,
	int32 Width,
	int32 Height,
	const FHistogramData& Data,
	const FFalseColorProfile& Profile)
{
	FHistogramExportResult Result;

	if (Width <= 0 || Height <= 0 || Pixels.Num() != Width * Height)
	{
		Result.Error = TEXT("没有可导出的视口像素");
		return Result;
	}

	const FString Dir = PreferredDirectory.IsEmpty()
		? FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("HistogramAssistant"))
		: PreferredDirectory;
	IFileManager::Get().MakeDirectory(*Dir, true);

	// 1) 黑白（按亮度去饱和，等价「黑白预览」的观感）
	TArray<FColor> Gray;
	Gray.SetNumUninitialized(Pixels.Num());
	ParallelFor(Pixels.Num(), [&Pixels, &Gray](int32 Index)
	{
		const FColor& C = Pixels[Index];
		const uint8 L = (uint8)FMath::Clamp(FMath::RoundToInt(0.2126f * C.R + 0.7152f * C.G + 0.0722f * C.B), 0, 255);
		Gray[Index] = FColor(L, L, L, 255);
	});

	// 2) 伪彩图（与屏幕显示同一份参数）
	TArray<FColor> FalseColor;
	FHistogramFalseColor::Apply(Pixels, Width, Height, Profile, FalseColor);

	// 3) 拼版尺寸：**保持视口原始分辨率**，三张内容图 1:1 不缩放
	//    安全上限：整图不超过约 4000 万像素（4K 视口才会触发，正常 ≤2K 视口不受影响）
	const int32 Gap = 6;
	const int32 Pad = 6;
	int32 CellW = Width;
	int32 CellH = Height;
	{
		const int64 MaxPixels = 40ll * 1000ll * 1000ll;
		const int64 TotalPixels = ((int64)CellW * 2 + Gap + Pad * 2) * ((int64)CellH * 2 + Gap + Pad * 2);
		if (TotalPixels > MaxPixels)
		{
			const double Shrink = FMath::Sqrt((double)MaxPixels / (double)TotalPixels);
			CellW = FMath::Max(64, FMath::RoundToInt((double)CellW * Shrink));
			CellH = FMath::Max(64, FMath::RoundToInt((double)CellH * Shrink));
		}
	}

	// 4) 直方图（按格子尺寸画，正好铺满，不留边）
	TArray<FColor> Histogram;
	MakeHistogramImage(Data, CellW, CellH, Histogram);

	// 5) 四合一
	TArray<FColor> Merged;
	int32 MergedW = 0;
	int32 MergedH = 0;
	MakeMergedImage(Pixels, Width, Height, Gray, FalseColor, Histogram, CellW, CellH, CellW, CellH, Merged, MergedW, MergedH);

	const FString Stamp = FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"));
	const FString FilePath = FPaths::Combine(Dir, FString::Printf(TEXT("%s_combined.png"), *Stamp));

	if (!SavePng(Merged, MergedW, MergedH, FilePath))
	{
		Result.Error = TEXT("写合并图失败");
		return Result;
	}

	Result.bSuccess = true;
	Result.Directory = Dir;
	Result.FilePath = FilePath;
	Result.FileCount = 1;
	return Result;
}
