// HistogramAssistant - 伪彩色曝光（False Color Exposure）
//
// 纯 CPU 实现：把「显示域」像素（ReadPixels 得到的 8bit sRGB 值）按 IRE 档位上色。
// 不参与渲染、不生成后期材质，因此：
//   1) 不存在 shader 编译失败的风险；
//   2) 不会被视口 ReadPixels 抓进去，不会污染直方图统计。

#pragma once

#include "CoreMinimal.h"

/** 一个档位：IRE 上界（不含）→ 显示颜色 */
struct FFalseColorBand
{
	/** 档位上界，单位 IRE%（0~100.1） */
	float MaxIRE = 100.1f;

	/** 该档显示的 8bit 显示域颜色（写进 sRGB 纹理，所见即所得） */
	FColor Color = FColor::White;

	/** true = 该档不做伪彩，保留原图（或以灰度显示） */
	bool bKeepOriginal = false;

	/** 图例文字 */
	FString Meaning;
};

/** 伪彩显示模式 */
enum class EFalseColorMode : uint8
{
	/** 档位（曝光）：只给关键档上色，其余显示灰度 —— 真机监视器语义，用来判曝光对不对 */
	ExposureBands,

	/** 连续色图：整幅画面按亮度着彩虹色（蓝→青→绿→黄→红）—— 用来看结构、层次和明暗分布 */
	ContinuousRamp
};

struct FFalseColorProfile
{
	TArray<FFalseColorBand> Bands;

	/** 显示模式（默认连续色图，与用户预期的"相机伪彩"一致） */
	EFalseColorMode Mode = EFalseColorMode::ContinuousRamp;

	/** 连续色图模式用的 256 项查找表（CLUT），由 MakeDefault 生成 */
	TArray<FColor> Ramp;

	/** bKeepOriginal 的档位是否改为显示灰度（RED 的"其余值显示去饱和图"风格） */
	bool bDesaturateOthers = false;

	/**
	 * 分档前先按 N×N 取「块平均 IRE」。
	 * 1 = 逐像素精确分档（真机语义，但带高频纹理的表面会出现彩色噪点）；
	 * >1 = 抹掉逐像素跳档造成的噪点，结果是大块干净的色域。
	 */
	int32 SampleBlockSize = 1;

	static FFalseColorProfile MakeDefault();
};

class FHistogramFalseColor
{
public:
	/** 上色。只读 SrcPixels（原始像素保持不动），OutPixels 为显示副本。Width/Height 用于可选的块平均。 */
	static void Apply(const TArray<FColor>& SrcPixels, int32 Width, int32 Height, const FFalseColorProfile& Profile, TArray<FColor>& OutPixels);

	/** IRE% → 档位下标（返回最后一档兜底） */
	static int32 BandIndexForIRE(float IRE, const FFalseColorProfile& Profile);

	/** FColor(显示域 sRGB 8bit) → IRE%（与面板「平均亮度×100」同一口径） */
	static float IREOf(const FColor& C);
};
