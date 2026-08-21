# W3C WebTransport API 対応のテストを追加する

- Created: 2026-08-21
- Completed: {YYYY-MM-DD}
- Branch: feature/add-w3c-api-tests
- Polished: {YYYY-MM-DD}

## 目的

W3C WebTransport API 準拠に再設計された新 API (0037 / 0038 / 0039 / 0040 / 0041) のテストを追加し、旧 API 前提の既存テストを新 API に追従させる。

## 現状

既存テストは旧 API 前提である:

- `tests/test_async_client/`: `AsyncWebTransportClient` の旧 API (connect / accept_*_stream / send_datagram / receive_datagram) を検証
- `tests/test_webtransport_interop/`: webtransport-py との相互運用 (旧 API 前提)
- `tests/test_webtransport_client.py`: WebTransport クライアントの旧 API を検証
- `tests/test_sans_io/`: 低レベル Sans-I/O コアを検証 (0043 で非公開化されるため、高レベル API 経由のテストに移行するか判断が必要)

既存の test issue (0012 / 0013 / 0014) は旧 API 前提で、新 API への追従が必要。

## 設計方針

0035 (公開 API の W3C WebTransport API 準拠再設計) の完了条件に従い、新 API のテストを追加する。shiguredo-python スキルのテスト規約 (モック・スタブ禁止、テストコメント重視、テストログは日本語) に従う。

## 完了条件

- 新 API の主要機能がテストで検証されていること (ready / closed / draining / close / datagrams / create_*_stream / incoming_*_streams / WebTransportError / send_order / send_group)
- 旧 API 前提の既存テストが新 API に追従して通ること
- 全テストが通ること

## 解決方法

- 新 API のテストを追加する (0037 / 0038 / 0039 / 0040 / 0041 の各サブ issue で実装した機能のテスト)
- 既存 test issue (0012 / 0013 / 0014) の内容を新 API に追従させる:
  - 0012 (相互運用テスト): 新 API で再検証
  - 0013 (ストリーム制御テスト): 新 API のストリーム制御で再検証
  - 0014 (エラーパス・クローズテスト): 新 API のエラーパス (WebTransportError 等) で再検証
- `tests/test_sans_io/` の扱い (非公開化された低レベル API のテストをどうするか) を判断する
- テストのコメントを重視し、テストログは日本語にする
