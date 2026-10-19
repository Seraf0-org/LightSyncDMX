// Copyright UE-Comp. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * LightSyncColor
 *
 * 色計算コア（純C++。UObject に依存しない）。
 *
 *   - 作業色空間 RGB ↔ CIE XYZ ↔ xyY
 *   - CCT + Duv（Ohno 2013）と、その逆変換（CCT + Duv → xy）
 *   - 灯体ソルバ: カメラ空間で M_fix · d = 目標 を非負最小二乗で解く
 *     （白エミッタ優先の正則化、色相を保つ色域マッピング、等倍クリップ）
 *   - 調光カーブ（リニア / ガンマ / 区分線形 LUT）の正逆変換
 *   - DMX 8/16bit 量子化
 *   - カメラ伝達関数（Log など）の線形化
 *   - キャリブレーション用のフィッティング（3x3 行列、ガンマ）
 *
 * 行列とベクトルの約束:
 *   FMatrix3x3 は列ベクトル規約（y = M · x）。M.M[行][列]。
 *   UE の FMatrix44d（行ベクトル規約 y = x · M）とは逆なので注意。
 */
namespace LightSyncColor
{
    // =========================================================================
    // 3x3 行列（列ベクトル規約）
    // =========================================================================

    struct LIGHTSYNCDMX_API FMatrix3x3
    {
        /** M[行][列]。y = M · x */
        double M[3][3];

        /** 単位行列で初期化する */
        FMatrix3x3();

        static FMatrix3x3 Identity() { return FMatrix3x3(); }

        /** 3 本の列ベクトルから作る（列 = 各原色のカメラ応答、など） */
        static FMatrix3x3 FromColumns(const FVector3d& C0, const FVector3d& C1, const FVector3d& C2);

        FVector3d GetColumn(int32 Index) const;

        /** y = M · x */
        FVector3d Transform(const FVector3d& V) const;

        FMatrix3x3 operator*(const FMatrix3x3& Other) const;

        FMatrix3x3 GetTransposed() const;

        double Determinant() const;

        /** 逆行列。特異（|det| が極小）なら false を返し、OutInverse は単位行列になる */
        bool Inverse(FMatrix3x3& OutInverse) const;
    };

    // =========================================================================
    // 1. 色空間
    // =========================================================================

    /** 作業色空間のリニア RGB → CIE XYZ（UE::Color::FColorSpace::GetWorking() を使用） */
    LIGHTSYNCDMX_API FVector3d WorkingRGBToXYZ(const FVector3d& RGB);

    /** CIE XYZ → 作業色空間のリニア RGB */
    LIGHTSYNCDMX_API FVector3d XYZToWorkingRGB(const FVector3d& XYZ);

    /** XYZ → xyY（X+Y+Z が 0 のときは作業色空間の白色点の xy、Y=0 を返す） */
    LIGHTSYNCDMX_API FVector3d XYZToxyY(const FVector3d& XYZ);

    /** xyY → XYZ（y が 0 のときは黒） */
    LIGHTSYNCDMX_API FVector3d xyYToXYZ(const FVector3d& xyY);

    /** XYZ → CIE 1960 UCS (u, v) */
    LIGHTSYNCDMX_API FVector2d XYZToUV1960(const FVector3d& XYZ);

    /** CIE 1960 UCS (u, v) → CIE 1931 xy */
    LIGHTSYNCDMX_API FVector2d UV1960Toxy(const FVector2d& UV);

    /** CIE 1931 xy → CIE 1960 UCS (u, v) */
    LIGHTSYNCDMX_API FVector2d xyToUV1960(const FVector2d& xy);

    // =========================================================================
    // 2. 色温度（CCT）と Duv
    // =========================================================================

    /** CCT 計算の対象レンジ [K] */
    constexpr double MinCCT = 1000.0;
    constexpr double MaxCCT = 25000.0;

    /**
     * 黒体放射（Planck 軌跡）の CIE 1960 (u, v)。
     * CIE 1931 2° 等色関数（CIE 公表の 5nm 表、360–830nm）と Planck の法則
     * （c2 = 1.438776877e-2 m·K）を数値積分して求める厳密計算。近似式は使わない。
     * 1nm 表との差は 1000–10000K で CCT 換算 0.2K 未満。
     */
    LIGHTSYNCDMX_API FVector2d PlanckianUV(double CCT);

    /**
     * XYZ → CCT [K] と Duv（Ohno 2013, "Practical Use and Calculation of CCT and Duv",
     * LEUKOS 10(1)）。Planck 表を 1% 刻みで引いてから、最小距離の前後区間を
     * 細かく張り直すカスケード展開を行い、最後に三角法（|Duv| < 0.002）または
     * 放物線法で内挿する。カスケードで区間を十分に細かくしているので、
     * 論文にある放物線法の補正係数（0.99991）は掛けない。
     * Duv は Planck 軌跡より上（緑側）が正。
     * CCT は [MinCCT, MaxCCT] にクランプされる。XYZ が黒なら false。
     */
    LIGHTSYNCDMX_API bool XYZToCCTDuv(const FVector3d& XYZ, double& OutCCT, double& OutDuv);

    /** CCT [K] + Duv → CIE 1931 xy（Ohno 2013 の逆変換。法線方向に Duv だけずらす） */
    LIGHTSYNCDMX_API FVector2d CCTDuvToxy(double CCT, double Duv);

    // =========================================================================
    // 3. 灯体ソルバ
    // =========================================================================

    /** 灯体のエミッタ集合（カメラ空間での応答） */
    struct LIGHTSYNCDMX_API FEmitterSet
    {
        /** エミッタごとの 100% 点灯時のカメラ RGB 応答（M_fix の列）。最大 6 本を想定 */
        TArray<FVector3d> Responses;

        /** 白エミッタのインデックス（RGBW の W など）。なければ INDEX_NONE */
        int32 PreferredWhiteIndex = INDEX_NONE;

        /**
         * 白エミッタ優先の強さ [0, 1]。
         * 0 = 全エミッタの二乗和が最小になる解、1 = 白エミッタを最大限使う解。
         * 色の一致（厳密解）が優先され、解が複数あるときの選び方だけを変える。
         */
        double WhitePreference = 0.0;

        /** 色域マッピングで「等輝度」とみなすための輝度の重み（カメラ RGB に対して）。既定は Rec.709 */
        FVector3d LuminanceWeights = FVector3d(0.2126, 0.7152, 0.0722);

        /** 色域マッピングの白（PreferredWhiteIndex の応答、なければ全エミッタの和） */
        FVector3d GetWhiteResponse() const;
    };

    struct LIGHTSYNCDMX_API FSolveResult
    {
        /** エミッタごとのレベル [0, 1]（Responses と同じ並び） */
        TArray<double> Levels;

        /** 目標が非負のレベルで再現できず、色域マッピング（白方向への彩度低下）を行った */
        bool bOutOfGamut = false;

        /** レベルが 1 を超えたため全体を等倍で縮小した */
        bool bClipped = false;

        /** 最終レベルで得られるカメラ RGB（= M_fix · Levels） */
        FVector3d Achieved = FVector3d::ZeroVector;

        /** |Achieved − Target|（ユークリッド距離。色域マッピングとクリップの影響を含む） */
        double Residual = 0.0;
    };

    /**
     * カメラ空間の目標 RGB に対してエミッタのレベルを解く。
     *
     *  1. 非負最小二乗（Lawson–Hanson NNLS）で厳密に再現できるか調べる
     *  2. 再現できる場合: 色を厳密に保ったまま、白エミッタ優先の Tikhonov 項
     *     Σ c_i d_i²（c_white = 1 − WhitePreference）が最小になる解を選ぶ
     *  3. 再現できない場合: 目標と同じ輝度に揃えた白へ向かう直線上で、
     *     再現できる最も彩度の高い点を二分探索する（色度図上で白色点まわりの色相が保たれる）
     *  4. 最大レベルが 1 を超えたら全体を等倍で縮小する
     */
    LIGHTSYNCDMX_API FSolveResult SolveEmitterLevels(const FEmitterSet& Emitters, const FVector3d& TargetCameraRGB);

    /**
     * 非負最小二乗 min |A·x − b|², x ≥ 0（Lawson–Hanson）。
     * A は行優先（Rows × Cols）。Cols ≤ 16、Rows ≤ 32 を想定した小規模実装。
     * 戻り値は残差ノルム |A·x − b|。
     */
    LIGHTSYNCDMX_API double SolveNNLS(const TArray<double>& A, int32 Rows, int32 Cols, const TArray<double>& B, TArray<double>& OutX);

    // =========================================================================
    // 4. 調光カーブ
    // =========================================================================

    /**
     * DMX レベル（0..1）→ 光出力の割合（0..1）。
     * 灯体は「DMX レベル L で出力 Apply(L)」を出す、というモデル。
     * ソルバが求めるのは光出力なので、送信前に Invert() で DMX レベルへ戻す。
     */
    struct LIGHTSYNCDMX_API FDimmingCurve
    {
        /** 列挙子名がメンバ Gamma と衝突しないよう enum class にしている（FDimmingCurve::EType::Gamma） */
        enum class EType : uint8
        {
            Linear,
            Gamma,
            LUT,
        };

        EType Type = EType::Linear;

        /** Type == Gamma のとき: 出力 = レベル ^ Gamma */
        double Gamma = 2.2;

        /**
         * Type == LUT のとき: (X = DMX レベル, Y = 光出力) の点列。
         * X で並べ替え、Y は累積最大で単調非減少として扱う（区分線形）。
         * レンジ外は端の値にクランプ。点が 2 つ未満ならリニアとして扱う。
         */
        TArray<FVector2d> Points;

        /** DMX レベル → 光出力 */
        double Apply(double Level) const;

        /** 光出力 → DMX レベル（Apply の逆。平坦な区間では最小のレベルを返す） */
        double Invert(double OutputFraction) const;
    };

    // =========================================================================
    // 5. DMX 量子化
    // =========================================================================

    /**
     * レベル [0, 1] を DMX 値へ量子化する（最近接の符号に丸める）。
     *   16bit: V = round(Level · 65535), Coarse = V >> 8, Fine = V & 0xFF
     *    8bit: Coarse = round(Level · 255), Fine = 0
     */
    LIGHTSYNCDMX_API void QuantizeToDMX(double Level, bool b16Bit, uint8& Coarse, uint8& Fine);

    /** QuantizeToDMX の逆（DMX 値 → レベル [0, 1]） */
    LIGHTSYNCDMX_API double DequantizeDMX(uint8 Coarse, uint8 Fine, bool b16Bit);

    // =========================================================================
    // 6. カメラ伝達関数の線形化
    // =========================================================================

    enum class ECameraTransfer : uint8
    {
        /** 既にリニア */
        Linear,
        /** IEC 61966-2-1 sRGB */
        sRGB,
        /** ITU-R BT.709 OETF */
        Rec709,
        /** ARRI ALEXA Log C (v3)、EI 800 */
        ArriLogC3_EI800,
        /** ARRI LogC4 */
        ArriLogC4,
        /** Sony S-Log3 */
        SonySLog3,
    };

    /**
     * カメラのコード値 → シーンリニア値（18% グレー → 0.18）。
     * CodeValue01 はフルレンジ正規化値（10bit なら コード値 / 1023）。
     */
    LIGHTSYNCDMX_API double LinearizeCameraValue(ECameraTransfer Transfer, double CodeValue01);

    /** 3 チャンネルまとめて線形化する */
    LIGHTSYNCDMX_API FVector3d LinearizeCameraRGB(ECameraTransfer Transfer, const FVector3d& CodeValues01);

    // =========================================================================
    // 7. キャリブレーション用フィッティング
    // =========================================================================

    /**
     * Measured_i ≈ M · Reference_i となる 3x3 行列 M を最小二乗で求める。
     * 例: Reference = 壁パッチの作業色空間 RGB、Measured = カメラで読んだリニア RGB → M_ref。
     * パッチが 3 未満、または参照が同一平面上（行列が特異）なら false。
     * OutRmsError が指定されていれば、当てはめ後の RMS 誤差（ベクトル長）を返す。
     */
    LIGHTSYNCDMX_API bool Fit3x3FromPatches(const TArray<FVector3d>& ReferenceRGB, const TArray<FVector3d>& MeasuredRGB,
                                            FMatrix3x3& OutMatrix, double* OutRmsError = nullptr);

    /**
     * 調光ステップの計測値から ガンマ を求める（Measured ≈ k · Level ^ Gamma を対数空間で回帰）。
     * Level ≤ 0 や Measured ≤ 0 の点は無視する。k は自由（100% の計測値で正規化しなくてよい）。
     * 有効な点が 2 つ未満なら 0 を返す。OutScale が指定されていれば k を返す。
     */
    LIGHTSYNCDMX_API double FitGammaFromSteps(const TArray<double>& Levels, const TArray<double>& Measured, double* OutScale = nullptr);
}
