# slcan-utils

Windows 用の SLCAN (Lawicel/CANable プロトコル) ユーティリティです。物理シリアル(COM)ポート経由で接続された SLCAN 対応 CAN/CAN FD アダプタを、複数のプロセスから同時に読み書きできるように名前付きパイプでブリッジします。

## 構成

| 実行ファイル | 役割 |
| --- | --- |
| `slcd.exe` (`serial_daemon.c`) | COM ポートと SLCAN アダプタを開き、名前付きパイプ (`\\.\pipe\serial_tx\<channel>` / `\\.\pipe\serial_rx\<channel>`) を公開する常駐デーモン。複数の reader/writer から同時接続可能。 |
| `slcr.exe` (`serial_reader.c`) | デーモンの RX パイプに接続し、受信した CAN/CAN FD フレームを candump ライクなテキスト形式で標準出力に表示する CLI。 |
| `slcw.exe` (`serial_writer.c`) | 標準入力からフレームのテキスト行を読み取り、デーモンの TX パイプ経由で送信する CLI。 |
| `slcan` (`slcan.c` / `slcan.h`) | 上記3つが共有する SLCAN プロトコルのエンコード/デコード処理をまとめた静的ライブラリ。 |

`channel` は複数の COM ポート/デーモンを同時に扱うための名前空間で、reader/writer 側は接続したいデーモンと同じ `channel` を指定する必要があります。

## ビルド

Windows + CMake (Visual Studio または MinGW) でのビルドを想定しています。

```
cmake -S . -B build
cmake --build build --config Release
```

Visual Studio のプロジェクトファイルを生成した場合、`CMAKE_BUILD_TYPE` はビルド時の `--config` 指定 (または IDE のソリューション構成) で切り替わります。単一コンフィグ生成 (Ninja/Makefiles) の場合は `CMakeLists.txt` 側で `CMAKE_BUILD_TYPE` が未指定なら自動的に `Release` になります。

ビルドされた実行ファイルは `build/` 配下に出力されます。

## 使い方

### デーモンの起動

```
slcd.exe [COM] [channel] [arb_code] [data_code]
slcd.exe -h | --help
```

```
slcd.exe COM1 can0 6 5     CAN FD (調停 500kbps / データ 1Mbps)
slcd.exe COM1 can0 6       Classic CAN 500kbps
slcd.exe                   すべて既定値 (COM1, can0, 500kbps, Classic CAN)
```

- `arb_code` (調停レート): `0`=10k `1`=20k `2`=50k `3`=100k `4`=125k `5`=250k `6`=500k `7`=800k `8`=1M
- `data_code` (CAN FD データレート、CANable 2.0 系): `1`=1M `2`=2M `4`=4M `5`=5M。省略時は Classic CAN。

### フレームの受信 (reader)

```
slcr.exe [channel]
slcr.exe -h | --help
```

指定した `channel` のデーモンの RX パイプに接続し、フレームを受信するたびに次の形式で1行ずつ標準出力に表示します。

```
(1735689600.123456) can0 123#DEADBEEF
```

### フレームの送信 (writer)

```
slcw.exe [channel]
slcw.exe -h | --help
```

標準入力から1行1フレームの形式で読み込み、デーモンの TX パイプへ送信します。

```
123#DEADBEEF          Classic CAN Standard
00000123#DEADBEEF     Classic CAN Extended
123#R                 RTR
123##DEADBEEF...      CAN FD Standard
123##*DEADBEEF...     CAN FD Standard + BRS
```

`#` または `;` で始まる行はコメントとして無視されます。

## 動作要件

Windows 専用です (名前付きパイプ / Win32 API に依存しているため)。SLCAN プロトコルに対応した COM ポート接続の CAN/CAN FD アダプタ (Lawicel CANUSB, CANable など) が別途必要です。
