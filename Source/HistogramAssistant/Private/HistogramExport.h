// HistogramAssistant - 一键导出（四合一合并图）
//
// 点一次「导出」→ 选目录 → 输出 **一张 PNG**，2×2 拼版：
//   ┌──────────────┬──────────────┐
//   │ 原始视口画面   │ 黑白（去饱和）│
//   ├──────────────┼──────────────┤
//   │ 伪彩色曝光图   │ 直方图曲线    │
//   └──────────────┴──────────────┘
//
// 只输出这一张图，不写任何文字说明文件。
// 全部用 CPU 合成 + PNG 编码（FImageUtils::PNGCompressImageArray），不碰 RHI。

#pragma once

#include "CoreMinimal.h"
#include "HistogramCapture.h"
#include "HistogramFalseColor.h"

struct FHistogramExportResult
{
	bool bSuccess = false;
	FString Directory;
	FString FilePath;
	FString Error;
	int32 FileCount = 0;
};

class FHistogramExport
{
public:
	/** 导出四合一合并图。PreferredDirectory 为空时用默认的 Saved/HistogramAssistant。
	 *  Profile 请传"与屏幕显示一致"的那份（含 SampleBlockSize）。 */
	static FHistogramExportResult ExportMerged(
		const FString& PreferredDirectory,
		const TArray<FColor>& Pixels,
		int32 Width,
		int32 Height,
		const FHistogramData& Data,
		const FFalseColorProfile& Profile);

	/** 单张 PNG 落盘 */
	static bool SavePng(const TArray<FColor>& Pixels, int32 Width, int32 Height, const FString& FilePath);
};
