# 受信 datagram キューが無制限に成長する

- Created: 2026-08-04
- Completed: {YYYY-MM-DD}
- Branch: feature/fix-unbounded-datagram-queue
- Polished: {YYYY-MM-DD}

## 目的

受信 datagram の蓄積キューに上限を設け、帯域いっぱいに datagram を送るサーバーによるメモリ枯渇（OOM）を防ぐ。

## 現状

`src/bindings/quiche_glue.cc` と `src/bindings/quiche_glue_wt.cc` の各 SessionVisitor の `OnDatagramReceived()` は受信 datagram を `std::deque<std::string>` へ無制限に `emplace_back()` する。Python 側が `receive_datagram()` を呼ぶまで蓄積され続け、上限も破棄ポリシーもない。

上流の設計は「datagram はコールバックで即時配信し、キューを持たない」前提（`web_transport.h` の `DatagramStats` のコメント: droppedIncoming が存在しないのは配信が即時だから）であり、無制限キューという別のメモリモデルを持ち込んでいる。DATAGRAM フレームにはフロー制御が効かないため、クライアント側とはいえサーバーは攻撃者になり得る。

## 設計方針

受信キューに件数または総バイト数の上限を設け、超過時のドロップ方針（古いものを破棄 / 新しいものを破棄）を定める。上限値と方針は実装コメントに明記する。

## 完了条件

キューが上限に達した後もメモリ使用量が一定に保たれ、datagram の受信処理が継続すること（ドロップが発生しても例外やクラッシュが起きないこと）。

## 解決方法

- SessionVisitor の受信キューに上限（例: 総バイト数で固定値）とドロップ方針を実装する
- ドロップが発生した場合の観測手段（統計カウンタ等）を検討する
- テスト: 上限を超えて datagram を受信した場合の挙動を確認するテストを追加
