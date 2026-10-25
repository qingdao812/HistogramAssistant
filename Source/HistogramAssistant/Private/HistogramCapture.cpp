#include "HistogramCapture.h"

#include "Editor.h"
#include "UnrealClient.h"
#include "ViewportClient.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/World.h"
#include "Engine/BlendableInterface.h"
#include "Materials/Material.h"
#include "RenderingThread.h"

static TWeakObjectPtr<APostProcessVolume> GGrayscalePreviewVolume;

static float LumaOf(const FColor& C)
{
	return 0.2126f * C.R + 0.7152f * C.G + 0.0722f * C.B;
}

bool FHistogramCapture::CaptureActiveViewport(FHistogramData& OutData, TArray<FColor>* OutPixels)
{
	if (!GEditor)
	{
		return false;
	}

	FViewport* Viewport = GEditor->GetActiveViewport();
	if (!Viewport)
	{
		return false;
	}

	const FIntPoint Size = Viewport->GetSizeXY();
	if (Size.X <= 0 || Size.Y <= 0)
	{
		return false;
	}

	TArray<FColor> Bitmap;
	Bitmap.Reserve(Size.X * Size.Y);

	// 抓图前暂时禁用黑白预览滤镜，保证直方图永远基于彩色原始画面。
	// 否则 ReadPixels 会把黑白后的画面也统计进去，直方图被自己的滤镜污染。
	const bool bGrayscaleOn = GGrayscalePreviewVolume.IsValid();
	if (bGrayscaleOn && GGrayscalePreviewVolume.IsValid())
	{
		GGrayscalePreviewVolume->bEnabled = false;
		GEditor->RedrawAllViewports(false);
		FlushRenderingCommands(); // 等禁用生效，确保 ReadPixels 拿的是无滤镜画面
	}

	const bool bReadOK = Viewport->ReadPixels(Bitmap) && Bitmap.Num() == Size.X * Size.Y;

	// ★ ReadPixels 回来的 alpha 是 0（编辑器视口的 alpha 没有意义）。
	//   统一在源头刷成不透明，避免下游反复踩"整块全透明"的坑：
	//   - 伪彩上色：曾导致非关键档变成"洞"、看着像没覆盖模型
	//   - 导出拼图：曾导致"原始视口截图"那格在 PNG 里全透明、看图时一片白
	if (bReadOK)
	{
		for (FColor& C : Bitmap)
		{
			C.A = 255;
		}
	}

	// 恢复黑白预览滤镜
	if (bGrayscaleOn && GGrayscalePreviewVolume.IsValid())
	{
		GGrayscalePreviewVolume->bEnabled = true;
		GEditor->RedrawAllViewports(false);
	}

	if (!bReadOK)
	{
		return false;
	}

	OutData = FHistogramData();
	OutData.Width = Size.X;
	OutData.Height = Size.Y;
	OutData.LumaHist.SetNumZeroed(256);
	OutData.RHist.SetNumZeroed(256);
	OutData.GHist.SetNumZeroed(256);
	OutData.BHist.SetNumZeroed(256);

	double SumR = 0.0, SumG = 0.0, SumB = 0.0;
	double SumLuma = 0.0;
	float MinLuma = FLT_MAX;
	float MaxLuma = 0.f;
	int64 ShadowCount = 0;
	int64 MidtoneCount = 0;
	int64 HighlightCount = 0;
	const int64 Total = (int64)Size.X * Size.Y;

	for (const FColor& C : Bitmap)
	{
		const float L = LumaOf(C);
		const int32 Li = FMath::Clamp(FMath::RoundToInt(L), 0, 255);

		OutData.LumaHist[Li]++;
		OutData.RHist[C.R]++;
		OutData.GHist[C.G]++;
		OutData.BHist[C.B]++;

		SumR += C.R;
		SumG += C.G;
		SumB += C.B;
		SumLuma += L;
		MinLuma = FMath::Min(MinLuma, L);
		MaxLuma = FMath::Max(MaxLuma, L);

		if (L < 85.f)
		{
			++ShadowCount;
		}
		else if (L > 170.f)
		{
			++HighlightCount;
		}
		else
		{
			++MidtoneCount;
		}
	}

	const float Inv = 1.0f / (float)Total;
	OutData.AvgR = (float)(SumR * Inv);
	OutData.AvgG = (float)(SumG * Inv);
	OutData.AvgB = (float)(SumB * Inv);
	OutData.AvgLuma = (float)(SumLuma * Inv);
	OutData.MinLuma = MinLuma;
	OutData.MaxLuma = MaxLuma;
	OutData.ShadowRatio = (float)ShadowCount * Inv;
	OutData.MidtoneRatio = (float)MidtoneCount * Inv;
	OutData.HighlightRatio = (float)HighlightCount * Inv;

	OutData.MaxHistCount = 1;
	for (int32 i = 0; i < 256; ++i)
	{
		OutData.MaxHistCount = FMath::Max(OutData.MaxHistCount, OutData.LumaHist[i]);
		OutData.MaxHistCount = FMath::Max(OutData.MaxHistCount, OutData.RHist[i]);
		OutData.MaxHistCount = FMath::Max(OutData.MaxHistCount, OutData.GHist[i]);
		OutData.MaxHistCount = FMath::Max(OutData.MaxHistCount, OutData.BHist[i]);
	}

	// 统计已用完这份像素，需要就交给调用方（伪彩色用），避免多余拷贝
	if (OutPixels)
	{
		*OutPixels = MoveTemp(Bitmap);
	}

	return true;
}

bool FHistogramCapture::SetGrayscalePreview(bool bEnabled)
{
	if (!GEditor)
	{
		return false;
	}

	// 取活动视口对应的世界（编辑视口或 PIE 视口）
	UWorld* World = nullptr;
	FViewport* Viewport = GEditor->GetActiveViewport();
	if (Viewport && Viewport->GetClient())
	{
		World = Viewport->GetClient()->GetWorld();
	}
	if (!World)
	{
		return false;
	}

	if (bEnabled)
	{
		APostProcessVolume* Volume = GGrayscalePreviewVolume.Get();
		if (!Volume)
		{
			FActorSpawnParameters SpawnParams;
			SpawnParams.ObjectFlags |= RF_Transient; // 不保存进关卡
			Volume = World->SpawnActor<APostProcessVolume>(FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
			GGrayscalePreviewVolume = Volume;
		}

		if (!Volume)
		{
			return false;
		}

		Volume->SetActorLabel(TEXT("HistogramAssistant_GrayscalePreview"));
		Volume->bUnbound = true;      // 覆盖整个场景
		Volume->BlendRadius = 0.f;
		Volume->BlendWeight = 1.f;
		Volume->Priority = 100.f;
		Volume->bEnabled = true;
		Volume->Settings.bOverride_ColorSaturation = true;
		Volume->Settings.ColorSaturation = FVector4(0.f, 0.f, 0.f, 1.f); // RGB 饱和度=0 => 黑白
	}
	else
	{
		if (APostProcessVolume* Volume = GGrayscalePreviewVolume.Get())
		{
			Volume->Destroy();
		}
		GGrayscalePreviewVolume = nullptr;
	}

	GEditor->RedrawAllViewports(false);
	return true;
}

bool FHistogramCapture::IsGrayscalePreviewEnabled()
{
	return GGrayscalePreviewVolume.IsValid();
}
