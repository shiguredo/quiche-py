# HTTP/3 リクエストの重複ヘッダ名が黙って失われる

- Created: 2026-08-04
- Completed: {YYYY-MM-DD}
- Branch: feature/fix-duplicate-header-loss
- Polished: {YYYY-MM-DD}

## 目的

同一フィールド名のヘッダが複数ある場合に、値を失わずに送信する（または明示的なエラーにする）。

## 現状

`src/bindings/quiche_glue_http3.cc` の `SubmitRequest()` と `src/bindings/quiche_glue_wt.cc` の `ConnectSession()` は、ヘッダを `quiche::HttpHeaderBlock`（map）に挿入するため:

```cpp
block[name] = value;
```

同一フィールド名が 2 回以上あると最後の 1 つだけが送信され、残りは警告なしに消失する。`module.cpp` の `submit_request()` / `connect_session()` の検証は pair の形状（長さ 2、bytes 型）のみで、名前の重複を検出しない。

RFC 9110 では同一フィールド名の複数フィールド行は合法（Set-Cookie 等は重複が意味を持つ）であり、クライアントとしてデータ欠落になる。pyi の型（`list[tuple[bytes, bytes]]`）は重複を許すため型チェッカでも検出できない。

## 設計方針

重複ヘッダを検出してエラーにするか、QUICHE が受け付ける形で複数値として送信するかを決定する。

## 完了条件

- 重複ヘッダを渡した場合、データが黙って失われないこと（エラーになる、またはすべて送信される）

## 解決方法

- `module.cpp` のヘッダ検証で同一名の重複を検出して `ValueError` にする（最小の対応）
- または HTTP/3 のフィールド結合ルールに従い、QUICHE の `HttpHeaderBlock` がサポートする形で複数値を送る
- テスト: 重複ヘッダの送信テストを追加
