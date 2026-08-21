# quiche-py

- より良い設計のためには破壊的変更を恐れないこと
- 公開 API は asyncio ベースの高レベル API のみを提供すること
  - 低レベル API や Sans I/O の公開はしない (内部実装は Sans-IO でよい)
- W3C WebTransport API (https://www.w3.org/TR/webtransport/) を意識して API を設計すること
- quiche の webtransport / quic / moqt を主対象とすること
- 性能は優先しない。asyncio のみでよい
- 最新ドラフトに準拠すること
- バージョンが 2026.0.0 の間は変更履歴を `CHANGES.md` に残さないこと
- バージョンが 2026.0.0 の間は Pull-Request やブランチを作らず develop にコミットしていくこと
  - 1 Issue 1 コミット 1 プッシュ

