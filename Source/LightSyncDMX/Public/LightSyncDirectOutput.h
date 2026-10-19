// Copyright UE-Comp. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * LightSyncDirectOutput
 *
 * アプリ終了時のブラックアウトを、UE の DMX 出力ポートを通さずに直接送る。
 * (UE の DMX 出力ポートのスレッドはエンジン終了が要求された時点で送信を止めるため、
 *  終了処理中に Blackout() してもパケットが出ない)
 *
 * 通常送信のたびに「どのポートのどのユニバースのどのチャンネルをこのプラグインが使ったか」と
 * そのブラックアウト値を記録しておき、終了直前 (FCoreDelegates::OnPreExit) に
 * ポートの最新ユニバース (他の送信元のチャンネルも含む) に重ねて 1 ユニバース分を送る。
 *
 * Art-Net: UE の送信ソケットと同じ送信元アドレス (Device Address:6454) から送るので、
 *          ノード側では同じ送信元の更新として扱われる。
 * sACN   : E1.31 パケットを同じ優先度で送る (別 CID なので、受信機のマージ設定によってはソースの
 *          タイムアウト後に反映される)。
 */
namespace LightSyncDirectOutput
{
    /** 送信したチャンネルとそのブラックアウト値を記録 (ゲームスレッド) */
    LIGHTSYNCDMX_API void RecordChannels(const FGuid& PortGuid, int32 LocalUniverse, const TMap<int32, uint8>& BlackoutValues);

    /** 記録したチャンネルをブラックアウトにしたユニバースを、全出力ポートへ直接送る。送ったパケット数を返す */
    LIGHTSYNCDMX_API int32 SendBlackoutNow();

    /** 記録を消す */
    LIGHTSYNCDMX_API void Reset();

    /** Art-Net ArtDmx パケット (テスト用に公開) */
    LIGHTSYNCDMX_API TArray<uint8> MakeArtNetPacket(int32 PortAddress, const uint8 (&Data)[512]);

    /** sACN (E1.31) データパケット (テスト用に公開) */
    LIGHTSYNCDMX_API TArray<uint8> MakeSACNPacket(int32 Universe, uint8 Priority, uint8 Sequence, const FGuid& CID, const uint8 (&Data)[512]);
}
