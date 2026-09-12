# SessionNegotiator (main_app) 詳細設計書

## 1. モジュール概要
* **ファイル**: `src/app/main_app/network_wrapper/SessionNegotiator.cpp`, `src/include/cccaster/main_app/network_wrapper/SessionNegotiator.hpp`
* **機能**: C++ アプリケーション（CLI層）から実際のゲーム・ネットワークレイヤーへの橋渡しを行います。**（コア設計）**相手との通信を一旦ここで確立し、ラウンド開始前に通信品質（Ping/Loss等）の事前評価とレイテンシ測定を行うためのネゴシエータ機構です。測定後にこの一時セッションは破棄され、ランチャー起動後のゲーム（DLL）側が同じIP/Portで通信を再開します。

## 2. 構造定義と責務

### `class SessionNegotiator`
* **機能**: IPv4 / IPv6 別の自己IP取得、入力文字列の解析、および実際の UDP ソケット（`cccaster::network::UdpSocket`）を用いたハンドシェイク通信を行います。
* **ビジネス変数 (引数/ローカル)**:
  * `bool isIpv6`: IPv6 ネットワークを対象とするかどうかの判定フラグ。
  * `bool isHost`: ローカル（自分自身）がサーバーとして着信を待ち受ける役割か、クライアントとして接続を試みる役割かを判定するフラグ。ターゲットとなる UDP ポートバインドに影響します。
  * `std::string targetIp`: クライアントとして接続を試みる際の対象IP。
  * `uint16_t port`: 通信に使用する UDP ポート番号（通常 `10800`）。

## 3. 主要メソッド詳解

#### `GetGlobalIp(bool isIpv6)`
* `InternetOpenUrlA` API 等を利用し、外部サービス（`api.ipify.org` 等）へアクセスして自マシンのグローバル IP アドレス（IPv4/IPv6）を取得・返却します。ホストが相手にIPを伝える際に使用されます。

#### `ParseAddressAndPort`
* ユーザーがコンソールへ入力した手打ち文字列（例 `10800` あるいは `192.168.1.5:10800` など）を解析し、セパレーター（`:` や `[]`）で分割してIPアドレスとポート番号の構造データとして分離します。
* `isIpv6` モードに基づくフォーマット検証および、数字だけであればHostとしてのポート指定と判断する、といったビジネスロジックを含みます。

#### `RunNegotiation`
* **UDP ハンドシェイクの心臓部**です。
* `cccaster::network::UdpSocket` をバインドし（ホストなら指定ポート、クライアントなら `0` で任意ポート）、非同期コールバック `socket.OnReceive` を設定します。
* 内部で約 60FPS（`sleep_for(16ms)`）の通信ループを回し、自身から相手に向けてタイムスタンプ入りのパケット（26バイト）を投げ続けます。
* **レイテンシ測定**: 相手からパケットが返送された際の時刻差分（RTT）から、PING（ミリ秒）およびジッター（ばらつき）を計算し続け、`ConsoleRenderer` に対して描画指令を出します。
* 最終的に、ユーザーが「安定している・接続ロックイン」と判断し Enter キーを押下することで `lockedIn = true` となり呼び出し元（MainController）へ制御を返します。
