# slcan-utils

Windows 用の SLCAN (Lawicel/CANable プロトコル) ユーティリティです。物理シリアル(COM)ポート経由で接続された SLCAN 対応 CAN/CAN FD アダプタを、複数のプロセスから同時に読み書きできるように名前付きパイプでブリッジします。

## 構成

| 実行ファイル | 役割 |
| --- | --- |
| `slcd.exe` (`serial_daemon.c`) | COM ポートと SLCAN アダプタを開き、名前付きパイプ (`\\.\pipe\serial_tx\<channel>` / `\\.\pipe\serial_rx\<channel>`) を公開する常駐デーモン。複数の reader/writer から同時接続可能。`--vcan` 指定時は物理アダプタなしで動作する仮想 CAN チャネルになる。 |
| `slcr.exe` (`serial_reader.c`) | デーモンの RX パイプに接続し、受信した CAN/CAN FD フレームを candump ライクなテキスト形式で標準出力に表示する CLI。 |
| `slcw.exe` (`serial_writer.c`) | 標準入力からフレームのテキスト行を読み取り、デーモンの TX パイプ経由で送信する CLI。 |
| `slcplay.exe` (`slcan_player.c`) | `slcr.exe` の出力形式のログファイルを、元のフレーム間隔を再現しながらデーモンの TX パイプへ再生する CLI。 |
| `slcgw.exe` (`slcan_gateway.c`) | チャネルごとに割り当てた Lua スクリプトでゲートウェイルール (フィルタ/編集/転送先) を記述できる CAN ゲートウェイ。 |
| `slcgen.exe` (`slcan_generator.c`) | 指定した周期で CAN/CAN FD フレームを生成し、デーモンの TX パイプへ送り続ける CLI。動作確認用のトラフィック生成に使う。 |
| `slcan` (`slcan.c` / `slcan.h`) | 上記の各 CLI が共有する SLCAN プロトコルのエンコード/デコード処理をまとめた静的ライブラリ。 |

`channel` は複数の COM ポート/デーモンを同時に扱うための名前空間で、reader/writer 側は接続したいデーモンと同じ `channel` を指定する必要があります。

## ビルド

Windows + CMake (Visual Studio または MinGW) でのビルドを想定しています。

```
cmake -S . -B build
cmake --build build --config Release
```

Visual Studio のプロジェクトファイルを生成した場合、`CMAKE_BUILD_TYPE` はビルド時の `--config` 指定 (または IDE のソリューション構成) で切り替わります。単一コンフィグ生成 (Ninja/Makefiles) の場合は `CMakeLists.txt` 側で `CMAKE_BUILD_TYPE` が未指定なら自動的に `Release` になります。

ビルドされた実行ファイルは `build/` 配下に出力されます。

`slcgw.exe` は Lua (5.5.1) を埋め込んでおり、`cmake -S . -B build` の実行時に CMake の `FetchContent` が `https://www.lua.org/ftp/lua-5.5.1.tar.gz` を自動的にダウンロードしてビルドします。追加のインストール作業は不要ですが、初回の `cmake` 実行時にこの URL へのネットワークアクセスが必要です。

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

### 仮想 CAN チャネル (vcan)

物理の USB-CAN アダプタを持っていなくても、`--vcan` を指定すると仮想チャネルとして `slcd.exe` を起動できます。

```
slcd.exe --vcan [channel]
```

```
slcd.exe --vcan can0      仮想チャネル can0 (アダプタ不要)
slcd.exe --vcan           既定チャネル (can0) の仮想チャネル
```

COM ポートを一切開かず、`slcw.exe` (または `slcplay.exe`/`slcgw.exe` からの転送) でそのチャネルの TX パイプに書き込まれたフレームを、SLCAN のエンコード/デコードを介さずそのまま RX パイプへ流します。つまり、あるチャネルに書き込んだフレームが、同じチャネルを見ている reader (`slcr.exe`、`slcgw.exe` など) にそのまま届く、単純なループバックです。`arb_code`/`data_code` は仮想チャネルには意味を持たないため指定できません (指定するとエラーになります)。

`slcr.exe`/`slcw.exe`/`slcplay.exe`/`slcgw.exe` 側は通常の (物理アダプタの) チャネルと全く同じように扱えるので、実機なしでのゲートウェイスクリプトの動作確認や、reader/writer/player の疎通確認などに使えます。

物理モードの `slcd.exe` と同じチャネル名で同時に `--vcan` を起動すると、名前付きパイプが競合し、reader/writer がどちらのデーモンに繋がるか不定になります。併用する場合はチャネル名を分けてください。

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

### ログの再生 (player)

```
slcplay.exe -I <infile> [options] [channel ...]
slcplay.exe -h | --help
```

`slcr.exe` の出力 (例: `slcr.exe can0 > log.txt`) をそのまま再生できます。ログの各行のタイムスタンプ間隔をもとに、元のフレーム間隔を再現しながらデーモンの TX パイプへ送信します。

```
(1735689600.123456) can0 123#DEADBEEF
```

ログに複数の `channel` が混在していても、登場したチャネルごとに TX パイプ (`\\.\pipe\serial_tx\<channel>`) を自動的に開いて振り分けるため、1つの `slcplay.exe` プロセスで複数チャネル分をまとめて再生できます。対応するデーモンが起動していないチャネルは警告を出してそのフレームだけスキップし、他のチャネルの再生は継続します。

`slcplay.exe` はログ行の `channel` 名を書き換えたりせず、そのまま接続先の TX パイプ名 (`\\.\pipe\serial_tx\<channel>`) として使います。そのため、例えば `can0` で録ったログを `can1` のデーモンへ再生したい場合など、記録時と異なるチャネルへ再生したいときは、事前にログファイル側の `channel` 列を目的のチャネル名に書き換えておく必要があります。

オプション:

- `-I <infile>`: 再生するログファイル (必須)
- `-l <num>`: ログファイルを再生する回数 (既定値: `1`)。`i` を指定すると無限ループ。
- `-g <ms>`: 送信タイミングの再チェック間隔 (既定値: `1`ミリ秒)。フレーム間の間隔そのものではなく、スケジューラがどのくらいの頻度で「送信すべきフレームがないか」を確認するかを指定するものです。

位置引数で `channel` を1つ以上指定すると、そのチャネルのフレームのみ再生します (省略時はログ内の全チャネルを再生)。

### フレームの生成 (generator)

```
slcgen.exe [channel] [-g <ms>] [-f] [-e] [-I <id>] [-L <len>] [-D <data>]
slcgen.exe -h | --help
```

指定した周期 (`-g`、ミリ秒) で CAN/CAN FD フレームを生成し、デーモンの TX パイプへ送り続けます。Ctrl+C で停止するまで終了しません。`-I`/`-L`/`-D` で指定したフィールドは毎回そのまま使われ、指定しなかったフィールドは送信のたびに毎回ランダムな値で作り直されます。RTR フレーム、BRS、ESI はこのツールでは生成しません。

```
slcgen.exe can0                              Classic CAN、既定周期200ms、ID/長さ/データを毎回ランダム生成
slcgen.exe can0 -g 100 -I 123                100ms周期、ID=0x123 固定、長さ/データは毎回ランダム
slcgen.exe can0 -g 100 -f -I 123 -D DEADBEEF CAN FD、ID=0x123・データ=DEADBEEF固定 (長さはDから4に確定)
slcgen.exe can0 -e                           拡張(29bit)IDを毎回ランダム生成
```

オプション:

- `-g <ms>`: 送信周期 (既定値: `200`)
- `-f`: CAN FD モード (省略時は Classic CAN)。
- `-e`: 拡張(29bit)ID を強制。`-I` 省略時はランダムIDの生成範囲が拡張IDになります。`-I` 指定時は、桁数による標準/拡張の自動判定(下記)を拡張側に上書きします(標準への上書きはできません)。
- `-I <id>`: CAN ID を16進数で指定 (例: `123`、`1A2B3C4D`)。桁数が8桁ちょうどなら拡張(29bit)、それ以外(1〜7桁)は標準(11bit)として解釈されます (candump形式の `ID#DATA` をパースする `slcan_parse_frame()` と同じ規約、`-e` 指定時は常に拡張)。省略時はIDを毎回ランダム生成 (`-e` 未指定なら標準11bit、指定時は拡張29bit)。
- `-L <len>`: データ長をバイト数で指定 (Classic: 0-8、`-f`: 0-64)。`-D` も指定している場合はバイト数が一致している必要があります。`-D` のみ指定した場合は `-D` のバイト数がそのまま長さになります。両方省略時は毎回ランダムな長さ。
- `-D <data>`: データを16進数で指定 (例: `DEADBEEF`、偶数桁)。省略時は毎回ランダムなデータ。

### ゲートウェイ (gateway)

```
slcgw.exe <channel>=<script.lua> [<channel>=<script.lua> ...]
slcgw.exe -h | --help
```

`channel=script.lua` の形式で、受信するチャネルとそれを処理する Lua スクリプトの組を1つ以上指定します。指定したチャネルごとに独立した Lua VM ・ワーカースレッドが立ち上がり、そのチャネルの RX パイプ (`\\.\pipe\serial_rx\<channel>`) から受信したフレームごとに、スクリプトの `gateway(src_channel, frame)` 関数が呼ばれます。

```
slcgw.exe can0=examples/gateway_example.lua can1=other.lua
```

スクリプト側が定義できる関数は次の3つです。

- `initialize()`: チャネル開始時に一度だけ呼ばれます。`0` 以外を返す、またはエラーを起こすと、そのチャネルだけが無効化されます (他のチャネルには影響しません)。
- `gateway(src_channel, frame)`: フレーム受信のたびに呼ばれます。`frame` は `id`/`ext`/`rtr`/`fd`/`brs`/`esi`/`dlc`/`len`/`data` (1始まりの配列) を持つテーブルで、フィールドを直接書き換えることでフレームを編集できます (`dlc` は `len` から自動的に再計算されるため、書き換えても反映されません)。戻り値で転送先を決めます。
  - `nil` / `false`: ドロップ
  - `"channel名"`: (編集後の) フレームをそのチャネル1つへ転送
  - `{ "ch1", "ch2", ... }`: 同じ編集後フレームを複数チャネルへファンアウト
  - `{ {channel="ch1", frame=f1}, "ch2", ... }`: チャネル名の文字列と `{channel=, frame=}` テーブルを混在させられます。後者を使うと、宛先ごとに異なる内容のフレームを送ったり、同じチャネルへ複数回(別内容で)送ったりできます。
- `finalize()`: チャネル停止時 (Ctrl+C、または受信元デーモンとの切断) に一度だけ呼ばれます。エラーはログに出るだけで処理には影響しません (すでにシャットダウン中のため)。

転送先チャネルの TX パイプは、実際に転送が発生した時点で遅延接続されます。対応するデーモンが起動していない転送先はフレームをドロップし、3秒ごとに再接続を試みます (他の転送先への配信には影響しません)。

サンプルスクリプトは `examples/gateway_example.lua` にあります。

## 動作要件

Windows 専用です (名前付きパイプ / Win32 API に依存しているため)。SLCAN プロトコルに対応した COM ポート接続の CAN/CAN FD アダプタ (Lawicel CANUSB, CANable など) が別途必要です (ただし `slcd.exe --vcan` を使う場合はアダプタ不要)。
